/*
 * GeForce 6200 (NV44A) curie 3D class (0x4497) interpreter
 *
 * Included from geforce6200-fifo.c.inc.  A functional software
 * implementation of the method subset mesa's gallium nv30 driver
 * emits: render target setup, viewport/scissor, clears, VP40 vertex
 * program and FP40 fragment program interpretation, vertex array
 * fetch, triangle rasterization with perspective-correct varyings,
 * and the 3D fence method.  Method numbers and instruction encodings
 * follow mesa's nv30 driver and nvfx shader headers.
 */

#include <math.h>

#define NV3D_MAX_VP_INSN   544
#define NV3D_MAX_VP_CONST  468
#define NV3D_MAX_ATTR      16
#define NV3D_MAX_OUT       16

typedef struct { float v[4]; } NVVec4;

typedef struct NV3DVertex {
    NVVec4 pos;             /* clip space */
    NVVec4 out[NV3D_MAX_OUT];
    /* window space, filled at raster time */
    float x, y, z, rw;
} NV3DVertex;

typedef struct NV3DState {
    /* dma object instances */
    uint32_t dma_color0, dma_zeta, dma_vtxbuf0, dma_vtxbuf1;
    uint32_t dma_fence, dma_query, dma_notify;
    /* render target */
    uint32_t rt_horiz, rt_vert, rt_format, rt_enable;
    uint32_t color0_pitch, color0_offset, zeta_pitch, zeta_offset;
    /* viewport / scissor / clears */
    float vp_xlate[4], vp_scale[4];
    uint32_t vp_horiz, vp_vert;
    uint32_t scissor_h, scissor_v;
    uint32_t clear_color, clear_depth;
    float depth_near, depth_far;
    /* raster state */
    uint32_t cull_face, front_face;
    bool cull_enable;
    bool depth_test, depth_write;
    uint32_t depth_func;
    /* vertex arrays */
    uint32_t vtxfmt[NV3D_MAX_ATTR];
    uint32_t vtxbuf[NV3D_MAX_ATTR];
    /* vertex program */
    uint32_t vp_code[NV3D_MAX_VP_INSN][4];
    uint32_t vp_upload_ptr, vp_upload_buf[4], vp_upload_n;
    uint32_t vp_start;
    NVVec4 vp_const[NV3D_MAX_VP_CONST];
    uint32_t vp_const_id, vp_const_buf[4], vp_const_n;
    /* fragment program */
    uint32_t fp_addr;
    /* fence */
    uint32_t fence_offset;
    /* primitive assembly */
    uint32_t prim;
    int nverts;
    NV3DVertex vq[3];
    bool strip_odd;
    /* inline FIFO-vertex accumulator (VERTEX_DATA) */
    uint32_t vdata[64];
    int vdata_n;
} NV3DState;

static NV3DState *nv_3d_state(GeForce6200State *s, NVChan *c)
{
    if (!c->n3d) {
        c->n3d = g_new0(NV3DState, 1);
    }
    return c->n3d;
}

/* ---------------------------------------------------------------- */
/* small vector helpers                                             */

static inline float nv_sat(float f, bool sat)
{
    if (!sat) {
        return f;
    }
    return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
}

static inline NVVec4 nv_swz(NVVec4 in, uint32_t swz, bool neg, bool absol)
{
    NVVec4 r;
    int i;

    for (i = 0; i < 4; i++) {
        float f = in.v[(swz >> ((3 - i) * 2)) & 3];

        if (absol) {
            f = fabsf(f);
        }
        r.v[i] = neg ? -f : f;
    }
    return r;
}

/* ---------------------------------------------------------------- */
/* VP40 interpreter                                                 */

static NVVec4 nv_vp_src(NV3DState *st, uint32_t sel, bool absol,
                        const NVVec4 *inputs, const NVVec4 *temps,
                        uint32_t const_idx, uint32_t input_idx)
{
    NVVec4 zero = { { 0, 0, 0, 0 } };
    NVVec4 base;

    switch (sel & 3) {
    case 1: base = temps[(sel >> 2) & 0x1f]; break;
    case 2: base = inputs[input_idx & 0xf]; break;
    case 3: base = st->vp_const[const_idx % NV3D_MAX_VP_CONST]; break;
    default: base = zero; break;
    }
    return nv_swz(base, (sel >> 8) & 0xff, sel & 0x10000, absol);
}

static void nv_vp_exec(GeForce6200State *s, NV3DState *st,
                       const NVVec4 *inputs, NV3DVertex *vout)
{
    NVVec4 temps[32] = {};
    NVVec4 *outs = vout->out;
    uint32_t pc = st->vp_start;
    int steps;

    memset(vout->out, 0, sizeof(vout->out));

    for (steps = 0; steps < NV3D_MAX_VP_INSN; steps++, pc++) {
        if (pc >= NV3D_MAX_VP_INSN) {
            break;
        }
        uint32_t d0 = st->vp_code[pc][0], d1 = st->vp_code[pc][1];
        uint32_t d2 = st->vp_code[pc][2], d3 = st->vp_code[pc][3];
        uint32_t vec_op = (d1 >> 22) & 0x1f;
        uint32_t sca_op = (d1 >> 27) & 0x1f;
        uint32_t const_idx = (d1 >> 12) & 0x1ff;
        uint32_t input_idx = (d1 >> 8) & 0xf;
        uint32_t s0sel = ((d1 & 0xff) << 9) | (d2 >> 23);
        uint32_t s1sel = (d2 >> 6) & 0x1ffff;
        uint32_t s2sel = ((d2 & 0x3f) << 11) | (d3 >> 21);
        uint32_t vec_wm = (d3 >> 13) & 0xf;    /* X..W = bit3..bit0 */
        uint32_t sca_wm = (d3 >> 17) & 0xf;
        uint32_t dest = (d3 >> 2) & 0x1f;
        uint32_t vec_temp = (d0 >> 15) & 0x3f;
        uint32_t sca_temp = (d3 >> 7) & 0x1f;
        bool vec_result = d0 & (1u << 30);
        bool sca_result = d3 & (1u << 12);
        bool sat = d0 & (1u << 26);
        NVVec4 a = nv_vp_src(st, s0sel, d0 & (1u << 21), inputs, temps,
                             const_idx, input_idx);
        NVVec4 b = nv_vp_src(st, s1sel, d0 & (1u << 22), inputs, temps,
                             const_idx, input_idx);
        NVVec4 cc = nv_vp_src(st, s2sel, d0 & (1u << 23), inputs, temps,
                              const_idx, input_idx);
        NVVec4 r = { { 0, 0, 0, 0 } };
        bool vec_write = vec_op && (vec_wm != 0);
        int i;

        switch (vec_op) {
        case 0x01: r = a; break;                                /* MOV */
        case 0x02: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
        case 0x03: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + cc.v[i]; break;
        case 0x04: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + cc.v[i]; break;
        case 0x05: {                                            /* DP3 */
            float d = a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2];
            for (i = 0; i < 4; i++) r.v[i] = d;
            break;
        }
        case 0x06: {                                            /* DPH */
            float d = a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2] + b.v[3];
            for (i = 0; i < 4; i++) r.v[i] = d;
            break;
        }
        case 0x07: {                                            /* DP4 */
            float d = a.v[0]*b.v[0] + a.v[1]*b.v[1] +
                      a.v[2]*b.v[2] + a.v[3]*b.v[3];
            for (i = 0; i < 4; i++) r.v[i] = d;
            break;
        }
        case 0x09: for (i = 0; i < 4; i++) r.v[i] = MIN(a.v[i], b.v[i]); break;
        case 0x0a: for (i = 0; i < 4; i++) r.v[i] = MAX(a.v[i], b.v[i]); break;
        case 0x0b: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i]; break;
        case 0x0c: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i]; break;
        case 0x0e: for (i = 0; i < 4; i++) r.v[i] = a.v[i] - floorf(a.v[i]); break;
        case 0x0f: for (i = 0; i < 4; i++) r.v[i] = floorf(a.v[i]); break;
        case 0x10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] == b.v[i]; break;
        case 0x12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] > b.v[i]; break;
        case 0x13: for (i = 0; i < 4; i++) r.v[i] = a.v[i] <= b.v[i]; break;
        case 0x14: for (i = 0; i < 4; i++) r.v[i] = a.v[i] != b.v[i]; break;
        default: vec_write = false; break;
        }

        if (vec_write) {
            NVVec4 *dst = vec_result ? &outs[dest] :
                          vec_temp < 32 ? &temps[vec_temp] : NULL;

            if (dst) {
                for (i = 0; i < 4; i++) {
                    if (vec_wm & (8 >> i)) {
                        dst->v[i] = nv_sat(r.v[i], sat);
                    }
                }
            }
        }

        if (sca_op && sca_wm) {
            float x = cc.v[0], sr = 0.0f;
            bool ok = true;

            switch (sca_op) {
            case 0x01: sr = x; break;
            case 0x02: sr = x != 0.0f ? 1.0f / x : INFINITY; break;  /* RCP */
            case 0x03: sr = x != 0.0f ? 1.0f / x : INFINITY; break;  /* RCC */
            case 0x04: sr = x != 0.0f ? 1.0f / sqrtf(fabsf(x)) : INFINITY; break;
            case 0x05: sr = exp2f(floorf(x)); break;
            case 0x06: sr = (x != 0.0f) ? floorf(log2f(fabsf(x))) : 0.0f; break;
            case 0x0d: sr = (x > 0.0f) ? log2f(x) : -INFINITY; break;
            case 0x0e: sr = exp2f(x); break;
            case 0x0f: sr = sinf(x); break;
            case 0x10: sr = cosf(x); break;
            default: ok = false; break;
            }
            if (ok) {
                NVVec4 *dst = sca_result ? &outs[dest] : &temps[sca_temp];

                for (i = 0; i < 4; i++) {
                    if (sca_wm & (8 >> i)) {
                        dst->v[i] = nv_sat(sr, sat);
                    }
                }
            }
        }

        if (d3 & 1) {   /* LAST */
            break;
        }
    }

    vout->pos = outs[0];
}

/* ---------------------------------------------------------------- */
/* FP40 interpreter                                                 */

#define NV_FP_MAX_INSN 128

typedef struct NVFpProg {
    uint32_t code[NV_FP_MAX_INSN * 4];
    int words;
} NVFpProg;

static NVVec4 nv_fp_src(uint32_t sel, const NVVec4 *temps, NVVec4 input,
                        const uint32_t *imm)
{
    NVVec4 base = { { 0, 0, 0, 0 } };
    int i;

    switch (sel & 3) {
    case 0: base = temps[(sel >> 2) & 0x3f]; break;
    case 1: base = input; break;
    case 2:
        for (i = 0; i < 4; i++) {
            union { uint32_t u; float f; } u = { .u = imm[i] };
            base.v[i] = u.f;
        }
        break;
    }
    return nv_swz(base, (sel >> 9) & 0xff, sel & 0x20000, false);
}

/* returns final color */
static NVVec4 nv_fp_exec(const NVFpProg *fp, const NVVec4 *varyings)
{
    NVVec4 temps[64] = {};
    int pc = 0;

    while (pc + 4 <= fp->words && pc < NV_FP_MAX_INSN * 4) {
        const uint32_t *insn = &fp->code[pc];
        uint32_t d0 = insn[0], d1 = insn[1], d2 = insn[2], d3 = insn[3];
        uint32_t op = (d0 >> 24) & 0x3f;
        uint32_t wm = (d0 >> 9) & 0xf;      /* X..W = bit0..bit3 */
        uint32_t dreg = (d0 >> 1) & 0x3f;
        uint32_t input_idx = (d0 >> 13) & 0xf;
        bool sat = d0 & (1u << 31);
        bool no_dest = d0 & (1u << 30);
        NVVec4 input = varyings[input_idx & 0xf];
        bool has_imm = ((d1 & 3) == 2) || ((d2 & 3) == 2) || ((d3 & 3) == 2);
        const uint32_t *imm = insn + 4;
        NVVec4 a, b, cc, r = { { 0, 0, 0, 0 } };
        bool write = !no_dest;
        int i;

        if (has_imm && pc + 8 > fp->words) {
            break;
        }
        a = nv_fp_src(d1 & 0x3ffff, temps, input, imm);
        if (d1 & (1u << 29)) {
            for (i = 0; i < 4; i++) a.v[i] = fabsf(a.v[i]);
        }
        b = nv_fp_src(d2 & 0x3ffff, temps, input, imm);
        cc = nv_fp_src(d3 & 0x3ffff, temps, input, imm);

        switch (op) {
        case 0x00: write = false; break;
        case 0x01: r = a; break;
        case 0x02: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
        case 0x03: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i]; break;
        case 0x04: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + cc.v[i]; break;
        case 0x05: {
            float d = a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2];
            for (i = 0; i < 4; i++) r.v[i] = d;
            break;
        }
        case 0x06: {
            float d = a.v[0]*b.v[0] + a.v[1]*b.v[1] +
                      a.v[2]*b.v[2] + a.v[3]*b.v[3];
            for (i = 0; i < 4; i++) r.v[i] = d;
            break;
        }
        case 0x08: for (i = 0; i < 4; i++) r.v[i] = MIN(a.v[i], b.v[i]); break;
        case 0x09: for (i = 0; i < 4; i++) r.v[i] = MAX(a.v[i], b.v[i]); break;
        case 0x10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] - floorf(a.v[i]); break;
        case 0x11: for (i = 0; i < 4; i++) r.v[i] = floorf(a.v[i]); break;
        case 0x1a: r.v[0] = r.v[1] = r.v[2] = r.v[3] =
                       a.v[0] != 0.0f ? 1.0f / a.v[0] : INFINITY; break;
        default: write = false; break;
        }

        if (write) {
            for (i = 0; i < 4; i++) {
                if (wm & (1 << i)) {
                    temps[dreg].v[i] = nv_sat(r.v[i], sat);
                }
            }
        }

        pc += has_imm ? 8 : 4;
        if (d0 & 1) {
            break;
        }
    }
    return temps[0];
}

/* ---------------------------------------------------------------- */
/* rasterizer                                                       */

/* resolve the RT to a direct VRAM pointer (linear VRAM ctxdma only) */
static uint8_t *nv_3d_rt_ptr(GeForce6200State *s, NV3DState *st,
                             uint32_t *w, uint32_t *h)
{
    NVCtxDma d = nv_ctxdma_parse(s, st->dma_color0);
    uint64_t vram_size = (uint64_t)s->vram_size_mb * MiB;
    uint64_t off;

    *w = st->rt_horiz >> 16;
    *h = st->rt_vert >> 16;
    if (!d.valid || !d.linear || d.target >= 2 || !*w || !*h ||
        !st->color0_pitch) {
        return NULL;
    }
    off = d.base + d.adjust + st->color0_offset;
    if (off + (uint64_t)st->color0_pitch * *h > vram_size) {
        return NULL;
    }
    return (uint8_t *)memory_region_get_ram_ptr(&s->vram) + off;
}

static void nv_3d_clip_rect(NV3DState *st, uint32_t w, uint32_t h,
                            int *x0, int *y0, int *x1, int *y1)
{
    int sx = st->scissor_h & 0xffff, sw = st->scissor_h >> 16;
    int sy = st->scissor_v & 0xffff, sh = st->scissor_v >> 16;

    *x0 = MAX(0, sx);
    *y0 = MAX(0, sy);
    *x1 = MIN((int)w, sx + sw);
    *y1 = MIN((int)h, sy + sh);
}

static void nv_3d_clear(GeForce6200State *s, NV3DState *st, uint32_t buffers)
{
    uint32_t w, h;
    uint8_t *rt = nv_3d_rt_ptr(s, st, &w, &h);
    int x0, y0, x1, y1, x, y;

    if (!rt || !(buffers & 0xf0)) {
        return;
    }
    nv_3d_clip_rect(st, w, h, &x0, &y0, &x1, &y1);
    for (y = y0; y < y1; y++) {
        uint8_t *row = rt + (size_t)y * st->color0_pitch;

        for (x = x0; x < x1; x++) {
            nv_st32(s, row + x * 4, st->clear_color);
        }
    }
}

static void nv_3d_raster_tri(GeForce6200State *s, NV3DState *st,
                             NV3DVertex *v0, NV3DVertex *v1, NV3DVertex *v2,
                             const NVFpProg *fp)
{
    uint32_t w, h;
    uint8_t *rt = nv_3d_rt_ptr(s, st, &w, &h);
    float area;
    int x0, y0, x1, y1, x, y;

    if (!rt) {
        return;
    }

    area = (v1->x - v0->x) * (v2->y - v0->y) -
           (v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0.0f) {
        return;
    }

    /*
     * Face culling: with the viewport's negative y scale, GL CCW
     * front faces have negative signed area in window space.
     */
    if (st->cull_enable) {
        bool ccw_front = (st->front_face != 0x900);
        bool front = ccw_front ? (area < 0.0f) : (area > 0.0f);
        uint32_t cull = st->cull_face;

        if (cull == 0x408 ||                    /* FRONT_AND_BACK */
            (front && cull == 0x404) ||         /* FRONT */
            (!front && cull == 0x405)) {        /* BACK */
            return;
        }
    }

    nv_3d_clip_rect(st, w, h, &x0, &y0, &x1, &y1);
    x0 = MAX(x0, (int)floorf(MIN(v0->x, MIN(v1->x, v2->x))));
    y0 = MAX(y0, (int)floorf(MIN(v0->y, MIN(v1->y, v2->y))));
    x1 = MIN(x1, (int)ceilf(MAX(v0->x, MAX(v1->x, v2->x))) + 1);
    y1 = MIN(y1, (int)ceilf(MAX(v0->y, MAX(v1->y, v2->y))) + 1);

    for (y = y0; y < y1; y++) {
        uint8_t *row = rt + (size_t)y * st->color0_pitch;
        float py = y + 0.5f;

        for (x = x0; x < x1; x++) {
            float px = x + 0.5f;
            float w0 = (v1->x - px) * (v2->y - py) - (v2->x - px) * (v1->y - py);
            float w1 = (v2->x - px) * (v0->y - py) - (v0->x - px) * (v2->y - py);
            float w2 = (v0->x - px) * (v1->y - py) - (v1->x - px) * (v0->y - py);
            NVVec4 varyings[16];
            NVVec4 col;
            float rw;
            uint32_t pix;
            int vi, ci;

            if (area > 0.0f) {
                if (w0 < 0 || w1 < 0 || w2 < 0) {
                    continue;
                }
            } else {
                if (w0 > 0 || w1 > 0 || w2 > 0) {
                    continue;
                }
            }
            w0 /= area;
            w1 /= area;
            w2 /= area;

            /* perspective-correct interpolation */
            rw = w0 * v0->rw + w1 * v1->rw + w2 * v2->rw;
            if (rw == 0.0f) {
                continue;
            }
            /* FP varying slot: 1=COL0,2=COL1,4+n=TC(n); VP out 7+n=TC(n) */
            for (vi = 0; vi < 16; vi++) {
                int src = -1;

                switch (vi) {
                case 1: src = 1; break;                 /* COL0 */
                case 2: src = 2; break;                 /* COL1 */
                default:
                    if (vi >= 4 && vi < 14) {
                        src = 7 + (vi - 4);             /* TC0..9 */
                    }
                    break;
                }
                if (src < 0) {
                    memset(&varyings[vi], 0, sizeof(NVVec4));
                    continue;
                }
                for (ci = 0; ci < 4; ci++) {
                    float f = w0 * v0->out[src].v[ci] * v0->rw +
                              w1 * v1->out[src].v[ci] * v1->rw +
                              w2 * v2->out[src].v[ci] * v2->rw;
                    varyings[vi].v[ci] = f / rw;
                }
            }
            /* WPOS */
            varyings[0].v[0] = px;
            varyings[0].v[1] = py;
            varyings[0].v[2] = w0 * v0->z + w1 * v1->z + w2 * v2->z;
            varyings[0].v[3] = 1.0f;

            col = nv_fp_exec(fp, varyings);

            pix = ((uint32_t)(nv_sat(col.v[0], true) * 255.0f + 0.5f) << 16) |
                  ((uint32_t)(nv_sat(col.v[1], true) * 255.0f + 0.5f) << 8) |
                  ((uint32_t)(nv_sat(col.v[2], true) * 255.0f + 0.5f)) |
                  ((uint32_t)(nv_sat(col.v[3], true) * 255.0f + 0.5f) << 24);
            nv_st32(s, row + x * 4, pix);
        }
    }
}

/* ---------------------------------------------------------------- */
/* vertex fetch + primitive assembly                                */

static void nv_3d_fetch_vertex(GeForce6200State *s, NV3DState *st,
                               uint32_t idx, NVVec4 *attrs)
{
    int i;

    for (i = 0; i < NV3D_MAX_ATTR; i++) {
        uint32_t fmt = st->vtxfmt[i];
        uint32_t ncomp = (fmt >> 4) & 0xf;
        uint32_t type = fmt & 0xf;
        uint32_t stride = (fmt >> 8) & 0xff;
        uint32_t addr;
        NVCtxDma d;
        uint32_t raw[4] = { 0, 0, 0, 0 };
        int ci;

        attrs[i].v[0] = attrs[i].v[1] = attrs[i].v[2] = 0.0f;
        attrs[i].v[3] = 1.0f;
        if (!ncomp || type != 2) {      /* only float32 arrays */
            continue;
        }
        addr = st->vtxbuf[i] & 0x0fffffff;
        d = nv_ctxdma_parse(s, (st->vtxbuf[i] & 0x80000000) ?
                            st->dma_vtxbuf1 : st->dma_vtxbuf0);
        if (!nv_ctxdma_rw(s, &d, addr + (uint64_t)idx * stride, raw,
                          MIN(ncomp, 4u) * 4, false)) {
            continue;
        }
        for (ci = 0; ci < (int)MIN(ncomp, 4u); ci++) {
            union { uint32_t u; float f; } u = { .u = nv_ld32(s, &raw[ci]) };
            attrs[i].v[ci] = u.f;
        }
    }
}

/*
 * Run one vertex (given its attributes) through the vertex program,
 * assemble primitives and rasterize completed triangles.  Shared by
 * the array-fetch path (VB_VERTEX_BATCH) and the inline FIFO-vertex
 * path (VERTEX_DATA), which mesa uses on big-endian hosts.
 */
static void nv_3d_process_vertex(GeForce6200State *s, NV3DState *st,
                                 const NVVec4 *attrs)
{
    NV3DVertex vert;
    NVFpProg fp;
    NVCtxDma fpdma;
    NV3DVertex *tri[3] = { NULL, NULL, NULL };
    bool emit = false;
    int i;

    nv_vp_exec(s, st, attrs, &vert);

    /* clip-space -> window (crude near-plane reject only) */
    if (vert.pos.v[3] <= 0.0f) {
        vert.rw = 0.0f;
    } else {
        float iw = 1.0f / vert.pos.v[3];

        vert.rw = iw;
        vert.x = st->vp_xlate[0] + st->vp_scale[0] * vert.pos.v[0] * iw;
        vert.y = st->vp_xlate[1] + st->vp_scale[1] * vert.pos.v[1] * iw;
        vert.z = st->vp_xlate[2] + st->vp_scale[2] * vert.pos.v[2] * iw;
    }

    switch (st->prim) {
    case 5:     /* TRIANGLES */
        st->vq[st->nverts++] = vert;
        if (st->nverts == 3) {
            tri[0] = &st->vq[0]; tri[1] = &st->vq[1]; tri[2] = &st->vq[2];
            emit = true;
            st->nverts = 0;
        }
        break;
    case 6:     /* TRIANGLE_STRIP */
        if (st->nverts < 3) {
            st->vq[st->nverts++] = vert;
            if (st->nverts == 3) {
                tri[0] = &st->vq[0]; tri[1] = &st->vq[1]; tri[2] = &st->vq[2];
                emit = true;
                st->strip_odd = true;
            }
        } else {
            st->vq[0] = st->vq[1];
            st->vq[1] = st->vq[2];
            st->vq[2] = vert;
            /* keep winding consistent across the strip */
            if (st->strip_odd) {
                tri[0] = &st->vq[1]; tri[1] = &st->vq[0]; tri[2] = &st->vq[2];
            } else {
                tri[0] = &st->vq[0]; tri[1] = &st->vq[1]; tri[2] = &st->vq[2];
            }
            st->strip_odd = !st->strip_odd;
            emit = true;
        }
        break;
    case 7:     /* TRIANGLE_FAN */
        if (st->nverts < 3) {
            st->vq[st->nverts++] = vert;
            emit = st->nverts == 3;
            if (emit) {
                tri[0] = &st->vq[0]; tri[1] = &st->vq[1]; tri[2] = &st->vq[2];
            }
        } else {
            st->vq[1] = st->vq[2];
            st->vq[2] = vert;
            tri[0] = &st->vq[0]; tri[1] = &st->vq[1]; tri[2] = &st->vq[2];
            emit = true;
        }
        break;
    default:
        return;
    }

    if (!emit || tri[0]->rw == 0.0f || tri[1]->rw == 0.0f ||
        tri[2]->rw == 0.0f) {
        return;
    }

    /* fetch the fragment program from memory */
    memset(&fp, 0, sizeof(fp));
    fpdma = nv_ctxdma_parse(s, (st->fp_addr & 2) ? st->dma_vtxbuf1 :
                            st->dma_color0);
    if (nv_ctxdma_rw(s, &fpdma, st->fp_addr & ~3ULL, fp.code,
                     sizeof(fp.code), false)) {
        fp.words = NV_FP_MAX_INSN * 4;
        /*
         * Decode the FP words in the card's byte order.  On a big-
         * endian host mesa additionally stores each word with its two
         * 16-bit halves swapped (nv30_fragprog.c), matching how the FP
         * engine fetches code in big-endian mode; undo that here.  The
         * appended immediate constants get the same treatment, which
         * is what the FP engine expects.
         */
        for (i = 0; i < fp.words; i++) {
            uint32_t w = nv_ld32(s, &fp.code[i]);

            fp.code[i] = s->be ? (w >> 16) | (w << 16) : w;
        }
    }

    nv_3d_raster_tri(s, st, tri[0], tri[1], tri[2], &fp);
}

static void nv_3d_emit_vertex(GeForce6200State *s, NV3DState *st,
                              uint32_t idx)
{
    NVVec4 attrs[NV3D_MAX_ATTR];

    nv_3d_fetch_vertex(s, st, idx, attrs);
    nv_3d_process_vertex(s, st, attrs);
}

/*
 * Inline FIFO vertices (NV30_3D VERTEX_DATA, 0x1818).  On big-endian
 * hosts mesa does not use hardware vertex-array fetch; it streams the
 * vertex attributes straight through the pushbuf, vertex_words per
 * vertex, laid out attribute-by-attribute per the current VTXFMT.
 * The words arrive here as ordinary method data - already decoded to
 * logical values by the be-aware pusher - so no further byte-swapping
 * is needed.
 */
static void nv_3d_vertex_data(GeForce6200State *s, NV3DState *st,
                              uint32_t data)
{
    uint32_t vwords = 0;
    int i;

    for (i = 0; i < NV3D_MAX_ATTR; i++) {
        if ((st->vtxfmt[i] & 0xf) == 2) {       /* float32 attribute */
            vwords += (st->vtxfmt[i] >> 4) & 0xf;
        }
    }
    if (!vwords || vwords > ARRAY_SIZE(st->vdata)) {
        return;
    }

    st->vdata[st->vdata_n++] = data;
    if (st->vdata_n < (int)vwords) {
        return;
    }

    {
        NVVec4 attrs[NV3D_MAX_ATTR];
        uint32_t w = 0;

        for (i = 0; i < NV3D_MAX_ATTR; i++) {
            uint32_t nc = ((st->vtxfmt[i] & 0xf) == 2) ?
                          ((st->vtxfmt[i] >> 4) & 0xf) : 0;
            int ci;

            attrs[i].v[0] = attrs[i].v[1] = attrs[i].v[2] = 0.0f;
            attrs[i].v[3] = 1.0f;
            for (ci = 0; ci < (int)MIN(nc, 4u); ci++) {
                union { uint32_t u; float f; } u = { .u = st->vdata[w + ci] };
                attrs[i].v[ci] = u.f;
            }
            w += nc;
        }
        st->vdata_n = 0;
        nv_3d_process_vertex(s, st, attrs);
    }
}

/* ---------------------------------------------------------------- */
/* method dispatch                                                  */

static void nv_3d_mthd(GeForce6200State *s, int chid, uint32_t mthd,
                       uint32_t data)
{
    NVChan *c = &s->chan[chid];
    NV3DState *st = nv_3d_state(s, c);
    union { uint32_t u; float f; } uf = { .u = data };

    switch (mthd) {
    case 0x0180: st->dma_notify = nv_handle_to_inst(s, chid, data); break;
    case 0x0194: st->dma_color0 = nv_handle_to_inst(s, chid, data); break;
    case 0x0198: st->dma_zeta = nv_handle_to_inst(s, chid, data); break;
    case 0x019c: st->dma_vtxbuf0 = nv_handle_to_inst(s, chid, data); break;
    case 0x01a0: st->dma_vtxbuf1 = nv_handle_to_inst(s, chid, data); break;
    case 0x01a4: st->dma_fence = nv_handle_to_inst(s, chid, data); break;
    case 0x01a8: st->dma_query = nv_handle_to_inst(s, chid, data); break;

    case 0x0200: st->rt_horiz = data; break;
    case 0x0204: st->rt_vert = data; break;
    case 0x0208: st->rt_format = data; break;
    case 0x020c: st->color0_pitch = data; break;
    case 0x0210: st->color0_offset = data; break;
    case 0x0214: st->zeta_offset = data; break;
    case 0x0220: st->rt_enable = data; break;
    case 0x022c: st->zeta_pitch = data; break;

    case 0x0394: st->depth_near = uf.f; break;
    case 0x0398: st->depth_far = uf.f; break;
    case 0x08c0: st->scissor_h = data; break;
    case 0x08c4: st->scissor_v = data; break;
    case 0x0a00: st->vp_horiz = data; break;
    case 0x0a04: st->vp_vert = data; break;
    case 0x0a20: case 0x0a24: case 0x0a28: case 0x0a2c:
        st->vp_xlate[(mthd - 0x0a20) / 4] = uf.f;
        break;
    case 0x0a30: case 0x0a34: case 0x0a38: case 0x0a3c:
        st->vp_scale[(mthd - 0x0a30) / 4] = uf.f;
        break;

    case 0x0a6c: st->depth_func = data; break;
    case 0x0a70: st->depth_write = data; break;
    case 0x0a74: st->depth_test = data; break;
    case 0x1830: st->cull_face = data; break;
    case 0x1834: st->front_face = data; break;
    case 0x183c: st->cull_enable = data; break;

    case 0x08e4: st->fp_addr = data; break;

    case 0x1d6c: st->fence_offset = data; break;
    case 0x1d70: {  /* FENCE_VALUE: write through DMA_FENCE */
        NVCtxDma d = nv_ctxdma_parse(s, st->dma_fence);

        nv_ctxdma_wr32(s, &d, st->fence_offset, data);
        break;
    }

    case 0x1d8c: st->clear_depth = data; break;
    case 0x1d90: st->clear_color = data; break;
    case 0x1d94: nv_3d_clear(s, st, data); break;

    case 0x1e9c: st->vp_upload_ptr = data; st->vp_upload_n = 0; break;
    case 0x0b80: case 0x0b84: case 0x0b88: case 0x0b8c:
        st->vp_upload_buf[(mthd - 0x0b80) / 4] = data;
        if (mthd == 0x0b8c && st->vp_upload_ptr < NV3D_MAX_VP_INSN) {
            memcpy(st->vp_code[st->vp_upload_ptr++], st->vp_upload_buf, 16);
        }
        break;
    case 0x1ea0: st->vp_start = data; break;
    case 0x1efc: st->vp_const_id = data; break;
    case 0x1f00: case 0x1f04: case 0x1f08: case 0x1f0c:
        st->vp_const_buf[(mthd - 0x1f00) / 4] = data;
        if (mthd == 0x1f0c && st->vp_const_id < NV3D_MAX_VP_CONST) {
            int i;

            for (i = 0; i < 4; i++) {
                union { uint32_t u; float f; } u2 = { .u = st->vp_const_buf[i] };
                st->vp_const[st->vp_const_id].v[i] = u2.f;
            }
            st->vp_const_id++;
        }
        break;

    case 0x1740 ... 0x177c:
        st->vtxfmt[(mthd - 0x1740) / 4] = data;
        break;
    case 0x1680 ... 0x16bc:
        st->vtxbuf[(mthd - 0x1680) / 4] = data;
        break;

    case 0x1808:    /* VERTEX_BEGIN_END */
        st->prim = data;
        st->nverts = 0;
        st->strip_odd = false;
        st->vdata_n = 0;
        break;
    case 0x1818:    /* VERTEX_DATA: inline FIFO vertex stream */
        nv_3d_vertex_data(s, st, data);
        break;
    case 0x1814: {  /* VB_VERTEX_BATCH */
        uint32_t first = data & 0xffffff;
        uint32_t count = (data >> 24) + 1;
        uint32_t i;

        for (i = 0; i < count; i++) {
            nv_3d_emit_vertex(s, st, first + i);
        }
        break;
    }
    case 0x180c: {  /* VB_ELEMENT_U16: two indices, low first */
        nv_3d_emit_vertex(s, st, data & 0xffff);
        nv_3d_emit_vertex(s, st, data >> 16);
        break;
    }
    case 0x1810:    /* VB_ELEMENT_U32 */
        nv_3d_emit_vertex(s, st, data);
        break;

    default:
        /* remaining state accepted and ignored */
        break;
    }
}
