/*
 * Copyright (C) 2007 Marco Gerards <marco@gnu.org>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file libavcodec/dirac_arith.h
 * Arithmetic decoder for Dirac
 * @author Marco Gerards <marco@gnu.org>
 */

#ifndef AVCODEC_DIRAC_ARITH_H
#define AVCODEC_DIRAC_ARITH_H

#include "get_bits.h"

enum dirac_arith_contexts {
    CTX_ZPZN_F1,
    CTX_ZPNN_F1,
    CTX_NPZN_F1,
    CTX_NPNN_F1,
    CTX_ZP_F2,
    CTX_ZP_F3,
    CTX_ZP_F4,
    CTX_ZP_F5,
    CTX_ZP_F6,
    CTX_NP_F2,
    CTX_NP_F3,
    CTX_NP_F4,
    CTX_NP_F5,
    CTX_NP_F6,
    CTX_COEFF_DATA,
    CTX_SIGN_NEG,
    CTX_SIGN_ZERO,
    CTX_SIGN_POS,
    CTX_ZERO_BLOCK,
    CTX_DELTA_Q_F,
    CTX_DELTA_Q_DATA,
    CTX_DELTA_Q_SIGN,
#if 0
    CTX_SB_F1,
    CTX_SB_F2,
    CTX_SB_DATA,
    CTX_PMODE_REF1,
    CTX_PMODE_REF2,
    CTX_GLOBAL_BLOCK,
    CTX_MV_F1,
    CTX_MV_F2,
    CTX_MV_F3,
    CTX_MV_F4,
    CTX_MV_F5,
    CTX_MV_DATA,
    CTX_MV_SIGN,
    CTX_DC_F1,
    CTX_DC_F2,
    CTX_DC_DATA,
    CTX_DC_SIGN,
#endif
    DIRAC_CTX_COUNT
};

// Dirac resets the arith decoder between decoding various types of data,
// so many contexts are never used simultaneously. Thus, we can reduce
// the number of contexts needed by reusing them.
#define CTX_PMODE_REF1   0
#define CTX_PMODE_REF2   1
#define CTX_GLOBAL_BLOCK 2
#define CTX_SB_F1        CTX_ZP_F5
#define CTX_SB_DATA      0
#define CTX_MV_F1        CTX_ZP_F2
#define CTX_MV_DATA      0
#define CTX_DC_F1        CTX_ZP_F5
#define CTX_DC_DATA      0

typedef struct {
    unsigned low;
    unsigned range;
    unsigned counter;

    const uint8_t *bytestream_start;
    const uint8_t *bytestream;
    const uint8_t *bytestream_end;

    uint16_t contexts[DIRAC_CTX_COUNT];
} dirac_arith;

const uint8_t ff_dirac_next_ctx[DIRAC_CTX_COUNT];
const uint16_t ff_dirac_prob[256];

static inline void renorm_arith_decoder(dirac_arith *arith)
{
    while (arith->range <= 0x4000) {
        arith->low   <<= 1;
        arith->range <<= 1;

        if (!--arith->counter) {
            arith->low += AV_RB16(arith->bytestream);
            arith->bytestream += 2;

            // the spec defines overread bits to be 1
            if (arith->bytestream > arith->bytestream_end) {
                arith->low |= 0xff;
                if (arith->bytestream > arith->bytestream_end+1)
                    arith->low |= 0xff00;
                arith->bytestream = arith->bytestream_end;
            }
            arith->counter = 16;
        }
    }
}

static inline int dirac_get_arith_bit(dirac_arith *arith, int ctx)
{
    int prob_zero = arith->contexts[ctx];
    int range_times_prob, ret;
    int prob_index = arith->contexts[ctx] >> 8;

    range_times_prob  = (arith->range * prob_zero) >> 16;
    ret = (arith->low >> 16) >= range_times_prob;

    if (ret) {
        arith->low   -= range_times_prob << 16;
        arith->range -= range_times_prob;
        arith->contexts[ctx] -= ff_dirac_prob[prob_index];
    } else {
        arith->range  = range_times_prob;
        arith->contexts[ctx] += ff_dirac_prob[255 - prob_index];
    }

    renorm_arith_decoder(arith);
    return ret;
}

static inline int dirac_get_arith_uint(dirac_arith *arith, int follow_ctx, int data_ctx)
{
    int ret = 1;
    while (!dirac_get_arith_bit(arith, follow_ctx)) {
        ret <<= 1;
        ret += dirac_get_arith_bit(arith, data_ctx);
        follow_ctx = ff_dirac_next_ctx[follow_ctx];
    }
    return ret-1;
}

static inline int dirac_get_arith_int(dirac_arith *arith, int follow_ctx, int data_ctx)
{
    int ret = dirac_get_arith_uint(arith, follow_ctx, data_ctx);
    if (ret && dirac_get_arith_bit(arith, data_ctx+1))
        ret = -ret;
    return ret;
}

void ff_dirac_init_arith_decoder(dirac_arith *arith, GetBitContext *gb, int length);

#endif /* AVCODEC_DIRAC_ARITH_H */
