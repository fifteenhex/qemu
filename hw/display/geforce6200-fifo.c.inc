/*
 * GeForce 6200 (NV44A) functional FIFO emulation
 *
 * Included from geforce6200.c.  Implements a functional model of the
 * PFIFO DMA pusher/puller: command fetch through ctxdma objects (with
 * nv04 GART page-table walks), RAMHT object resolution, host methods
 * (REF_CNT, semaphores), the software-method CACHE_ERROR trap
 * protocol, and the M2MF copy engine.  Register layouts follow the
 * nouveau driver (Linux 7.2), which is the reference for this model.
 */

/* RAMIN layout created by nouveau's nv40 instmem */
#define NV_RAMIN_SIZE       (16 * MiB)
#define NV_RAMHT_BASE       0x10000
#define NV_RAMHT_SIZE       0x8000
#define NV_RAMFC_BASE       0x20000

#define NV_USERD_BASE       0xc00000
#define NV_USERD_CHAN_SIZE  0x1000

#define NV_PFIFO_INTR       0x002100
#define NV_PFIFO_INTR_EN    0x002140
#define NV_PFIFO_MODE       0x002504
#define NV_PFIFO_CACHE1_PUSH1    0x003204
#define NV_PFIFO_CACHE1_HASH     0x003258
#define NV_PFIFO_CACHE1_GET      0x003270
#define NV_PFIFO_CACHE1_PULL0    0x003250
#define NV_PFIFO_CACHE1_METHOD   0x090000

#define NV_PMC_INTR_FIFO    0x00000100

static uint8_t *nv_ramin_ptr(GeForce6200State *s)
{
    uint64_t vram_size = (uint64_t)s->vram_size_mb * MiB;

    return memory_region_get_ram_ptr(&s->vram) + vram_size - NV_RAMIN_SIZE;
}

static uint32_t nv_ramin_rd32(GeForce6200State *s, uint32_t off)
{
    if (off + 4 > NV_RAMIN_SIZE) {
        return 0;
    }
    return nv_ld32(s, nv_ramin_ptr(s) + off);
}

/* ---------------------------------------------------------------- */
/* ctxdma objects (nv04 16-byte layout)                             */

typedef struct NVCtxDma {
    bool valid;
    bool linear;
    int target;         /* 0/1 vram, 2 pci, 3 pci nosnoop */
    uint32_t inst;      /* RAMIN offset of the object */
    uint32_t adjust;
    uint32_t limit;
    uint64_t base;      /* linear only */
} NVCtxDma;

static NVCtxDma nv_ctxdma_parse(GeForce6200State *s, uint32_t inst)
{
    NVCtxDma d = { .valid = false };
    uint32_t w0 = nv_ramin_rd32(s, inst);
    uint32_t w1 = nv_ramin_rd32(s, inst + 4);
    uint32_t w2 = nv_ramin_rd32(s, inst + 8);

    if (!w0) {
        return d;
    }
    d.valid = true;
    d.inst = inst;
    d.linear = w0 & 0x00002000;
    d.target = (w0 >> 16) & 3;
    d.adjust = (w0 >> 20) & 0xfff;
    d.limit = w1;
    d.base = w2 & ~0xfffULL;
    return d;
}

/*
 * Translate an offset inside a ctxdma to (vram offset | bus address).
 * Returns the number of contiguous bytes valid at *out (0 on fault).
 */
static uint32_t nv_ctxdma_xlate(GeForce6200State *s, NVCtxDma *d,
                                uint64_t addr, bool *is_vram, uint64_t *out)
{
    uint64_t eff = addr + d->adjust;

    if (!d->valid || addr > d->limit) {
        return 0;
    }

    if (d->linear) {
        *is_vram = d->target < 2;
        *out = d->base + eff;
        return 0x1000 - (eff & 0xfff);
    }

    /* paged: walk nv04 GART-style PTEs at inst + 8 */
    uint32_t pte = nv_ramin_rd32(s, d->inst + 8 + (eff >> 12) * 4);

    if (!(pte & 1)) {
        return 0;
    }
    *is_vram = false;   /* PTEs point at system pages */
    *out = (pte & ~0xfffULL) | (eff & 0xfff);
    return 0x1000 - (eff & 0xfff);
}

static bool nv_ctxdma_rw(GeForce6200State *s, NVCtxDma *d, uint64_t addr,
                         void *buf, uint32_t len, bool write)
{
    uint64_t vram_size = (uint64_t)s->vram_size_mb * MiB;
    uint8_t *p = buf;

    while (len) {
        bool is_vram;
        uint64_t out;
        uint32_t chunk = nv_ctxdma_xlate(s, d, addr, &is_vram, &out);

        chunk = MIN(chunk, len);
        if (!chunk) {
            return false;
        }
        if (is_vram) {
            if (out + chunk > vram_size) {
                return false;
            }
            if (write) {
                memcpy(memory_region_get_ram_ptr(&s->vram) + out, p, chunk);
                memory_region_set_dirty(&s->vram, out, chunk);
            } else {
                memcpy(p, memory_region_get_ram_ptr(&s->vram) + out, chunk);
            }
        } else {
            MemTxResult r;

            if (write) {
                r = pci_dma_write(PCI_DEVICE(s), out, p, chunk);
            } else {
                r = pci_dma_read(PCI_DEVICE(s), out, p, chunk);
            }
            if (r != MEMTX_OK) {
                return false;
            }
        }
        addr += chunk;
        p += chunk;
        len -= chunk;
    }
    return true;
}

static uint32_t nv_ctxdma_rd32(GeForce6200State *s, NVCtxDma *d, uint64_t addr)
{
    uint8_t buf[4] = { 0 };

    nv_ctxdma_rw(s, d, addr, buf, 4, false);
    return nv_ld32(s, buf);
}

static void nv_ctxdma_wr32(GeForce6200State *s, NVCtxDma *d, uint64_t addr,
                           uint32_t v)
{
    uint8_t buf[4];

    nv_st32(s, buf, v);
    nv_ctxdma_rw(s, d, addr, buf, 4, true);
}

/* ---------------------------------------------------------------- */
/* RAMHT                                                            */

/* returns context word, or 0 if not found */
static uint32_t nv_ramht_lookup(GeForce6200State *s, int chid, uint32_t handle)
{
    const int bits = 12;
    const uint32_t entries = NV_RAMHT_SIZE / 8;
    uint32_t hash = 0, h = handle;
    uint32_t i, slot;

    while (h) {
        hash ^= h & ((1 << bits) - 1);
        h >>= bits;
    }
    hash ^= chid << (bits - 4);

    slot = hash;
    for (i = 0; i < entries; i++) {
        uint32_t w0 = nv_ramin_rd32(s, NV_RAMHT_BASE + slot * 8);
        uint32_t w1 = nv_ramin_rd32(s, NV_RAMHT_BASE + slot * 8 + 4);

        if (w0 == handle && ((w1 >> 23) & 0x1f) == (uint32_t)chid) {
            return w1;
        }
        slot = (slot + 1) % entries;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* pusher                                                           */

static uint32_t nv_ramfc_off(int chid)
{
    return NV_RAMFC_BASE + chid * 128;
}

static void nv_chan_sync_ramfc(GeForce6200State *s, int chid)
{
    NVChan *c = &s->chan[chid];
    uint8_t *fc = nv_ramin_ptr(s) + nv_ramfc_off(chid);

    nv_st32(s, fc + 0x00, c->put);
    nv_st32(s, fc + 0x04, c->get);
    nv_st32(s, fc + 0x08, c->ref);
}

static void nv_chan_activate(GeForce6200State *s, int chid)
{
    NVChan *c = &s->chan[chid];
    uint8_t *fc = nv_ramin_ptr(s) + nv_ramfc_off(chid);

    c->get = nv_ld32(s, fc + 0x04);
    c->ref = nv_ld32(s, fc + 0x08);
    c->pb_inst = nv_ld32(s, fc + 0x0c) << 4;
    c->active = true;
}

static void nv_sw_trap(GeForce6200State *s, int chid, int subc,
                       uint32_t mthd, uint32_t data)
{
    s->trap_chid = chid;
    s->trap_acked = false;
    s->regs[NV_PFIFO_CACHE1_PUSH1 / 4] = chid;
    s->regs[NV_PFIFO_CACHE1_PULL0 / 4] |= 0x100;
    s->regs[NV_PFIFO_CACHE1_GET / 4] = 0;
    s->regs[NV_PFIFO_CACHE1_METHOD / 4] = (subc << 13) | mthd;
    s->regs[(NV_PFIFO_CACHE1_METHOD + 4) / 4] = data;
    if (mthd == 0) {
        /* kernel replaces data with the RAMHT-resolved instance */
        s->regs[NV_PFIFO_CACHE1_HASH / 4] = 4;
    }
    s->regs[NV_PFIFO_INTR / 4] |= 1;    /* CACHE_ERROR */
    nv_update_irq(s);
}

static void nv_m2mf_kick(GeForce6200State *s, NVChan *c)
{
    NVCtxDma src = nv_ctxdma_parse(s, c->m2mf.src_inst);
    NVCtxDma dst = nv_ctxdma_parse(s, c->m2mf.dst_inst);
    uint32_t line;
    g_autofree uint8_t *buf = NULL;

    if (!src.valid || !dst.valid || !c->m2mf.line_len || !c->m2mf.line_count) {
        return;
    }
    if (c->m2mf.line_len > 0x100000) {
        return;
    }

    buf = g_malloc(c->m2mf.line_len);
    for (line = 0; line < c->m2mf.line_count; line++) {
        if (!nv_ctxdma_rw(s, &src,
                          c->m2mf.off_in + (uint64_t)line * c->m2mf.pitch_in,
                          buf, c->m2mf.line_len, false) ||
            !nv_ctxdma_rw(s, &dst,
                          c->m2mf.off_out + (uint64_t)line * c->m2mf.pitch_out,
                          buf, c->m2mf.line_len, true)) {
            qemu_log_mask(LOG_GUEST_ERROR, "nv44 m2mf: copy fault\n");
            break;
        }
    }

    if (c->m2mf.notify_inst) {
        NVCtxDma n = nv_ctxdma_parse(s, c->m2mf.notify_inst);
        /* 16-byte notifier record: timestamp lo/hi, error, status(0=done) */
        nv_ctxdma_wr32(s, &n, 0x0, 0);
        nv_ctxdma_wr32(s, &n, 0x4, 0);
        nv_ctxdma_wr32(s, &n, 0x8, 0);
        nv_ctxdma_wr32(s, &n, 0xc, 0);
    }
}

static uint32_t nv_handle_to_inst(GeForce6200State *s, int chid,
                                  uint32_t handle)
{
    uint32_t ctx = nv_ramht_lookup(s, chid, handle);

    return (ctx & 0xfffff) << 4;
}

#include "geforce6200-3d.c.inc"

/* engine (GR-class) methods: M2MF and 3D are real, the rest accepted */
static void nv_gr_mthd(GeForce6200State *s, int chid, int subc,
                       uint32_t mthd, uint32_t data)
{
    NVChan *c = &s->chan[chid];

    switch (c->subc_class[subc]) {
    case 0x0039:    /* M2MF */
        switch (mthd) {
        case 0x0180:
            c->m2mf.notify_inst = nv_handle_to_inst(s, chid, data);
            break;
        case 0x0184:
            c->m2mf.src_inst = nv_handle_to_inst(s, chid, data);
            break;
        case 0x0188:
            c->m2mf.dst_inst = nv_handle_to_inst(s, chid, data);
            break;
        case 0x030c: c->m2mf.off_in = data; break;
        case 0x0310: c->m2mf.off_out = data; break;
        case 0x0314: c->m2mf.pitch_in = data; break;
        case 0x0318: c->m2mf.pitch_out = data; break;
        case 0x031c: c->m2mf.line_len = data; break;
        case 0x0320: c->m2mf.line_count = data; break;
        case 0x0324: c->m2mf.format = data; break;
        case 0x0328:
            nv_m2mf_kick(s, c);
            break;
        }
        break;
    case 0x4497:    /* curie 3D */
        nv_3d_mthd(s, chid, mthd, data);
        break;
    default:
        /* remaining 2D classes: state accepted, not needed by kmscube */
        break;
    }
}

/*
 * Dispatch one (subc, mthd, data).  Returns false if the pusher must
 * pause (software-method trap or semaphore acquire stall).
 */
static bool nv_fifo_mthd(GeForce6200State *s, int chid, int subc,
                         uint32_t mthd, uint32_t data)
{
    NVChan *c = &s->chan[chid];

    /* host (puller) methods */
    switch (mthd) {
    case 0x0000: {  /* SET_OBJECT */
        uint32_t ctx = nv_ramht_lookup(s, chid, data);
        int engine = (ctx >> 20) & 7;

        if (!ctx || engine == 0) {
            /* software object (or unknown): raise the sw trap */
            c->subc_engine[subc] = 0;
            nv_sw_trap(s, chid, subc, mthd, data);
            return false;
        }
        c->subc_engine[subc] = engine;
        c->subc_inst[subc] = (ctx & 0xfffff) << 4;
        c->subc_class[subc] = nv_ramin_rd32(s, c->subc_inst[subc]) & 0xffff;
        return true;
    }
    case 0x0050:    /* REF_CNT */
        c->ref = data;
        nv_chan_sync_ramfc(s, chid);
        return true;
    case 0x0060:    /* SET_CONTEXT_DMA_SEMAPHORE */
        c->sema_inst = nv_handle_to_inst(s, chid, data);
        return true;
    case 0x0064:    /* SEMAPHORE_OFFSET */
        c->sema_off = data;
        return true;
    case 0x0068: {  /* SEMAPHORE_ACQUIRE */
        NVCtxDma d = nv_ctxdma_parse(s, c->sema_inst);

        if (nv_ctxdma_rd32(s, &d, c->sema_off) == data) {
            return true;
        }
        c->blocked = true;
        c->acq_val = data;
        return false;
    }
    case 0x006c: {  /* SEMAPHORE_RELEASE */
        NVCtxDma d = nv_ctxdma_parse(s, c->sema_inst);
        int i;

        nv_ctxdma_wr32(s, &d, c->sema_off, data);
        /* wake channels stalled on acquire */
        for (i = 0; i < NV_FIFO_CHANNELS; i++) {
            s->chan[i].blocked = false;
        }
        return true;
    }
    default:
        break;
    }

    if (c->subc_engine[subc] == 0) {
        nv_sw_trap(s, chid, subc, mthd, data);
        return false;
    }

    nv_gr_mthd(s, chid, subc, mthd, data);
    return true;
}

static void nv_fifo_run_chan(GeForce6200State *s, int chid)
{
    NVChan *c = &s->chan[chid];
    NVCtxDma pb;
    uint32_t budget = 4 * 1024 * 1024;

    if (!c->active || c->blocked || s->trap_chid == chid) {
        return;
    }

    pb = nv_ctxdma_parse(s, c->pb_inst);
    if (!pb.valid) {
        return;
    }

    while (c->get != c->put && budget--) {
        uint32_t w = nv_ctxdma_rd32(s, &pb, c->get);

        if (c->mcnt) {
            uint32_t mthd = c->mthd;

            c->mcnt--;
            if (!c->ni) {
                c->mthd += 4;
            }
            c->get += 4;
            if (!nv_fifo_mthd(s, chid, c->msubc, mthd, w)) {
                break;      /* trapped or blocked; get already advanced */
            }
            continue;
        }

        /* control words */
        if ((w & 3) == 2) {                     /* CALL */
            c->subr_ret = c->get + 4;
            c->in_subr = true;
            c->get = w & 0xfffffffc;
            continue;
        }
        if (w == 0x00020000 && c->in_subr) {    /* RETURN */
            c->get = c->subr_ret;
            c->in_subr = false;
            continue;
        }
        if ((w & 0xe0000003) == 0x20000000) {   /* JUMP (nv04) */
            c->get = w & 0x1ffffffc;
            continue;
        }

        switch ((w >> 29) & 7) {
        case 0:     /* increasing methods */
        case 2:     /* non-increasing methods */
            c->mcnt = (w >> 18) & 0x7ff;
            c->msubc = (w >> 13) & 7;
            c->mthd = w & 0x1ffc;
            c->ni = ((w >> 29) & 7) == 2;
            c->get += 4;
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "nv44 fifo: bad command %08x at %08x\n", w, c->get);
            c->get += 4;
            break;
        }
    }

    nv_chan_sync_ramfc(s, chid);
}

static void nv_fifo_poll(GeForce6200State *s)
{
    int i;

    for (i = 0; i < NV_FIFO_CHANNELS; i++) {
        if (s->chan[i].active) {
            nv_fifo_run_chan(s, i);
        }
    }
}

/* ---------------------------------------------------------------- */
/* MMIO hooks (called from nv_bar0_read/write)                      */

static bool nv_fifo_bar0_read(GeForce6200State *s, hwaddr addr, uint32_t *val)
{
    if (addr >= NV_USERD_BASE &&
        addr < NV_USERD_BASE + NV_FIFO_CHANNELS * NV_USERD_CHAN_SIZE) {
        int chid = (addr - NV_USERD_BASE) / NV_USERD_CHAN_SIZE;
        NVChan *c = &s->chan[chid];

        switch (addr & (NV_USERD_CHAN_SIZE - 1)) {
        case 0x40: *val = c->put; return true;
        case 0x44: *val = c->get; return true;
        case 0x48: *val = c->ref; return true;
        default:   *val = 0;      return true;
        }
    }
    return false;
}

static bool nv_fifo_bar0_write(GeForce6200State *s, hwaddr addr, uint32_t val)
{
    if (addr >= NV_USERD_BASE &&
        addr < NV_USERD_BASE + NV_FIFO_CHANNELS * NV_USERD_CHAN_SIZE) {
        int chid = (addr - NV_USERD_BASE) / NV_USERD_CHAN_SIZE;
        NVChan *c = &s->chan[chid];

        switch (addr & (NV_USERD_CHAN_SIZE - 1)) {
        case 0x40:
            if (!c->active) {
                nv_chan_activate(s, chid);
            }
            c->put = val;
            nv_fifo_run_chan(s, chid);
            break;
        case 0x44:
            c->get = val;
            break;
        case 0x48:
            c->ref = val;
            break;
        }
        return true;
    }

    switch (addr) {
    case NV_PFIFO_INTR:
        s->regs[NV_PFIFO_INTR / 4] &= ~val;     /* W1C */
        nv_update_irq(s);
        return true;
    case NV_PFIFO_INTR_EN:
        s->regs[NV_PFIFO_INTR_EN / 4] = val;
        nv_update_irq(s);
        return true;
    case NV_PFIFO_CACHE1_GET:
        s->regs[NV_PFIFO_CACHE1_GET / 4] = val;
        if (s->trap_chid >= 0) {
            s->trap_acked = true;
        }
        return true;
    case NV_PFIFO_CACHE1_PULL0:
        s->regs[NV_PFIFO_CACHE1_PULL0 / 4] = val;
        if ((val & 1) && s->trap_chid >= 0 && s->trap_acked) {
            int chid = s->trap_chid;

            s->trap_chid = -1;
            s->regs[NV_PFIFO_CACHE1_PUSH1 / 4] = 0x1f;
            nv_fifo_run_chan(s, chid);
        }
        return true;
    case NV_PFIFO_MODE: {
        uint32_t prev = s->regs[NV_PFIFO_MODE / 4];
        int i;

        s->regs[NV_PFIFO_MODE / 4] = val;
        /* channels whose DMA-mode bit was cleared are being torn down */
        for (i = 0; i < NV_FIFO_CHANNELS; i++) {
            if ((prev & (1u << i)) && !(val & (1u << i))) {
                g_free(s->chan[i].n3d);
                memset(&s->chan[i], 0, sizeof(s->chan[i]));
            }
        }
        return true;
    }
    default:
        return false;
    }
}
