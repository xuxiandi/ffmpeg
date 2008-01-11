/*
 * AVC helper functions for muxers
 * Copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@smartjog.com>
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
#include "avformat.h"
#include "avio.h"

static uint8_t *avc_find_startcode( uint8_t *p, uint8_t *end )
{
    uint8_t *a = p + 4 - ((long)p & 3);

    for( end -= 3; p < a && p < end; p++ ) {
        if( p[0] == 0 && p[1] == 0 && p[2] == 1 )
            return p;
    }

    for( end -= 3; p < end; p += 4 ) {
        uint32_t x = *(uint32_t*)p;
//      if( (x - 0x01000100) & (~x) & 0x80008000 ) // little endian
//      if( (x - 0x00010001) & (~x) & 0x00800080 ) // big endian
        if( (x - 0x01010101) & (~x) & 0x80808080 ) { // generic
            if( p[1] == 0 ) {
                if( p[0] == 0 && p[2] == 1 )
                    return p-1;
                if( p[2] == 0 && p[3] == 1 )
                    return p;
            }
            if( p[3] == 0 ) {
                if( p[2] == 0 && p[4] == 1 )
                    return p+1;
                if( p[4] == 0 && p[5] == 1 )
                    return p+2;
            }
        }
    }

    for( end += 3; p < end; p++ ) {
        if( p[0] == 0 && p[1] == 0 && p[2] == 1 )
            return p;
    }

    return end + 3;
}

int ff_avc_parse_nal_units(uint8_t *buf_in, uint8_t **buf, int *size)
{
    ByteIOContext *pb;
    uint8_t *p = buf_in;
    uint8_t *end = p + *size;
    uint8_t *nal_start, *nal_end;
    int ret = url_open_dyn_buf(&pb);
    if(ret < 0)
        return ret;

    nal_start = avc_find_startcode(p, end);
    while (nal_start < end) {
        while(!*(nal_start++));
        nal_end = avc_find_startcode(nal_start, end);
        put_be32(pb, nal_end - nal_start);
        put_buffer(pb, nal_start, nal_end - nal_start);
        nal_start = nal_end;
    }
    av_freep(buf);
    *size = url_close_dyn_buf(pb, buf);
    return 0;
}

int ff_isom_write_avcc(ByteIOContext *pb, uint8_t *data, int len)
{
    if (len > 6) {
        /* check for h264 start code */
        if (AV_RB32(data) == 0x00000001) {
            uint8_t *buf=NULL, *end;
            uint32_t sps_size=0, pps_size=0;
            uint8_t *sps=0, *pps=0;

            int ret = ff_avc_parse_nal_units(data, &buf, &len);
            if (ret < 0)
                return ret;
            data = buf;
            end = buf + len;

            /* look for sps and pps */
            while (buf < end) {
                unsigned int size;
                uint8_t nal_type;
                size = AV_RB32(buf);
                nal_type = buf[4] & 0x1f;
                if (nal_type == 7) { /* SPS */
                    sps = buf + 4;
                    sps_size = size;
                } else if (nal_type == 8) { /* PPS */
                    pps = buf + 4;
                    pps_size = size;
                }
                buf += size + 4;
            }
            assert(sps);
            assert(pps);

            put_byte(pb, 1); /* version */
            put_byte(pb, sps[1]); /* profile */
            put_byte(pb, sps[2]); /* profile compat */
            put_byte(pb, sps[3]); /* level */
            put_byte(pb, 0xff); /* 6 bits reserved (111111) + 2 bits nal size length - 1 (11) */
            put_byte(pb, 0xe1); /* 3 bits reserved (111) + 5 bits number of sps (00001) */

            put_be16(pb, sps_size);
            put_buffer(pb, sps, sps_size);
            put_byte(pb, 1); /* number of pps */
            put_be16(pb, pps_size);
            put_buffer(pb, pps, pps_size);
            av_free(data);
        } else {
            put_buffer(pb, data, len);
        }
    }
    return 0;
}
