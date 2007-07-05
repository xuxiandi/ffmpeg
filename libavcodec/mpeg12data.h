/*
 * MPEG1/2 tables
 * copyright (c) 2000,2001 Fabrice Bellard
 * copyright (c) 2002-2004 Michael Niedermayer <michaelni@gmx.at>
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
 * @file mpeg12data.h
 * MPEG1/2 tables.
 */

#ifndef AVCODEC_MPEG12DATA_H
#define AVCODEC_MPEG12DATA_H

#include <stdint.h>
#include "mpegvideo.h"

extern const uint16_t vlc_dc_lum_code[12];
extern const unsigned char vlc_dc_lum_bits[12];
extern const uint16_t vlc_dc_chroma_code[12];
extern const unsigned char vlc_dc_chroma_bits[12];

extern RLTable rl_mpeg1;
extern RLTable rl_mpeg2;

extern const uint8_t mbAddrIncrTable[36][2];
extern const uint8_t mbPatTable[64][2];

extern const uint8_t mbMotionVectorTable[17][2];

extern const float mpeg1_aspect[16];
extern const AVRational mpeg2_aspect[16];

#endif // AVCODEC_MPEG12DATA_H
