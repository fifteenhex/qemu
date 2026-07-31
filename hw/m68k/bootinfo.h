/*
 * SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 *
 * Bootinfo tags from linux bootinfo.h and bootinfo-mac.h:
 * This is an easily parsable and extendable structure containing all
 * information to be passed from the bootstrap to the kernel
 *
 * This structure is copied right after the kernel by the bootstrap
 * routine.
 */

#ifndef HW_M68K_BOOTINFO_H
#define HW_M68K_BOOTINFO_H

#define BOOTINFO0(base, id) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, sizeof(struct bi_record)); \
        base += 2; \
    } while (0)

#define BOOTINFO1(base, id, value) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, sizeof(struct bi_record) + 4); \
        base += 2; \
        stl_be_p(base, value); \
        base += 4; \
    } while (0)

/*
 * A record holding a single byte (e.g. BI_AMIGA_VBLANK), padded so the
 * next record stays 4-byte aligned.
 */
#define BOOTINFOBYTE(base, id, value) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, sizeof(struct bi_record) + 4); \
        base += 2; \
        stb_p(base, value); \
        base += 1; \
        stb_p(base, 0); \
        base += 1; \
        stw_be_p(base, 0); \
        base += 2; \
    } while (0)

/*
 * A record holding a single 16-bit word (e.g. BI_AMIGA_SERPER), padded
 * so the next record stays 4-byte aligned.
 */
#define BOOTINFOWORD(base, id, value) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, sizeof(struct bi_record) + 4); \
        base += 2; \
        stw_be_p(base, value); \
        base += 2; \
        stw_be_p(base, 0); \
        base += 2; \
    } while (0)

#define BOOTINFO2(base, id, value1, value2) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, sizeof(struct bi_record) + 8); \
        base += 2; \
        stl_be_p(base, value1); \
        base += 4; \
        stl_be_p(base, value2); \
        base += 4; \
    } while (0)

#define BOOTINFOSTR(base, id, string) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, \
                 (sizeof(struct bi_record) + strlen(string) + \
                  1 /* null termination */ + 3 /* padding */) & ~3); \
        base += 2; \
        for (unsigned i_ = 0; string[i_]; i_++) { \
            stb_p(base++, string[i_]); \
        } \
        stb_p(base++, 0); \
        base = QEMU_ALIGN_PTR_UP(base, 4); \
    } while (0)

#define BOOTINFODATA(base, id, data, len) \
    do { \
        stw_be_p(base, id); \
        base += 2; \
        stw_be_p(base, \
                 (sizeof(struct bi_record) + len + \
                  2 /* length field */ + 3 /* padding */) & ~3); \
        base += 2; \
        stw_be_p(base, len); \
        base += 2; \
        for (unsigned i_ = 0; i_ < len; ++i_) { \
            stb_p(base++, data[i_]); \
        } \
        base = QEMU_ALIGN_PTR_UP(base, 4); \
    } while (0)
#endif
