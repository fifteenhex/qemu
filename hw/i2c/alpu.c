/*
 * Neowine ALPU-FA i2c copy-protection / authentication chip
 *
 * A small i2c crypto chip boards fit to stop firmware being copied to
 * clones. The Miyoo Mini has one on i2c1 at address 0x3d; the vendor
 * 4.9 kernel and the MainUI app both run a challenge-response
 * handshake against it during init and abort if it does not answer
 * (the kernel dereferences NULL and takes PID 1 down, panicking with
 * "Attempted to kill init").
 *
 * The chip's crypto is modelled exactly, reverse-engineered from the
 * vendor kernel's own software verifier (auth code around 0xc01d0000,
 * the "transform" cipher at 0xc01d05a4 and its key tables at
 * 0xc0345264/0xc0345274). The verifier is symmetric and
 * deterministic: it recomputes what the chip should return and byte
 * compares, so the whole secret lives in the firmware and the chip
 * can be reproduced from it.
 *
 * Protocol (the register is the first byte of each i2c write; the
 * 7-bit address is 0x3d):
 *   wr 0x80/0x20/0x22   init/config, ignored
 *   rd 0x30 (16B)       chip nonce; the host derives a response and
 *                       writes it back (ignored). Marks the start of
 *                       an auth round, so the buf_ac counter reseeds.
 *   rd 0x73..0x76 (8B)  feed the host's buf_b0/buf_d8; return zeros so
 *                       both are all-zero in the host's transform.
 *   wr 0x40 (16B)       phase 3, ignored; bumps buf_ac by 2.
 *   {wr,rd} 0xe9 (16B)  auth round 0. The host writes arg0 in the even
 *   {wr,rd} 0x87 (16B)  auth round 1. bytes, reads back, extracts an
 *                       8-byte word W from bytes [0,1,4,5,8,9,12,13],
 *                       computes arg1 = transform(W) and requires
 *                       arg1 == arg0.
 *
 * So for an 0xe9/0x87 read we must return W = transform^-1(arg0). The
 * cipher's operation sequence depends only on the key tables and the
 * external buffers, never on its input, which makes transform a
 * composition of invertible byte operations - hence bijective and
 * invertible. We keep buf_b0 = buf_d8 = K3 = 0 (returning zeros for
 * the nonce and 0x73..76 reads and placing 0 in the K3 byte
 * positions), so only the deterministic buf_ac counter varies.
 *
 * Copyright (c) 2026 Daniel Palmer <daniel@thingy.jp>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/alpu.h"
#include "trace.h"

/* Key tables from the vendor kernel at 0xc0345264 and 0xc0345274 */
static const uint8_t ALPU_TBL0[16] = {
    0xe8, 0xf6, 0x78, 0x6a, 0x7b, 0x04, 0xf8, 0xd2,
    0xff, 0x1e, 0x6f, 0x82, 0x6c, 0xec, 0xef, 0x3a,
};
static const uint8_t ALPU_TBL1[16] = {
    0x9f, 0xb7, 0xfb, 0x2c, 0xc0, 0x36, 0x0a, 0x1e,
    0x09, 0x21, 0x6d, 0xba, 0x56, 0xa0, 0x9c, 0x88,
};

#define ALPU_W       0x20       /* transform "width" argument (rotations) */
#define ALPU_NROUNDS 2

typedef struct AlpuOp {
    uint8_t op;
    uint8_t s[8];
} AlpuOp;

/*
 * Replay the input-independent state evolution and record the ordered
 * list of sub-operations the transform dispatches onto its working
 * buffer. mode != 0 (the auth-round path) mixes in buf_b0/buf_d8;
 * mode 0 uses the raw table. Only op 1 (the XOR) needs its state
 * snapshot; 0 and 3 are pure permutations.
 */
static int alpu_plan(AlpuOp *ops, const uint8_t k3[2], int mode,
                     const uint8_t b0[8], const uint8_t d8[8],
                     const uint8_t ac[2])
{
    int n = 0;
    int rnd;

    for (rnd = 0; rnd < ALPU_NROUNDS; rnd++) {
        uint8_t s[8];
        int i, r;

        for (i = 0; i < 8; i++) {
            s[i] = mode == 0 ? ALPU_TBL0[rnd * 8 + i]
                             : (ALPU_TBL1[rnd * 8 + i] ^ b0[i] ^ d8[i]) & 0xff;
        }
        for (i = 0; i < 2; i++) {
            s[i] ^= k3[i];
        }
        for (i = 0; i < 2; i++) {
            s[i + 2] ^= ac[i];
        }
        for (r = 0; r < ALPU_W - 1; r++) {          /* rotate s left by 3 */
            uint8_t t = s[0];

            for (i = 0; i < 7; i++) {
                s[i] = ((s[i] << 3) | (s[i + 1] >> 5)) & 0xff;
            }
            s[7] = ((s[7] << 3) | (t >> 5)) & 0xff;
        }
        for (r = 0; r < ALPU_W - 1; r++) {  /* rotate s right by 3 + dispatch */
            uint8_t t = s[7], sel;

            for (i = 7; i > 0; i--) {
                s[i] = ((s[i] >> 3) | (s[i - 1] << 5)) & 0xff;
            }
            s[0] = ((s[0] >> 3) | (t << 5)) & 0xff;
            sel = ((s[0] >> 6) | ((s[0] >> 3) & 4)) & 0xff;
            if (sel == 0) {
                ops[n++].op = 0;
            } else if (sel == 1) {
                ops[n].op = 1;
                memcpy(ops[n].s, s, 8);
                n++;
            } else if (mode != 0 && sel == 3) {
                ops[n++].op = 3;
            }
        }
    }
    return n;
}

/* Inverse of one sub-operation */
static void alpu_apply_inv(uint8_t op, const uint8_t s[8], uint8_t o[8])
{
    uint8_t t[8];
    int i;

    if (op == 0) {
        for (i = 0; i < 4; i++) {
            t[i] = o[i + 4];
            t[i + 4] = (~o[i]) & 0xff;
        }
        memcpy(o, t, 8);
    } else if (op == 1) {
        for (i = 0; i < 4; i++) {
            t[i] = (o[i + 4] ^ s[i]) & 0xff;
            t[i + 4] = (o[i] ^ s[i + 4]) & 0xff;
        }
        memcpy(o, t, 8);
    } else if (op == 3) {
        for (i = 0; i < 7; i++) {
            t[i] = o[i + 1];
        }
        t[7] = o[0];
        memcpy(o, t, 8);
    }
}

/* W = transform^-1(target) with K3 = buf_b0 = buf_d8 = 0, auth mode */
static void alpu_inverse(const uint8_t target[8], uint8_t out[8],
                         const uint8_t ac[2])
{
    static const uint8_t z[8] = { 0 };
    static const uint8_t k3[2] = { 0 };
    AlpuOp ops[2 * ALPU_W];
    int n = alpu_plan(ops, k3, 2, z, z, ac);
    int i;

    memcpy(out, target, 8);
    for (i = n - 1; i >= 0; i--) {
        alpu_apply_inv(ops[i].op, ops[i].s, out);
    }
}

/* Build the 16-byte read response for an auth round from its arg0 */
static void alpu_build_response(AlpuState *s, const uint8_t tgt[8])
{
    static const int pos[8] = { 0, 1, 4, 5, 8, 9, 12, 13 };
    const uint8_t ac[2] = { 0, s->ac1 };
    uint8_t w[8];
    int i;

    alpu_inverse(tgt, w, ac);
    memset(s->resp, 0, sizeof(s->resp));        /* K3 bytes [2]/[11] stay 0 */
    for (i = 0; i < 8; i++) {
        s->resp[pos[i]] = w[i];
    }
    s->resp_len = 16;
}

/* Prepare the response for a master read of the register in cmd[0] */
static void alpu_prepare(AlpuState *s)
{
    uint8_t reg = s->cmd[0];

    memset(s->resp, 0, sizeof(s->resp));
    s->resp_len = 16;

    switch (reg) {
    case 0x30:                          /* nonce read starts an auth round */
        s->ac1 = 1;
        s->have_e9 = s->have_87 = false;
        break;
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
        s->resp_len = 8;                /* zeros -> host buf_b0/buf_d8 = 0 */
        break;
    case 0xe9:
        if (s->have_e9) {               /* the compute read (after the write) */
            s->ac1++;
            alpu_build_response(s, s->tgt_e9);
            s->have_e9 = false;
        }
        break;                          /* else pre-write read -> zeros */
    case 0x87:
        if (s->have_87) {
            s->ac1++;
            alpu_build_response(s, s->tgt_87);
            s->have_87 = false;
        }
        break;
    default:
        break;
    }
}

/* Act on a completed master write of data bytes to register cmd[0] */
static void alpu_finish_write(AlpuState *s)
{
    uint8_t reg = s->cmd[0];
    const uint8_t *data = &s->cmd[1];
    unsigned len = s->cmd_len - 1;
    int i;

    switch (reg) {
    case 0x40:                          /* phase 3 bumps host buf_ac by 2 */
        s->ac1 += 2;
        break;
    case 0xe9:
        if (len >= 15) {
            for (i = 0; i < 8; i++) {
                s->tgt_e9[i] = data[2 * i];
            }
            s->have_e9 = true;
        }
        break;
    case 0x87:
        if (len >= 15) {
            for (i = 0; i < 8; i++) {
                s->tgt_87[i] = data[2 * i];
            }
            s->have_87 = true;
        }
        break;
    default:
        break;
    }
}

static int alpu_event(I2CSlave *i2c, enum i2c_event event)
{
    AlpuState *s = ALPU(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->reading = false;
        s->cmd_len = 0;
        trace_alpu_event("START_SEND");
        break;
    case I2C_START_RECV:
        s->reading = true;
        s->resp_pos = 0;
        alpu_prepare(s);                /* cmd[0] holds the register pointer */
        trace_alpu_event("START_RECV");
        break;
    case I2C_FINISH:
        if (!s->reading && s->cmd_len > 1) {
            alpu_finish_write(s);
        }
        trace_alpu_event("FINISH");
        break;
    case I2C_NACK:
        trace_alpu_event("NACK");
        break;
    default:
        break;
    }
    return 0;
}

static int alpu_send(I2CSlave *i2c, uint8_t data)
{
    AlpuState *s = ALPU(i2c);

    if (s->cmd_len < ALPU_BUFSZ) {
        s->cmd[s->cmd_len] = data;
    }
    trace_alpu_send(s->cmd_len, data);
    s->cmd_len++;
    return 0;                           /* ACK */
}

static uint8_t alpu_recv(I2CSlave *i2c)
{
    AlpuState *s = ALPU(i2c);
    uint8_t data = s->resp_pos < s->resp_len ? s->resp[s->resp_pos] : 0;

    trace_alpu_recv(s->resp_pos, data);
    s->resp_pos++;
    return data;
}

static void alpu_reset(DeviceState *dev)
{
    AlpuState *s = ALPU(dev);

    s->reading = false;
    s->cmd_len = 0;
    s->resp_pos = 0;
    s->resp_len = 0;
    s->ac1 = 0;
    s->have_e9 = s->have_87 = false;
}

static void alpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, alpu_reset);
    sc->event = alpu_event;
    sc->send = alpu_send;
    sc->recv = alpu_recv;
}

static const TypeInfo alpu_types[] = {
    {
        .name           = TYPE_ALPU,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(AlpuState),
        .class_init     = alpu_class_init,
    },
};

DEFINE_TYPES(alpu_types)
