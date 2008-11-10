/* -*-  indent-tabs-mode:nil; c-basic-offset:4;  -*- */
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
 * @file diracdec.c
 * Dirac Decoder
 * @author Marco Gerards <marco@gnu.org>
 */

#undef DEBUG

#include "dirac.h"
#include "avcodec.h"
#include "dsputil.h"
#include "bitstream.h"
#include "bytestream.h"
#include "golomb.h"
#include "dirac_arith.h"
#include "dirac_wavelet.h"
#include "mpeg12data.h"

static int decode_init(AVCodecContext *avctx)
{
    av_log_set_level(AV_LOG_DEBUG);
    return 0;
}

static int decode_end(AVCodecContext *avctx)
{
    // DiracContext *s = avctx->priv_data;

    return 0;
}

/**
 * Dequantize a coefficient
 *
 * @param coeff coefficient to dequantize
 * @param qoffset quantizer offset
 * @param qfactor quantizer factor
 * @return dequantized coefficient
 */
static inline int coeff_dequant(int coeff, int qoffset, int qfactor)
{
    if (! coeff)
        return 0;

    coeff *= qfactor;

    coeff += qoffset;
    coeff >>= 2;

    return coeff;
}

/**
 * Unpack a single coefficient
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the subband
 * @param v vertical position of the to be decoded coefficient in the subband
 * @param h horizontal position of the to be decoded coefficient in the subband
 * @param qoffset quantizer offset
 * @param qfact quantizer factor
 */
static void coeff_unpack(DiracContext *s, int16_t *data, int level,
                         subband_t orientation, int v, int h,
                         int qoffset, int qfactor)
{
    int parent = 0;
    int nhood;
    int idx;
    int coeff;
    int read_sign;
    struct dirac_arith_context_set *context;
    int16_t *coeffp;
    int vdata, hdata;

    vdata = coeff_posy(s, level, orientation, v);
    hdata = coeff_posx(s, level, orientation, h);

    coeffp = &data[hdata + vdata * s->padded_width];

    /* The value of the pixel belonging to the lower level.  */
    if (level >= 2) {
        int x = coeff_posx(s, level - 1, orientation, h >> 1);
        int y = coeff_posy(s, level - 1, orientation, v >> 1);
        parent = data[s->padded_width * y + x] != 0;
    }

    /* Determine if the pixel has only zeros in its neighbourhood.  */
    nhood = zero_neighbourhood(s, coeffp, v, h);

    /* Calculate an index into context_sets_waveletcoeff.  */
    idx = parent * 6 + (!nhood) * 3;
    idx += sign_predict(s, coeffp, orientation, v, h);

    context = &ff_dirac_context_sets_waveletcoeff[idx];

    coeff = dirac_arith_read_uint(&s->arith, context);

    read_sign = coeff;
    coeff = coeff_dequant(coeff, qoffset, qfactor);
    if (read_sign) {
        if (dirac_arith_get_bit(&s->arith, context->sign))
            coeff = -coeff;
    }

    *coeffp = coeff;
}

/**
 * Decode a codeblock
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the current subband
 * @param x position of the codeblock within the subband in units of codeblocks
 * @param y position of the codeblock within the subband in units of codeblocks
 * @param quant quantizer offset
 * @param quant quantizer factor
 */
static void codeblock(DiracContext *s, int16_t *data, int level,
                      subband_t orientation, int x, int y,
                      int qoffset, int qfactor)
{
    int blockcnt_one = (s->codeblocksh[level] + s->codeblocksv[level]) == 2;
    int left, right, top, bottom;
    int v, h;

    left   = (subband_width(s, level)  *  x     ) / s->codeblocksh[level];
    right  = (subband_width(s, level)  * (x + 1)) / s->codeblocksh[level];
    top    = (subband_height(s, level) *  y     ) / s->codeblocksv[level];
    bottom = (subband_height(s, level) * (y + 1)) / s->codeblocksv[level];

    if (!blockcnt_one) {
        /* Determine if this codeblock is a zero block.  */
        if (dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_ZERO_BLOCK))
            return;
    }

    for (v = top; v < bottom; v++)
        for (h = left; h < right; h++)
            coeff_unpack(s, data, level, orientation, v, h,
                         qoffset, qfactor);
}

/**
 * Intra DC Prediction
 *
 * @param data coefficients
 */
static void intra_dc_prediction(DiracContext *s, int16_t *data)
{
    int x, y;
    int16_t *line = data;

    for (y = 0; y < subband_height(s, 0); y++) {
        for (x = 0; x < subband_width(s, 0); x++) {
            line[x] += intra_dc_coeff_prediction(s, &line[x], x, y);
        }
        line += s->padded_width;
    }
}

/**
 * Decode a subband
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the subband
 */
static int subband(DiracContext *s, int16_t *data, int level,
                   subband_t orientation)
{
    GetBitContext *gb = &s->gb;
    unsigned int length;
    unsigned int quant, qoffset, qfactor;
    int x, y;

    length = svq3_get_ue_golomb(gb);
    if (! length) {
        align_get_bits(gb);
    } else {
        quant = svq3_get_ue_golomb(gb);
        qfactor = coeff_quant_factor(quant);
        qoffset = coeff_quant_offset(s, quant) + 2;

        dirac_arith_init(&s->arith, gb, length);

        for (y = 0; y < s->codeblocksv[level]; y++)
            for (x = 0; x < s->codeblocksh[level]; x++)
                codeblock(s, data, level, orientation, x, y,
                          qoffset, qfactor);
        dirac_arith_flush(&s->arith);
    }

    return 0;
}

/**
 * Decode the DC subband
 *
 * @param data coefficients
 * @param level subband level
 * @param orientation orientation of the subband
 */
static int subband_dc(DiracContext *s, int16_t *data)
{
    GetBitContext *gb = &s->gb;
    unsigned int length;
    unsigned int quant, qoffset, qfactor;
    int width, height;
    int x, y;

    width  = subband_width(s, 0);
    height = subband_height(s, 0);

    length = svq3_get_ue_golomb(gb);
    if (! length) {
        align_get_bits(gb);
    } else {
        quant = svq3_get_ue_golomb(gb);
        qfactor = coeff_quant_factor(quant);
        qoffset = coeff_quant_offset(s, quant) + 2;

        dirac_arith_init(&s->arith, gb, length);

        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                coeff_unpack(s, data, 0, subband_ll, y, x,
                         qoffset, qfactor);

        dirac_arith_flush(&s->arith);
    }

    if (s->refs == 0)
        intra_dc_prediction(s, data);

    return 0;
}

/**
 * Unpack the motion compensation parameters
 */
static int dirac_unpack_prediction_parameters(DiracContext *s)
{
    GetBitContext *gb = &s->gb;

    /* Read block parameters.  */
    unsigned int idx = svq3_get_ue_golomb(gb);

    if (idx > 4)
        return -1;

    if (idx == 0) {
        s->decoding.luma_xblen = svq3_get_ue_golomb(gb);
        s->decoding.luma_yblen = svq3_get_ue_golomb(gb);
        s->decoding.luma_xbsep = svq3_get_ue_golomb(gb);
        s->decoding.luma_ybsep = svq3_get_ue_golomb(gb);
    } else {
        s->decoding.luma_xblen = ff_dirac_block_param_defaults[idx - 1].xblen;
        s->decoding.luma_yblen = ff_dirac_block_param_defaults[idx - 1].yblen;
        s->decoding.luma_xbsep = ff_dirac_block_param_defaults[idx - 1].xbsep;
        s->decoding.luma_ybsep = ff_dirac_block_param_defaults[idx - 1].ybsep;
    }

    /* Setup the blen and bsep parameters for the chroma
       component.  */
    s->decoding.chroma_xblen = s->decoding.luma_xblen >> s->chroma_hshift;
    s->decoding.chroma_yblen = s->decoding.luma_yblen >> s->chroma_vshift;
    s->decoding.chroma_xbsep = s->decoding.luma_xbsep >> s->chroma_hshift;
    s->decoding.chroma_ybsep = s->decoding.luma_ybsep >> s->chroma_vshift;

    /* Read motion vector precision.  */
    s->decoding.mv_precision = svq3_get_ue_golomb(gb);

    /* Read the global motion compensation parameters.  */
    s->globalmc_flag = get_bits1(gb);
    if (s->globalmc_flag) {
        int ref;
        for (ref = 0; ref < s->refs; ref++) {
            memset(&s->globalmc, 0, sizeof(s->globalmc));

            /* Pan/til parameters.  */
            if (get_bits1(gb)) {
                s->globalmc.pan_tilt[0] = dirac_get_se_golomb(gb);
                s->globalmc.pan_tilt[1] = dirac_get_se_golomb(gb);
            }

            /* Rotation/shear parameters.  */
            if (get_bits1(gb)) {
                s->globalmc.zrs_exp = svq3_get_ue_golomb(gb);
                s->globalmc.zrs[0][0] = dirac_get_se_golomb(gb);
                s->globalmc.zrs[0][1] = dirac_get_se_golomb(gb);
                s->globalmc.zrs[1][0] = dirac_get_se_golomb(gb);
                s->globalmc.zrs[1][1] = dirac_get_se_golomb(gb);
            } else {
                s->globalmc.zrs[0][0] = 1;
                s->globalmc.zrs[1][1] = 1;
            }

            /* Perspective parameters.  */
            if (get_bits1(gb)) {
                s->globalmc.perspective_exp = svq3_get_ue_golomb(gb);
                s->globalmc.perspective[0] = dirac_get_se_golomb(gb);
                s->globalmc.perspective[1] = dirac_get_se_golomb(gb);
            }
        }
    }

    /* Picture prediction mode.  Not used yet in the specification, so
       just ignore it, it should and will be zero.  */
    svq3_get_ue_golomb(gb);

    /* Default weights */
    s->decoding.picture_weight_precision = 1;
    s->decoding.picture_weight_ref1      = 1;
    s->decoding.picture_weight_ref2      = 1;

    /* Override reference picture weights.  */
    if (get_bits1(gb)) {
        s->decoding.picture_weight_precision = svq3_get_ue_golomb(gb);
        s->decoding.picture_weight_ref1 = dirac_get_se_golomb(gb);
        if (s->refs == 2)
            s->decoding.picture_weight_ref2 = dirac_get_se_golomb(gb);
    }

    return 0;
}

/**
 * Blockmode prediction
 *
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 */
static void blockmode_prediction(DiracContext *s, int x, int y)
{
    int res = dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_PMODE_REF1);

    res ^= mode_prediction(s, x, y, DIRAC_REF_MASK_REF1, 0);
    s->blmotion[y * s->blwidth + x].use_ref |= res;
    if (s->refs == 2) {
        res = dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_PMODE_REF2);
        res ^= mode_prediction(s, x, y, DIRAC_REF_MASK_REF2, 1);
        s->blmotion[y * s->blwidth + x].use_ref |= res << 1;
    }
}

/**
 * Prediction for global motion compensation
 *
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 */
static void blockglob_prediction(DiracContext *s, int x, int y)
{
    /* Global motion compensation is not used at all.  */
    if (!s->globalmc_flag)
        return;

    /* Global motion compensation is not used for this block.  */
    if (s->blmotion[y * s->blwidth + x].use_ref & 3) {
        int res = dirac_arith_get_bit(&s->arith, ARITH_CONTEXT_GLOBAL_BLOCK);
        res ^= mode_prediction(s, x, y, DIRAC_REF_MASK_GLOBAL, 2);
        s->blmotion[y * s->blwidth + x].use_ref |= res << 2;
    }
}

/**
 * copy the block data to other MC blocks
 *
 * @param step superblock step size, so the number of MC blocks to copy
 * @param x    horizontal position of the MC block
 * @param y    vertical position of the MC block
 */
static void propagate_block_data(DiracContext *s, int step, int x, int y)
{
    int i, j;

    /* XXX: For now this is rather inefficient, because everything is
       copied.  This function is called quite often.  */
    for (j = y; j < y + step; j++)
        for (i = x; i < x + step; i++)
            s->blmotion[j * s->blwidth + i] = s->blmotion[y * s->blwidth + x];
}

static void unpack_block_dc(DiracContext *s, int x, int y, int comp)
{
    int res;

    if (s->blmotion[y * s->blwidth + x].use_ref & 3) {
        s->blmotion[y * s->blwidth + x].dc[comp] = 0;
        return;
    }

    res = dirac_arith_read_int(&s->arith, &ff_dirac_context_set_dc);
    res += block_dc_prediction(s, x, y, comp);

    s->blmotion[y * s->blwidth + x].dc[comp] = res;
}

/**
 * Unpack a single motion vector
 *
 * @param ref reference frame
 * @param dir direction horizontal=0, vertical=1
 */
static void dirac_unpack_motion_vector(DiracContext *s, int ref, int dir,
                                       int x, int y)
{
    int res;
    const int refmask = (ref + 1) | DIRAC_REF_MASK_GLOBAL;

    /* First determine if for this block in the specific reference
       frame a motion vector is required.  */
    if ((s->blmotion[y * s->blwidth + x].use_ref & refmask) != ref + 1)
        return;

    res = dirac_arith_read_int(&s->arith, &ff_dirac_context_set_mv);
    res += motion_vector_prediction(s, x, y, ref, dir);
    s->blmotion[y * s->blwidth + x].vect[ref][dir] = res;
}

/**
 * Unpack motion vectors
 *
 * @param ref reference frame
 * @param dir direction horizontal=0, vertical=1
 */
static void dirac_unpack_motion_vectors(DiracContext *s, int ref, int dir)
{
    GetBitContext *gb = &s->gb;
    unsigned int length;
    int x, y;

    length = svq3_get_ue_golomb(gb);
    dirac_arith_init(&s->arith, gb, length);
    for (y = 0; y < s->sbheight; y++)
        for (x = 0; x < s->sbwidth; x++) {
                        int q, p;
            int blkcnt = 1 << s->sbsplit[y * s->sbwidth + x];
            int step = 4 >> s->sbsplit[y * s->sbwidth + x];

            for (q = 0; q < blkcnt; q++)
                for (p = 0; p < blkcnt; p++) {
                    dirac_unpack_motion_vector(s, ref, dir,
                                         4 * x + p * step,
                                         4 * y + q * step);
                    propagate_block_data(s, step,
                                         4 * x + p * step,
                                         4 * y + q * step);
                }
        }
    dirac_arith_flush(&s->arith);
}

/**
 * Unpack the motion compensation parameters
 */
static int dirac_unpack_block_motion_data(DiracContext *s)
{
    GetBitContext *gb = &s->gb;
    int i;
    unsigned int length;
    int comp;
    int x, y;

#define DIVRNDUP(a, b) ((a + b - 1) / b)

    s->sbwidth  = DIVRNDUP(s->source.luma_width,
                           (s->decoding.luma_xbsep << 2));
    s->sbheight = DIVRNDUP(s->source.luma_height,
                           (s->decoding.luma_ybsep << 2));
    s->blwidth  = s->sbwidth  << 2;
    s->blheight = s->sbheight << 2;

    s->sbsplit  = av_mallocz(s->sbwidth * s->sbheight * sizeof(int));
    if (!s->sbsplit) {
        av_log(s->avctx, AV_LOG_ERROR, "av_mallocz() failed\n");
        return -1;
    }

    s->blmotion = av_mallocz(s->blwidth * s->blheight * sizeof(*s->blmotion));
    if (!s->blmotion) {
        av_freep(&s->sbsplit);
        av_log(s->avctx, AV_LOG_ERROR, "av_mallocz() failed\n");
        return -1;
    }

    /* Superblock splitmodes.  */
    length = svq3_get_ue_golomb(gb);
    dirac_arith_init(&s->arith, gb, length);
    for (y = 0; y < s->sbheight; y++)
        for (x = 0; x < s->sbwidth; x++) {
            int res = dirac_arith_read_uint(&s->arith, &ff_dirac_context_set_split);
            s->sbsplit[y * s->sbwidth + x] = res + split_prediction(s, x, y);
            s->sbsplit[y * s->sbwidth + x] %= 3;
        }
    dirac_arith_flush(&s->arith);

    /* Prediction modes.  */
    length = svq3_get_ue_golomb(gb);
    dirac_arith_init(&s->arith, gb, length);
    for (y = 0; y < s->sbheight; y++)
        for (x = 0; x < s->sbwidth; x++) {
            int q, p;
            int blkcnt = 1 << s->sbsplit[y * s->sbwidth + x];
            int step   = 4 >> s->sbsplit[y * s->sbwidth + x];

            for (q = 0; q < blkcnt; q++)
                for (p = 0; p < blkcnt; p++) {
                    int xblk = 4*x + p*step;
                    int yblk = 4*y + q*step;
                    blockmode_prediction(s, xblk, yblk);
                    blockglob_prediction(s, xblk, yblk);
                    propagate_block_data(s, step, xblk, yblk);
                }
        }
    dirac_arith_flush(&s->arith);

    /* Unpack the motion vectors.  */
    for (i = 0; i < s->refs; i++) {
        dirac_unpack_motion_vectors(s, i, 0);
        dirac_unpack_motion_vectors(s, i, 1);
    }

    /* Unpack the DC values for all the three components (YUV).  */
    for (comp = 0; comp < 3; comp++) {
        /* Unpack the DC values.  */
        length = svq3_get_ue_golomb(gb);
        dirac_arith_init(&s->arith, gb, length);
        for (y = 0; y < s->sbheight; y++)
            for (x = 0; x < s->sbwidth; x++) {
                int q, p;
                int blkcnt = 1 << s->sbsplit[y * s->sbwidth + x];
                int step   = 4 >> s->sbsplit[y * s->sbwidth + x];

                for (q = 0; q < blkcnt; q++)
                    for (p = 0; p < blkcnt; p++) {
                        int xblk = 4*x + p*step;
                        int yblk = 4*y + q*step;
                        unpack_block_dc(s, xblk, yblk, comp);
                        propagate_block_data(s, step, xblk, yblk);
                    }
            }
        dirac_arith_flush(&s->arith);
    }

    return 0;
}

/**
 * Decode a single component
 *
 * @param coeffs coefficients for this component
 */
static void decode_component(DiracContext *s, int16_t *coeffs)
{
    GetBitContext *gb = &s->gb;
    int level;
    subband_t orientation;

    /* Align for coefficient bitstream.  */
    align_get_bits(gb);

    /* Unpack LL, level 0.  */
    subband_dc(s, coeffs);

    /* Unpack all other subbands at all levels.  */
    for (level = 1; level <= s->decoding.wavelet_depth; level++) {
        for (orientation = 1; orientation <= subband_hh; orientation++)
            subband(s, coeffs, level, orientation);
    }
 }

/**
 * IDWT
 *
 * @param coeffs coefficients to transform
 * @return returns 0 on succes, otherwise -1
 */
int dirac_idwt(DiracContext *s, int16_t *coeffs, int16_t *synth)
{
    int level;
    int width, height;

    for (level = 1; level <= s->decoding.wavelet_depth; level++) {
        width  = subband_width(s, level);
        height = subband_height(s, level);

        switch(s->wavelet_idx) {
        case 0:
            dprintf(s->avctx, "Deslauriers-Debuc (9,7) IDWT\n");
            dirac_subband_idwt_97(s->avctx, width, height, s->padded_width,
                                  coeffs, synth, level);
            break;
        case 1:
            dprintf(s->avctx, "LeGall (5,3) IDWT\n");
            dirac_subband_idwt_53(s->avctx, width, height, s->padded_width,
                                  coeffs, synth, level);
            break;
        default:
            av_log(s->avctx, AV_LOG_INFO, "unknown IDWT index: %d\n",
                   s->wavelet_idx);
        }
    }

    return 0;
}

/**
 * Decode a frame.
 *
 * @return 0 when successful, otherwise -1 is returned
 */
static int dirac_decode_frame_internal(DiracContext *s)
{
    AVCodecContext *avctx = s->avctx;
    int16_t *coeffs;
    int16_t *line;
    int16_t *mcline;
    int comp;
    int x,y;
    int16_t *synth;

    if (avcodec_check_dimensions(s->avctx, s->padded_luma_width,
                                 s->padded_luma_height)) {
        av_log(s->avctx, AV_LOG_ERROR, "avcodec_check_dimensions() failed\n");
        return -1;
    }

    coeffs = av_malloc(s->padded_luma_width * s->padded_luma_height
                       * sizeof(int16_t));
    if (! coeffs) {
        av_log(s->avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    /* Allocate memory for the IDWT to work in.  */
    synth = av_malloc(s->padded_luma_width * s->padded_luma_height
                      * sizeof(int16_t));
    if (!synth) {
        av_log(avctx, AV_LOG_ERROR, "av_malloc() failed\n");
        return -1;
    }

    for (comp = 0; comp < 3; comp++) {
        uint8_t *frame = s->picture.data[comp];
        int width, height;

        if (comp == 0) {
            width            = s->source.luma_width;
            height           = s->source.luma_height;
            s->padded_width  = s->padded_luma_width;
            s->padded_height = s->padded_luma_height;
        } else {
            width            = s->source.chroma_width;
            height           = s->source.chroma_height;
            s->padded_width  = s->padded_chroma_width;
            s->padded_height = s->padded_chroma_height;
        }

        memset(coeffs, 0,
               s->padded_width * s->padded_height * sizeof(int16_t));

        if (!s->zero_res)
            decode_component(s, coeffs);

        dirac_idwt(s, coeffs, synth);

        if (s->refs) {
            if (dirac_motion_compensation(s, coeffs, comp)) {
                av_freep(&s->sbsplit);
                av_freep(&s->blmotion);

                return -1;
            }
        }

        /* Copy the decoded coefficients into the frame and also add
           the data calculated by MC.  */
        line = coeffs;
        if (s->refs) {
            mcline    = s->mcpic;
            for (y = 0; y < height; y++) {
                for (x = 0; x < width; x++) {
                    int16_t coeff = mcline[x] + (1 << (s->total_wt_bits - 1));
                    line[x] += coeff >> s->total_wt_bits;
                    frame[x]= av_clip_uint8(line[x] + 128);
                }

                line  += s->padded_width;
                frame += s->picture.linesize[comp];
                mcline    += s->width;
        }
        } else {
            for (y = 0; y < height; y++) {
                for (x = 0; x < width; x++) {
                    frame[x]= av_clip_uint8(line[x] + 128);
                }

                line  += s->padded_width;
                frame += s->picture.linesize[comp];
            }
        }

        /* XXX: Just (de)allocate this once.  */
        av_freep(&s->mcpic);
    }

    if (s->refs) {
        av_freep(&s->sbsplit);
        av_freep(&s->blmotion);
    }
    av_free(coeffs);
    av_free(synth);

    return 0;
}

/**
 * Parse a frame and setup DiracContext to decode it
 *
 * @return 0 when successful, otherwise -1 is returned
 */
static int parse_frame(DiracContext *s)
{
    uint32_t retire;
    int i;
    GetBitContext *gb = &s->gb;

    s->picture.pict_type = FF_I_TYPE;
    s->picture.key_frame = 1;

    s->picnum = get_bits_long(gb, 32);

    for (i = 0; i < s->refs; i++)
        s->ref[i] = dirac_get_se_golomb(gb) + s->picnum;

    /* Retire the reference frames that are not used anymore.  */
    s->retirecnt = 0;
    if (s->picture.reference) {
        retire = dirac_get_se_golomb(gb);
        if (retire) {
            s->retireframe[0] = s->picnum;
            s->retirecnt = 1;
        }
    }

    if (s->refs) {
        align_get_bits(gb);
        if (dirac_unpack_prediction_parameters(s))
            return -1;
        align_get_bits(gb);
        if (dirac_unpack_block_motion_data(s))
            return -1;
    }

    align_get_bits(gb);

    /* Wavelet transform data.  */
    if (s->refs == 0)
        s->zero_res = 0;
    else
        s->zero_res = get_bits1(gb);

    if (!s->zero_res) {
        s->wavelet_idx = svq3_get_ue_golomb(gb);

        if (s->wavelet_idx > 6)
            return -1;

        s->decoding.wavelet_depth = svq3_get_ue_golomb(gb);

        /* Codeblock paramaters (core syntax only) */
        if (get_bits1(gb)) {
            for (i = 0; i <= s->decoding.wavelet_depth; i++) {
                s->codeblocksh[i] = svq3_get_ue_golomb(gb);
                s->codeblocksv[i] = svq3_get_ue_golomb(gb);
            }

            s->codeblock_mode = svq3_get_ue_golomb(gb);
        } else
            for (i = 0; i <= s->decoding.wavelet_depth; i++)
                s->codeblocksh[i] = s->codeblocksv[i] = 1;
    }

#define CALC_PADDING(size, depth) \
         (((size + (1 << depth) - 1) >> depth) << depth)

    /* Round up to a multiple of 2^depth.  */
    s->padded_luma_width    = CALC_PADDING(s->source.luma_width,
                                           s->decoding.wavelet_depth);
    s->padded_luma_height   = CALC_PADDING(s->source.luma_height,
                                           s->decoding.wavelet_depth);
    s->padded_chroma_width  = CALC_PADDING(s->source.chroma_width,
                                           s->decoding.wavelet_depth);
    s->padded_chroma_height = CALC_PADDING(s->source.chroma_height,
                                           s->decoding.wavelet_depth);

    return 0;
}


int dirac_decode_frame(AVCodecContext *avctx, void *data, int *data_size,
                        const uint8_t *buf, int buf_size)
{
    DiracContext *s = avctx->priv_data;
    AVFrame *picture = data;
    int i;
    int parse_code;

    if (buf_size == 0) {
        int idx = dirac_reference_frame_idx(s, avctx->frame_number);
        if (idx == -1) {
            /* The frame was not found.  */
            *data_size = 0;
        } else {
            *data_size = sizeof(AVFrame);
            *picture = s->refframes[idx].frame;
        }
        return 0;
    }

    parse_code = buf[4];

    dprintf(avctx, "Decoding frame: size=%d head=%c%c%c%c parse=%02x\n",
            buf_size, buf[0], buf[1], buf[2], buf[3], buf[4]);

    init_get_bits(&s->gb, &buf[13], (buf_size - 13) * 8);
    s->avctx = avctx;

    if (parse_code ==  pc_seq_header) {
        if (ff_dirac_parse_sequence_header(s))
            return -1;

        /* Dump the header.  */
#if 0
        dirac_dump_source_parameters(avctx);
#endif

        return 0;
    }

    /* If this is not a picture, return.  */
    if ((parse_code & 0x08) != 0x08)
        return 0;

    s->refs = parse_code & 0x03;
    s->picture.reference = (parse_code & 0x0C) == 0x0C;

    if (parse_frame(s) < 0)
        return -1;

    avctx->pix_fmt = PIX_FMT_YUV420P; /* XXX */

    if (avcodec_check_dimensions(avctx, s->source.luma_width,
                                 s->source.luma_height)) {
        av_log(avctx, AV_LOG_ERROR,
               "avcodec_check_dimensions() failed\n");
        return -1;
    }

    avcodec_set_dimensions(avctx, s->source.luma_width,
                           s->source.luma_height);

    if (s->picture.data[0] != NULL)
        avctx->release_buffer(avctx, &s->picture);

    if (avctx->get_buffer(avctx, &s->picture) < 0) {
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        return -1;
    }

#if 0
    for (i = 0; i < s->refcnt; i++)
        dprintf(avctx, "Reference frame #%d\n",
                s->refframes[i].frame.display_picture_number);

    for (i = 0; i < s->refs; i++)
        dprintf(avctx, "Reference frame %d: #%d\n", i, s->ref[i]);
#endif

    if (dirac_decode_frame_internal(s))
        return -1;

    s->picture.display_picture_number = s->picnum;

    if (s->picture.reference
        || s->picture.display_picture_number != avctx->frame_number) {
        if (s->refcnt + 1 == REFFRAME_CNT) {
            av_log(avctx, AV_LOG_ERROR, "reference picture buffer overrun\n");
            return -1;
        }

        s->refframes[s->refcnt].halfpel[0] = 0;
        s->refframes[s->refcnt].halfpel[1] = 0;
        s->refframes[s->refcnt].halfpel[2] = 0;
        s->refframes[s->refcnt++].frame = s->picture;
    }

    /* Retire frames that were reordered and displayed if they are no
       reference frames either.  */
    for (i = 0; i < s->refcnt; i++) {
        AVFrame *f = &s->refframes[i].frame;

        if (f->reference == 0
            && f->display_picture_number < avctx->frame_number) {
            s->retireframe[s->retirecnt++] = f->display_picture_number;
        }
    }

    for (i = 0; i < s->retirecnt; i++) {
        AVFrame *f;
        int idx, j;

        idx = dirac_reference_frame_idx(s, s->retireframe[i]);
        if (idx == -1) {
            av_log(avctx, AV_LOG_WARNING, "frame to retire #%d not found\n",
                   s->retireframe[i]);
            continue;
        }

        f = &s->refframes[idx].frame;
        /* Do not retire frames that were not displayed yet.  */
        if (f->display_picture_number >= avctx->frame_number) {
            f->reference = 0;
            continue;
        }

        if (f->data[0] != NULL)
            avctx->release_buffer(avctx, f);

        av_free(s->refframes[idx].halfpel[0]);
        av_free(s->refframes[idx].halfpel[1]);
        av_free(s->refframes[idx].halfpel[2]);

        s->refcnt--;

        for (j = idx; j < idx + s->refcnt; j++) {
            s->refframes[j] = s->refframes[j + 1];
        }
    }

    if (s->picture.display_picture_number > avctx->frame_number) {
        int idx;

        if (!s->picture.reference) {
            /* This picture needs to be shown at a later time.  */

            s->refframes[s->refcnt].halfpel[0] = 0;
            s->refframes[s->refcnt].halfpel[1] = 0;
            s->refframes[s->refcnt].halfpel[2] = 0;
            s->refframes[s->refcnt++].frame = s->picture;
        }

        idx = dirac_reference_frame_idx(s, avctx->frame_number);
        if (idx == -1) {
            /* The frame is not yet decoded.  */
            *data_size = 0;
        } else {
            *data_size = sizeof(AVFrame);
            *picture = s->refframes[idx].frame;
        }
    } else {
        /* The right frame at the right time :-) */
        *data_size = sizeof(AVFrame);
        *picture = s->picture;
    }

    if (s->picture.reference
        || s->picture.display_picture_number < avctx->frame_number)
        avcodec_get_frame_defaults(&s->picture);

    return buf_size;
}

AVCodec dirac_decoder = {
    "dirac",
    CODEC_TYPE_VIDEO,
    CODEC_ID_DIRAC,
    sizeof(DiracContext),
    decode_init,
    NULL,
    decode_end,
    dirac_decode_frame,
    CODEC_CAP_DELAY,
    NULL
};
