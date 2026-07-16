/*
 * MStar/SigmaStar board "security element" - i2c auth chip (0x3d on i2c1)
 *
 * Copyright (c) 2026 ...
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * TODO(rename): this part is actually an "ALPU-FA" (ALPU-family i2c copy-
 * protection / auth IC). Rename the type/file/struct to alpu-fa / AlpuFa* and
 * update MStarMachineClass.has_secelem + the wiki accordingly. Behaviour is
 * unchanged; this is a naming cleanup only.
 *
 * The Miyoo Mini (SSD202D) carries a small crypto auth chip on i2c1 at address
 * 0x3d. Both the vendor 4.9 kernel and the MainUI app run a challenge-response
 * handshake against it and refuse to run (kernel BUG()s PID 1; MainUI exit(2)s)
 * if it does not answer correctly.
 *
 * This models the chip's crypto for real, reverse-engineered from the vendor
 * kernel's software verifier (auth driver at 0xc01d0e64, "transform" cipher at
 * 0xc01d05a4, key tables at 0xc0345264/0xc0345274). The verifier is symmetric
 * and deterministic - it recomputes what the chip should return and byte-
 * compares - so the whole secret (algorithm + key tables) lives in the firmware
 * and the chip can be emulated exactly.
 *
 * Protocol (register = first byte of each i2c write; 7-bit addr 0x3d):
 *   wr 0x80/0x20/0x22   - init/config (ignored here)
 *   rd 0x30 (16B)       - chip nonce; host derives a response, writes it back
 *                         to 0x30 (we ignore it). Marks the start of an auth
 *                         round, so we (re)seed the buf_ac counter here.
 *   rd 0x73..0x76 (8B)  - feed the host's buf_b0/buf_d8; we return zeros so
 *                         both are all-zero in the host's transform.
 *   wr 0x40 (16B)       - phase-3 write (ignored); bumps buf_ac by 2.
 *   {wr,rd} 0xe9 (16B)  - auth round 0. Host writes arg0 in the even bytes,
 *   {wr,rd} 0x87 (16B)  - auth round 1. reads back, extracts an 8-byte word W
 *                         from bytes [0,1,4,5,8,9,12,13], computes
 *                         arg1 = transform(W) and requires arg1 == arg0.
 *
 * So for an 0xe9/0x87 read we must return W = transform^-1(arg0). The cipher's
 * operation sequence depends only on the key tables + external buffers (never
 * on its input), which makes transform a composition of invertible byte ops -
 * hence bijective and invertible. We keep buf_b0 = buf_d8 = K3 = 0 (by
 * returning zeros for the nonce/0x73..76 reads and putting 0 in the response's
 * K3 byte positions), so only the deterministic buf_ac counter varies.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/i2c/i2c.h"
#include "hw/arm/mstar.h"
#include "trace.h"

/* Key tables from the vendor kernel: 0xc0345264 and 0xc0345274. */
static const uint8_t SE_TBL0[16] = {
    0xe8, 0xf6, 0x78, 0x6a, 0x7b, 0x04, 0xf8, 0xd2,
    0xff, 0x1e, 0x6f, 0x82, 0x6c, 0xec, 0xef, 0x3a,
};
static const uint8_t SE_TBL1[16] = {
    0x9f, 0xb7, 0xfb, 0x2c, 0xc0, 0x36, 0x0a, 0x1e,
    0x09, 0x21, 0x6d, 0xba, 0x56, 0xa0, 0x9c, 0x88,
};

#define SE_W        0x20                /* transform "width" arg (rotations) */
#define SE_NROUNDS  2

typedef struct { uint8_t op; uint8_t S[8]; } SeOp;

/*
 * Replay the input-independent S-evolution and record the ordered list of
 * sub-operations dispatched onto the working buffer. mode!=0 (the auth-round
 * path) mixes in buf_b0/buf_d8; mode 0 uses the raw table. Only sel==1 needs
 * its S snapshot (the XOR sub-op); 0 and 3 are pure permutations.
 */
static int se_plan(SeOp *ops, const uint8_t K3[2], int mode,
                   const uint8_t b0[8], const uint8_t d8[8], const uint8_t ac[2])
{
    int n = 0;

    for (int rnd = 0; rnd < SE_NROUNDS; rnd++) {
        uint8_t S[8];

        for (int i = 0; i < 8; i++) {
            S[i] = mode == 0 ? SE_TBL0[rnd * 8 + i]
                             : (SE_TBL1[rnd * 8 + i] ^ b0[i] ^ d8[i]) & 0xff;
        }
        for (int i = 0; i < 2; i++) {
            S[i] ^= K3[i];
        }
        for (int i = 0; i < 2; i++) {
            S[i + 2] ^= ac[i];
        }
        for (int r = 0; r < SE_W - 1; r++) {            /* rotate S left by 3 */
            uint8_t t = S[0];
            for (int i = 0; i < 7; i++) {
                S[i] = ((S[i] << 3) | (S[i + 1] >> 5)) & 0xff;
            }
            S[7] = ((S[7] << 3) | (t >> 5)) & 0xff;
        }
        for (int r = 0; r < SE_W - 1; r++) {   /* rotate S right by 3 + dispatch */
            uint8_t t = S[7], sel;
            for (int i = 7; i > 0; i--) {
                S[i] = ((S[i] >> 3) | (S[i - 1] << 5)) & 0xff;
            }
            S[0] = ((S[0] >> 3) | (t << 5)) & 0xff;
            sel = ((S[0] >> 6) | ((S[0] >> 3) & 4)) & 0xff;
            if (sel == 0) {
                ops[n++].op = 0;
            } else if (sel == 1) {
                ops[n].op = 1;
                memcpy(ops[n].S, S, 8);
                n++;
            } else if (mode != 0 && sel == 3) {
                ops[n++].op = 3;
            }
        }
    }
    return n;
}

/* Inverse of one sub-operation (see the forward ops in the RE notes). */
static void se_apply_inv(uint8_t op, const uint8_t S[8], uint8_t O[8])
{
    uint8_t T[8];

    if (op == 0) {
        for (int i = 0; i < 4; i++) {
            T[i] = O[i + 4];
            T[i + 4] = (~O[i]) & 0xff;
        }
        memcpy(O, T, 8);
    } else if (op == 1) {
        for (int i = 0; i < 4; i++) {
            T[i] = (O[i + 4] ^ S[i]) & 0xff;
            T[i + 4] = (O[i] ^ S[i + 4]) & 0xff;
        }
        memcpy(O, T, 8);
    } else if (op == 3) {
        for (int i = 0; i < 7; i++) {
            T[i] = O[i + 1];
        }
        T[7] = O[0];
        memcpy(O, T, 8);
    }
}

/* W = transform^-1(target) with K3 = buf_b0 = buf_d8 = 0, mode = auth. */
static void se_inverse(const uint8_t target[8], uint8_t out[8],
                       const uint8_t ac[2])
{
    static const uint8_t z[8] = { 0 };
    static const uint8_t K3[2] = { 0 };
    SeOp ops[2 * SE_W];
    int n = se_plan(ops, K3, 2, z, z, ac);

    memcpy(out, target, 8);
    for (int i = n - 1; i >= 0; i--) {
        se_apply_inv(ops[i].op, ops[i].S, out);
    }
}

/* Build the 16-byte read response for an auth round from its captured arg0. */
static void se_build_response(MstarSecElemState *s, const uint8_t tgt[8])
{
    static const int pos[8] = { 0, 1, 4, 5, 8, 9, 12, 13 };
    const uint8_t ac[2] = { 0, s->ac1 };
    uint8_t W[8];

    se_inverse(tgt, W, ac);
    memset(s->resp, 0, sizeof(s->resp));        /* K3 bytes [2]/[11] stay 0 */
    for (int i = 0; i < 8; i++) {
        s->resp[pos[i]] = W[i];
    }
    s->resp_len = 16;
}

/* Prepare s->resp for a master-read of the register selected by cmd[0]. */
static void mstar_secelem_prepare(MstarSecElemState *s)
{
    uint8_t reg = s->cmd[0];

    memset(s->resp, 0, sizeof(s->resp));
    s->resp_len = 16;

    switch (reg) {
    case 0x30:                          /* nonce read == start of an auth round */
        s->ac1 = 1;
        s->have_e9 = s->have_87 = false;
        break;
    case 0x73: case 0x74: case 0x75: case 0x76:
        s->resp_len = 8;                /* zeros -> host buf_b0/buf_d8 = 0 */
        break;
    case 0xe9:
        if (s->have_e9) {               /* the compute read (after the write) */
            s->ac1++;
            se_build_response(s, s->tgt_e9);
            s->have_e9 = false;
        }
        break;                          /* else: pre-write read -> all zeros */
    case 0x87:
        if (s->have_87) {
            s->ac1++;
            se_build_response(s, s->tgt_87);
            s->have_87 = false;
        }
        break;
    default:
        break;
    }
}

/* Act on a completed master-write of data bytes to register cmd[0]. */
static void mstar_secelem_write(MstarSecElemState *s)
{
    uint8_t reg = s->cmd[0];
    const uint8_t *data = &s->cmd[1];
    unsigned len = s->cmd_len - 1;

    switch (reg) {
    case 0x40:                          /* phase 3: bumps host buf_ac by 2 */
        s->ac1 += 2;
        break;
    case 0xe9:
        if (len >= 15) {
            for (int i = 0; i < 8; i++) {
                s->tgt_e9[i] = data[2 * i];
            }
            s->have_e9 = true;
        }
        break;
    case 0x87:
        if (len >= 15) {
            for (int i = 0; i < 8; i++) {
                s->tgt_87[i] = data[2 * i];
            }
            s->have_87 = true;
        }
        break;
    default:
        break;
    }
}

static int mstar_secelem_event(I2CSlave *i2c, enum i2c_event event)
{
    MstarSecElemState *s = MSTAR_SECELEM(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->reading = false;
        s->cmd_len = 0;
        trace_mstar_secelem_event("START_SEND");
        break;
    case I2C_START_RECV:
        s->reading = true;
        s->resp_pos = 0;
        mstar_secelem_prepare(s);       /* cmd[0] holds the register pointer */
        trace_mstar_secelem_event("START_RECV");
        break;
    case I2C_FINISH:
        if (!s->reading && s->cmd_len > 1) {
            mstar_secelem_write(s);
        }
        trace_mstar_secelem_event("FINISH");
        break;
    case I2C_NACK:
        trace_mstar_secelem_event("NACK");
        break;
    default:
        break;
    }
    return 0;                           /* never NAK */
}

static int mstar_secelem_send(I2CSlave *i2c, uint8_t data)
{
    MstarSecElemState *s = MSTAR_SECELEM(i2c);

    if (s->cmd_len < MSTAR_SECELEM_BUFSZ) {
        s->cmd[s->cmd_len] = data;
    }
    trace_mstar_secelem_send(s->cmd_len, data);
    s->cmd_len++;
    return 0;                           /* ACK */
}

static uint8_t mstar_secelem_recv(I2CSlave *i2c)
{
    MstarSecElemState *s = MSTAR_SECELEM(i2c);
    uint8_t data = s->resp_pos < s->resp_len ? s->resp[s->resp_pos] : 0;

    trace_mstar_secelem_recv(s->resp_pos, data);
    s->resp_pos++;
    return data;
}

static void mstar_secelem_reset(DeviceState *dev)
{
    MstarSecElemState *s = MSTAR_SECELEM(dev);

    s->reading = false;
    s->cmd_len = 0;
    s->resp_pos = 0;
    s->resp_len = 0;
    s->ac1 = 0;
    s->have_e9 = s->have_87 = false;
}

static void mstar_secelem_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    device_class_set_legacy_reset(dc, mstar_secelem_reset);
    sc->event = mstar_secelem_event;
    sc->send = mstar_secelem_send;
    sc->recv = mstar_secelem_recv;
}

static const TypeInfo mstar_secelem_types[] = {
    {
        .name           = TYPE_MSTAR_SECELEM,
        .parent         = TYPE_I2C_SLAVE,
        .instance_size  = sizeof(MstarSecElemState),
        .class_init     = mstar_secelem_class_init,
    },
};

DEFINE_TYPES(mstar_secelem_types)
