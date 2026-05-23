/*****************************************************************************
 * internal.c: video filter utilities
 *****************************************************************************
 * Copyright (C) 2010-2025 x264 project
 *
 * Authors: Steven Walters <kemuri9@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@x264.com.
 *****************************************************************************/

#include "internal.h"
#include <limits.h>

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "x264", __VA_ARGS__ )

void x264_cli_plane_copy( uint8_t *dst, int i_dst, uint8_t *src, int i_src, int w, int h )
{
    while( h-- )
    {
        memcpy( dst, src, w );
        dst += i_dst;
        src += i_src;
    }
}

static int x264_cli_scale_dimension( int value, float scale, int *out )
{
    long double scaled = (long double)value * scale;
    if( !out || scaled != scaled || scaled < 0.0 || scaled > INT_MAX )
        return -1;
    int scaled_value = (int)scaled;
    *out = scaled_value;
    return 0;
}

static int x264_cli_pic_plane_dims( cli_pic_t *pic, int csp, int plane, int *width, int *height )
{
    if( !pic || !width || !height )
        return -1;
    int depth = x264_cli_csp_depth_factor( pic->img.csp );
    int plane_width;
    int plane_height;
    if( x264_cli_scale_dimension( pic->img.width, x264_cli_csps[csp].width[plane], &plane_width ) ||
        x264_cli_scale_dimension( pic->img.height, x264_cli_csps[csp].height[plane], &plane_height ) ||
        depth <= 0 || plane_width > INT_MAX / depth )
        return -1;
    *width = plane_width * depth;
    *height = plane_height;
    return 0;
}

static int x264_cli_abs_stride( int stride )
{
    if( stride == INT_MIN )
        return -1;
    return stride < 0 ? -stride : stride;
}

int x264_cli_pic_copy( cli_pic_t *out, cli_pic_t *in )
{
    if( !out || !in )
        return -1;
    int csp = in->img.csp & X264_CSP_MASK;
    int planes = in->img.planes;
    FAIL_IF_ERROR( x264_cli_csp_is_invalid( in->img.csp ), "invalid colorspace arg %d\n", in->img.csp );
    FAIL_IF_ERROR( in->img.csp != out->img.csp || in->img.height != out->img.height
                || in->img.width != out->img.width, "incompatible frame properties\n" );
    FAIL_IF_ERROR( planes != x264_cli_csps[csp].planes || planes != out->img.planes, "invalid frame plane count\n" );
    for( int i = 0; i < planes; i++ )
    {
        int width, height;
        int in_stride = x264_cli_abs_stride( in->img.stride[i] );
        int out_stride = x264_cli_abs_stride( out->img.stride[i] );
        FAIL_IF_ERROR( x264_cli_pic_plane_dims( in, csp, i, &width, &height ) ||
                       in_stride < 0 || out_stride < 0 ||
                       (width && height && (!out->img.plane[i] || !in->img.plane[i] ||
                                            in_stride < width || out_stride < width)),
                       "invalid frame plane data\n" );
    }

    out->duration = in->duration;
    out->pts = in->pts;
    out->opaque = in->opaque;

    for( int i = 0; i < planes; i++ )
    {
        int width, height;
        if( x264_cli_pic_plane_dims( in, csp, i, &width, &height ) )
            return -1;
        x264_cli_plane_copy( out->img.plane[i], out->img.stride[i], in->img.plane[i],
                             in->img.stride[i], width, height );
    }
    return 0;
}
