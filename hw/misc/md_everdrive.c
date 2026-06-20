/*
 * QEMU Sega MegaDrive Everdrive SSF2 mapper workalike
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qom/object.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "qemu/fifo8.h"

#include <dirent.h>

#include "hw/misc/md_everdrive.h"

#define ED_REG_MAILBOX        0x00
#define ED_REG_MAILBOX_STAT   0x02
#define ED_REG_TIMER          0x06
#define ED_REG_MAPPER_CTRL0   0x20
#define ED_REG_MAPPER_CTRL7   0x2e

enum {
    ED_FIFO_HDR = 0,    /* collecting the 4-byte command frame  */
    ED_FIFO_USBWR_LEN,  /* USB_WRITE: 2-byte payload length      */
    ED_FIFO_USBWR_DATA, /* USB_WRITE: forwarding the payload     */
    ED_FIFO_ARG,        /* collecting fixed-size command args    */
    ED_FIFO_STR_LEN,    /* collecting a 2-byte string length     */
    ED_FIFO_STR_DATA,   /* collecting the string body            */
};

/* Mailbox command framing: { preamble, ~preamble, cmd, ~cmd } */
#define ED_CMD_PREAMBLE     0x2b
#define ED_CMD_STATUS       0x10
#define ED_CMD_USB_WRITE    0x22
#define ED_CMD_DISK_INIT    0xC0
#define ED_CMD_F_DIR_LD     0xC5
#define ED_CMD_F_DIR_SIZE   0xC6
#define ED_CMD_F_DIR_GET    0xC8
#define ED_CMD_F_FOPN       0xC9
#define ED_CMD_F_FRD        0xCA
#define ED_CMD_F_FCLOSE     0xCE
#define ED_CMD_F_FPTR       0xCF
#define ED_CMD_F_AVB        0xD5

#define ED_FMODE_READ       0x01

/* Status (offset 0x02): low 11 bits = RX FIFO fill level (FIFO_RXF_MSK). */
#define FIFO_RXF_MSK   0x7FF

#define ED_STATUS_OK   0x0000
#define ED_STATUS_ERR  0x00FF

/* ------------------------------------------------------------------ */
/*  Debug tracing (enable with -d unimp)                               */
/* ------------------------------------------------------------------ */

#define ED_DBG(fmt, ...) \
    qemu_log_mask(LOG_UNIMP, "md_everdrive: " fmt "\n", ## __VA_ARGS__)

static const char *ed_cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case ED_CMD_STATUS:     return "STATUS";
    case ED_CMD_USB_WRITE:  return "USB_WRITE";
    case ED_CMD_DISK_INIT:  return "DISK_INIT";
    case ED_CMD_F_DIR_LD:   return "F_DIR_LD";
    case ED_CMD_F_DIR_SIZE: return "F_DIR_SIZE";
    case ED_CMD_F_DIR_GET:  return "F_DIR_GET";
    case ED_CMD_F_FOPN:     return "F_FOPN";
    case ED_CMD_F_FRD:      return "F_FRD";
    case ED_CMD_F_FCLOSE:   return "F_FCLOSE";
    case ED_CMD_F_FPTR:     return "F_FPTR";
    case ED_CMD_F_AVB:      return "F_AVB";
    default:                return "?";
    }
}

/* Hex dump a small buffer into the log, capped to keep output sane. */
static void ed_dbg_hex(const char *tag, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    g_autoptr(GString) line = g_string_new(NULL);
    size_t i, shown = MIN(len, 32);

    for (i = 0; i < shown; i++) {
        g_string_append_printf(line, "%02x ", p[i]);
    }
    if (len > shown) {
        g_string_append_printf(line, "... (%zu total)", len);
    }
    ED_DBG("%s [%zu]: %s", tag, len, line->str);
}

/* ------------------------------------------------------------------ */
/*  RX FIFO (host -> guest)                                            */
/* ------------------------------------------------------------------ */

/* Command responses are staged here, then flushed to rx_fifo after a delay. */
static void md_everdrive_rx_push(MDEverdriveState *s, uint8_t b)
{
    if (fifo8_is_full(&s->pending_fifo)) {
        qemu_log_mask(LOG_GUEST_ERROR, "md_everdrive: response FIFO overflow\n");
        return;
    }
    fifo8_push(&s->pending_fifo, b);
}

static void md_everdrive_rx_push_buf(MDEverdriveState *s,
                                     const void *buf, size_t len)
{
    const uint8_t *p = buf;
    size_t i;

    for (i = 0; i < len; i++) {
        md_everdrive_rx_push(s, p[i]);
    }
}

/* Big-endian helpers, matching the m68k guest's byte order. */
static void md_everdrive_rx_push_u16(MDEverdriveState *s, uint16_t v)
{
    md_everdrive_rx_push(s, v >> 8);
    md_everdrive_rx_push(s, v & 0xFF);
}

static void md_everdrive_rx_push_u32(MDEverdriveState *s, uint32_t v)
{
    md_everdrive_rx_push_u16(s, v >> 16);
    md_everdrive_rx_push_u16(s, v & 0xFFFF);
}

static void md_everdrive_rx_push_u64(MDEverdriveState *s, uint64_t v)
{
    md_everdrive_rx_push_u32(s, v >> 32);
    md_everdrive_rx_push_u32(s, v & 0xFFFFFFFF);
}

static uint8_t md_everdrive_rx_pop(MDEverdriveState *s)
{
    if (fifo8_is_empty(&s->rx_fifo)) {
        return 0;
    }
    return fifo8_pop(&s->rx_fifo);
}

static int md_everdrive_can_receive(void *opaque)
{
    MDEverdriveState *s = MD_EVERDRIVE(opaque);

    return fifo8_num_free(&s->rx_fifo);
}

static void md_everdrive_receive(void *opaque, const uint8_t *buf, int size)
{
    MDEverdriveState *s = MD_EVERDRIVE(opaque);
    int i;

    for (i = 0; i < size; i++) {
        if (fifo8_is_full(&s->rx_fifo)) {
            break;
        }
        fifo8_push(&s->rx_fifo, buf[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  Argument helpers (big-endian decode of collected arg_buf)          */
/* ------------------------------------------------------------------ */

static uint16_t ed_arg_u16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

static uint32_t ed_arg_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* Resolve a guest path against the exported directory, rejecting escapes. */
static char *md_everdrive_resolve(MDEverdriveState *s, const char *rel)
{
    if (!s->disk_dir) {
        return NULL;
    }
    while (*rel == '/') {
        rel++;
    }
    if (strstr(rel, "..")) {
        return NULL;
    }
    if (*rel == '\0') {
        return g_strdup(s->disk_dir);
    }
    return g_strdup_printf("%s/%s", s->disk_dir, rel);
}

/* ------------------------------------------------------------------ */
/*  Disk / file command execution                                     */
/* ------------------------------------------------------------------ */

static void md_everdrive_dir_load(MDEverdriveState *s)
{
    /* arg_buf: [0] = flags, [1..] = path body (pathlen = arg_pos - 1) */
    g_autofree char *rel = g_strndup((char *)s->arg_buf + 1, s->arg_pos - 1);

    g_free(s->dir_path);
    s->dir_path = g_strdup(rel);

    ED_DBG("dir_load: path='%s'", rel);

    if (s->dir) {
        closedir(s->dir);
        s->dir = NULL;
    }

    s->status = ED_STATUS_OK;
}

static unsigned md_everdrive_dir_count(MDEverdriveState *s)
{
    g_autofree char *path = md_everdrive_resolve(s, s->dir_path ?: "");
    DIR *d;
    struct dirent *de;
    unsigned n = 0;

    if (!path || !(d = opendir(path))) {
        ED_DBG("dir_count: opendir('%s') failed: %s",
               path ? path : "(null)", strerror(errno));
        return 0;
    }
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
            n++;
        }
    }
    closedir(d);
    ED_DBG("dir_count: %u entries in '%s'", n, path);
    return n;
}

static void md_everdrive_dir_size(MDEverdriveState *s)
{
    md_everdrive_rx_push_u16(s, md_everdrive_dir_count(s));
    s->status = ED_STATUS_OK;
}

static void md_everdrive_dir_get(MDEverdriveState *s)
{
    /* arg_buf: u16 start, u16 num, u16 maxnamelen */
    uint16_t start = ed_arg_u16(s->arg_buf);
    uint16_t num   = ed_arg_u16(s->arg_buf + 2);
    g_autofree char *path = md_everdrive_resolve(s, s->dir_path ?: "");
    DIR *d;
    struct dirent *de;
    unsigned idx = 0, sent = 0;

    if (!path || !(d = opendir(path))) {
        md_everdrive_rx_push(s, 1);     /* terminate: no entries */
        return;
    }

    while ((de = readdir(d))) {
        struct stat st;
        g_autofree char *full = NULL;
        uint16_t namelen;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        if (idx++ < start) {
            continue;
        }
        if (sent >= num) {
            break;
        }

        full = g_strdup_printf("%s/%s", path, de->d_name);
        if (stat(full, &st) != 0) {
            continue;
        }

        md_everdrive_rx_push(s, 0);                 /* resp: ok */
        md_everdrive_rx_push_u32(s, (uint32_t)st.st_size);
        md_everdrive_rx_push_u16(s, 0);             /* date */
        md_everdrive_rx_push_u16(s, 0);             /* time */
        md_everdrive_rx_push(s, S_ISDIR(st.st_mode) ? 1 : 0);

        namelen = strlen(de->d_name);
        md_everdrive_rx_push_u16(s, namelen);
        md_everdrive_rx_push_buf(s, de->d_name, namelen);
        ED_DBG("dir_get: entry '%s' size=%ld dir=%d",
               de->d_name, (long)st.st_size, S_ISDIR(st.st_mode) ? 1 : 0);
        sent++;
    }
    closedir(d);

    md_everdrive_rx_push(s, 1);     /* terminate */
}

static void md_everdrive_file_open(MDEverdriveState *s)
{
    /* arg_buf: [0] = mode, [1..] = path body (pathlen = arg_pos - 1) */
    g_autofree char *rel = g_strndup((char *)s->arg_buf + 1, s->arg_pos - 1);
    g_autofree char *full = md_everdrive_resolve(s, rel);

    if (s->file_fd >= 0) {
        close(s->file_fd);
        s->file_fd = -1;
    }

    if (full) {
        s->file_fd = open(full, O_RDONLY | O_BINARY);
    }

    ED_DBG("file_open: '%s' -> fd=%d (%s)",
           full ? full : "(null)", s->file_fd,
           s->file_fd >= 0 ? "ok" : strerror(errno));

    s->status = s->file_fd >= 0 ? ED_STATUS_OK : ED_STATUS_ERR;
}

static void md_everdrive_file_avb(MDEverdriveState *s)
{
    uint64_t avb = 0;

    if (s->file_fd >= 0) {
        off_t cur = lseek(s->file_fd, 0, SEEK_CUR);
        off_t end = lseek(s->file_fd, 0, SEEK_END);

        lseek(s->file_fd, cur, SEEK_SET);
        /* not sure if the everdrive always returns the file size or the file size minus the current pos? */
        avb = end;
    }
    md_everdrive_rx_push_u64(s, avb);
    ED_DBG("file_avb: %" PRIu64 " bytes", avb);
}

static void md_everdrive_file_read(MDEverdriveState *s)
{
    /* arg_buf: u32 amount */
    uint32_t amount = ed_arg_u32(s->arg_buf);
    g_autofree uint8_t *buf = g_malloc0(amount);
    ssize_t got = 0;

    if (s->file_fd >= 0) {
        got = read(s->file_fd, buf, amount);
        if (got < 0) {
            got = 0;
        }
    }

    md_everdrive_rx_push(s, got == (ssize_t)amount ? 0 : 1);    /* resp */
    md_everdrive_rx_push_buf(s, buf, amount);
    ED_DBG("file_read: req=%u got=%zd", amount, got);
}

static void md_everdrive_file_close(MDEverdriveState *s)
{
    if (s->file_fd >= 0) {
        close(s->file_fd);
        s->file_fd = -1;
    }
    s->status = ED_STATUS_OK;
}

static void md_everdrive_file_fptr(MDEverdriveState *s)
{
    uint32_t ptr = ed_arg_u32(s->arg_buf);

    if (s->file_fd >= 0) {
        lseek(s->file_fd, ptr, SEEK_SET);
    }

    ED_DBG("file_fptr: req=%u", ptr);
}


/* ------------------------------------------------------------------ */
/*  Command frame dispatch                                             */
/* ------------------------------------------------------------------ */

/* Begin collecting "need" fixed bytes; execute() runs once complete. */
static void md_everdrive_collect(MDEverdriveState *s, uint32_t need)
{
    s->arg_need = need;
    s->arg_pos = 0;
    s->fifo_state = ED_FIFO_ARG;
}

static void md_everdrive_fifo_command(MDEverdriveState *s)
{
    uint32_t rx_before = fifo8_num_used(&s->pending_fifo);

    ED_DBG("CMD 0x%02x (%s)", s->cmd, ed_cmd_name(s->cmd));

    switch (s->cmd) {
    case ED_CMD_USB_WRITE:
        s->fifo_state = ED_FIFO_USBWR_LEN;
        s->len_pos = 0;
        s->data_len = 0;
        break;

    case ED_CMD_STATUS:
        md_everdrive_rx_push_u16(s, s->status);
        break;

    case ED_CMD_DISK_INIT:
        s->status = ED_STATUS_OK;
        break;

    case ED_CMD_F_DIR_SIZE:
        md_everdrive_dir_size(s);
        break;

    case ED_CMD_F_AVB:
        md_everdrive_file_avb(s);
        break;

    case ED_CMD_F_FCLOSE:
        md_everdrive_file_close(s);
        break;

    case ED_CMD_F_FPTR:
        md_everdrive_collect(s, 4);     /* u32 position */
        break;

    case ED_CMD_F_DIR_GET:
        md_everdrive_collect(s, 6);     /* u16 start, u16 num, u16 maxname */
        break;

    case ED_CMD_F_FRD:
        md_everdrive_collect(s, 4);     /* u32 amount */
        break;

    case ED_CMD_F_DIR_LD:
        /* u8 flags + u16 strlen + str: collect flags first, then string */
        md_everdrive_collect(s, 1);
        break;

    case ED_CMD_F_FOPN:
        md_everdrive_collect(s, 1);     /* u8 mode, then string */
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
            "md_everdrive: unhandled FIFO cmd 0x%02x\n", s->cmd);
        break;
    }

    if (fifo8_num_used(&s->pending_fifo) != rx_before) {
        ED_DBG("  queued %u response byte(s)", fifo8_num_used(&s->pending_fifo) - rx_before);
    }
}

/* Fixed-size arg block complete: act on it (some commands then read a str). */
static void md_everdrive_args_done(MDEverdriveState *s)
{
    uint32_t rx_before = fifo8_num_used(&s->pending_fifo);

    ed_dbg_hex("args", s->arg_buf, s->arg_pos);

    switch (s->cmd) {
    case ED_CMD_F_DIR_GET:
        md_everdrive_dir_get(s);
        s->fifo_state = ED_FIFO_HDR;
        break;

    case ED_CMD_F_FRD:
        md_everdrive_file_read(s);
        s->fifo_state = ED_FIFO_HDR;
        break;

    case ED_CMD_F_DIR_LD:
    case ED_CMD_F_FOPN:
        /* leading byte collected; now read the u16-prefixed string */
        s->fifo_state = ED_FIFO_STR_LEN;
        s->len_pos = 0;
        s->data_len = 0;
        break;

    case ED_CMD_F_FPTR:
	md_everdrive_file_fptr(s);
        s->fifo_state = ED_FIFO_HDR;
	break;

    default:
        s->fifo_state = ED_FIFO_HDR;
        break;
    }

    if (fifo8_num_used(&s->pending_fifo) != rx_before) {
        ED_DBG("  queued %u response byte(s)", fifo8_num_used(&s->pending_fifo) - rx_before);
    }
}

/* String body complete (appended after the fixed args in arg_buf). */
static void md_everdrive_str_done(MDEverdriveState *s)
{
    uint32_t rx_before = fifo8_num_used(&s->pending_fifo);

    ed_dbg_hex("str+args", s->arg_buf, s->arg_pos);

    switch (s->cmd) {
    case ED_CMD_F_DIR_LD:
        md_everdrive_dir_load(s);
        break;
    case ED_CMD_F_FOPN:
        md_everdrive_file_open(s);
        break;
    default:
        break;
    }
    s->fifo_state = ED_FIFO_HDR;

    if (fifo8_num_used(&s->pending_fifo) != rx_before) {
        ED_DBG("  queued %u response byte(s)", fifo8_num_used(&s->pending_fifo) - rx_before);
    }
}

/* ------------------------------------------------------------------ */
/*  Mailbox byte parser                                                */
/* ------------------------------------------------------------------ */

/* After the modelled latency, make the staged response visible to the guest. */
static void md_everdrive_resp_cb(void *opaque)
{
    MDEverdriveState *s = opaque;

    while (!fifo8_is_empty(&s->pending_fifo) && !fifo8_is_full(&s->rx_fifo)) {
        fifo8_push(&s->rx_fifo, fifo8_pop(&s->pending_fifo));
    }
}

static void md_everdrive_fifo_write(MDEverdriveState *s, uint8_t b)
{
    switch (s->fifo_state) {
    case ED_FIFO_HDR:
        switch (s->hdr_pos) {
        case 0:
            s->hdr_pos = (b == ED_CMD_PREAMBLE) ? 1 : 0;
            break;
        case 1:
            if (b == (uint8_t)~ED_CMD_PREAMBLE) {
                s->hdr_pos = 2;
            } else {
                s->hdr_pos = (b == ED_CMD_PREAMBLE) ? 1 : 0;
            }
            break;
        case 2:
            s->cmd = b;
            s->hdr_pos = 3;
            break;
        case 3:
            s->hdr_pos = 0;
            if (b == (uint8_t)~s->cmd) {
                md_everdrive_fifo_command(s);
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                    "md_everdrive: bad cmd complement for 0x%02x\n", s->cmd);
            }
            break;
        }
        break;

    case ED_FIFO_USBWR_LEN:
        s->data_len = (s->data_len << 8) | b;
        if (++s->len_pos == 2) {
            s->data_pos = 0;
            s->fifo_state = s->data_len ? ED_FIFO_USBWR_DATA : ED_FIFO_HDR;
        }
        break;

    case ED_FIFO_USBWR_DATA:
        qemu_chr_fe_write_all(&s->chr, &b, 1);
        ED_DBG("usbwr tx: 0x%02x '%c'", b, (b >= 0x20 && b < 0x7f) ? b : '.');
        if (++s->data_pos == s->data_len) {
            s->fifo_state = ED_FIFO_HDR;
        }
        break;

    case ED_FIFO_ARG:
        if (s->arg_pos < MD_EVERDRIVE_ARG_MAX) {
            s->arg_buf[s->arg_pos] = b;
        }
        if (++s->arg_pos == s->arg_need) {
            md_everdrive_args_done(s);
        }
        break;

    case ED_FIFO_STR_LEN:
        s->data_len = (s->data_len << 8) | b;
        if (++s->len_pos == 2) {
            /* string body is appended to arg_buf after the fixed args */
            s->arg_need = s->arg_pos + s->data_len;
            s->fifo_state = s->data_len ? ED_FIFO_STR_DATA : ED_FIFO_HDR;
            if (!s->data_len) {
                md_everdrive_str_done(s);
            }
        }
        break;

    case ED_FIFO_STR_DATA:
        if (s->arg_pos < MD_EVERDRIVE_ARG_MAX) {
            s->arg_buf[s->arg_pos] = b;
        }
        if (++s->arg_pos == s->arg_need) {
            md_everdrive_str_done(s);
        }
        break;
    }

    /* If a command staged a response, deliver it after the modelled delay. */
    if (!fifo8_is_empty(&s->pending_fifo) && !timer_pending(s->resp_timer)) {
        timer_mod(s->resp_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)s->resp_delay_us * SCALE_US);
    }
}

/* 1 kHz free-running counter, in milliseconds since timer_base. */
static uint16_t md_everdrive_timer_get(MDEverdriveState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return (uint16_t)((now - s->timer_base) / SCALE_MS);
}

static uint64_t md_everdrive_read(void *opaque, hwaddr offset, unsigned size)
{
    MDEverdriveState *s = MD_EVERDRIVE(opaque);

    switch (offset) {
    case ED_REG_MAILBOX: {
        uint8_t b = md_everdrive_rx_pop(s);
        qemu_chr_fe_accept_input(&s->chr);
        return b;
    }
    case ED_REG_MAILBOX_STAT:
        /* low 11 bits = RX fill level; TX always ready */
        return MIN(fifo8_num_used(&s->rx_fifo), FIFO_RXF_MSK);
    case ED_REG_TIMER:
        return md_everdrive_timer_get(s);
    default:
        qemu_log_mask(LOG_UNIMP,
            "md_everdrive: read at offset 0x%02" HWADDR_PRIx "\n", offset);
        return 0xFF;
    }
}

static void md_everdrive_write(void *opaque, hwaddr offset, uint64_t val,
                               unsigned size)
{
    MDEverdriveState *s = MD_EVERDRIVE(opaque);

    if (offset == ED_REG_MAILBOX) {
	if (fifo8_num_used(&s->rx_fifo))
        ED_DBG("write when fifo still has: 0x%02x", fifo8_num_used(&s->rx_fifo));

        md_everdrive_fifo_write(s, val & 0xFF);
        return;
    }

    if (offset == ED_REG_TIMER) {
        /* Rebase so the counter reads back the written value */
        s->timer_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                        (int64_t)(val & 0xFFFF) * SCALE_MS;
        return;
    }

    if (offset >= ED_REG_MAPPER_CTRL0 && offset <= ED_REG_MAPPER_CTRL7 &&
        !(offset & 1)) {
        unsigned slot = (offset - ED_REG_MAPPER_CTRL0) >> 1;

        s->bank[slot] = (uint8_t)(val & 0xFF);
        qemu_log_mask(LOG_UNIMP,
            "md_everdrive: slot %u ctrl <- 0x%04" PRIx64 "\n", slot, val);
        /* TODO: remap the corresponding alias in the cartridge MemoryRegion */
        return;
    }

    qemu_log_mask(LOG_UNIMP,
        "md_everdrive: write 0x%04" PRIx64 " at offset 0x%02" HWADDR_PRIx "\n",
        val, offset);
}

static const MemoryRegionOps md_everdrive_ops = {
    .read       = md_everdrive_read,
    .write      = md_everdrive_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

static const VMStateDescription vmstate_md_everdrive = {
    .name           = "md-everdrive",
    .version_id     = 9,
    .minimum_version_id = 9,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(fifo_state, MDEverdriveState),
        VMSTATE_UINT8(hdr_pos, MDEverdriveState),
        VMSTATE_UINT8(cmd, MDEverdriveState),
        VMSTATE_UINT8(len_pos, MDEverdriveState),
        VMSTATE_UINT32(data_len, MDEverdriveState),
        VMSTATE_UINT32(data_pos, MDEverdriveState),
        VMSTATE_UINT32(arg_need, MDEverdriveState),
        VMSTATE_UINT32(arg_pos, MDEverdriveState),
        VMSTATE_UINT8_ARRAY(arg_buf, MDEverdriveState, MD_EVERDRIVE_ARG_MAX),
        VMSTATE_INT64(timer_base, MDEverdriveState),
        VMSTATE_UINT16(status, MDEverdriveState),
        VMSTATE_FIFO8(rx_fifo, MDEverdriveState),
        VMSTATE_FIFO8(pending_fifo, MDEverdriveState),
        VMSTATE_TIMER_PTR(resp_timer, MDEverdriveState),
        VMSTATE_UINT8_ARRAY(bank, MDEverdriveState, MD_EVERDRIVE_NUM_BANKS),
        VMSTATE_END_OF_LIST()
    },
};

static void md_everdrive_realize(DeviceState *dev, Error **errp)
{
    MDEverdriveState *s   = MD_EVERDRIVE(dev);
    SysBusDevice     *sbd = SYS_BUS_DEVICE(dev);

    s->file_fd = -1;
    fifo8_create(&s->rx_fifo, MD_EVERDRIVE_RX_FIFO);
    fifo8_create(&s->pending_fifo, MD_EVERDRIVE_RX_FIFO);
    s->resp_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, md_everdrive_resp_cb, s);

    memory_region_init_io(&s->iomem, OBJECT(s), &md_everdrive_ops, s,
                          "md-everdrive", MD_EVERDRIVE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qemu_chr_fe_set_handlers(&s->chr, md_everdrive_can_receive,
                             md_everdrive_receive, NULL, NULL, s, NULL, true);
}

static void md_everdrive_reset(DeviceState *dev)
{
    MDEverdriveState *s = MD_EVERDRIVE(dev);
    unsigned i;

    s->fifo_state = ED_FIFO_HDR;
    s->hdr_pos = 0;
    s->cmd = 0;
    s->len_pos = 0;
    s->data_len = 0;
    s->data_pos = 0;
    s->arg_need = 0;
    s->arg_pos = 0;

    s->timer_base = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    fifo8_reset(&s->rx_fifo);
    fifo8_reset(&s->pending_fifo);
    if (s->resp_timer) {
        timer_del(s->resp_timer);
    }
    s->status = ED_STATUS_OK;

    if (s->dir) {
        closedir(s->dir);
        s->dir = NULL;
    }
    if (s->file_fd >= 0) {
        close(s->file_fd);
        s->file_fd = -1;
    }

    for (i = 0; i < MD_EVERDRIVE_NUM_BANKS; i++) {
        s->bank[i] = (uint8_t)i;
    }
}

static const Property md_everdrive_properties[] = {
    DEFINE_PROP_CHR("chardev", MDEverdriveState, chr),
    DEFINE_PROP_STRING("dir", MDEverdriveState, disk_dir),
    DEFINE_PROP_UINT32("resp-delay-us", MDEverdriveState, resp_delay_us, 1000000/4),
};

static void md_everdrive_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = md_everdrive_realize;
    dc->legacy_reset   = md_everdrive_reset;
    dc->vmsd    = &vmstate_md_everdrive;
    dc->desc    = "Sega MegaDrive Everdrive SSF2 mapper (skeleton)";
    device_class_set_props(dc, md_everdrive_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo md_everdrive_info = {
    .name          = TYPE_MD_EVERDRIVE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MDEverdriveState),
    .class_init    = md_everdrive_class_init,
};

static void md_everdrive_register_types(void)
{
    type_register_static(&md_everdrive_info);
}

type_init(md_everdrive_register_types)
