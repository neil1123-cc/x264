/*****************************************************************************
 * crop.c: crop video filter
 *****************************************************************************
 * Copyright (C) 2010-2025 x264 project
 *
 * Authors: Steven Walters <kemuri9@gmail.com>
 *          James Darnley <james.darnley@gmail.com>
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

#include "video.h"
#include <limits.h>
#include <math.h>

#define NAME "crop"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

cli_vid_filter_t crop_filter;

typedef struct
{
    hnd_t prev_hnd;
    cli_vid_filter_t prev_filter;

    int dims[4]; /* left, top, width, height */
    const x264_cli_csp_t *csp;
} crop_hnd_t;

static void help( int longhelp )
{
    printf( "      "NAME":left,top,right,bottom\n" );
    if( !longhelp )
        return;
    printf( "            removes pixels from the edges of the frame\n" );
}

static int handle_opts( crop_hnd_t *h, video_info_t *info, char **opts, const char * const *optlist )
{
    for( int i = 0; i < 4; i++ )
    {
        char *opt = x264_get_option( optlist[i], opts );
        FAIL_IF_ERROR( !opt, "%s crop value not specified\n", optlist[i] );
        int parsed_dim;
        FAIL_IF_ERROR( x264_otoi_checked( opt, &parsed_dim ),
                       "%s crop value `%s' is invalid\n", optlist[i], opt );
        FAIL_IF_ERROR( parsed_dim < 0, "%s crop value `%s' is less than 0\n", optlist[i], opt );
        int dim_mod = i&1 ? (unsigned)h->csp->mod_height << info->interlaced : h->csp->mod_width;
        FAIL_IF_ERROR( parsed_dim % dim_mod, "%s crop value `%s' is not a multiple of %d\n", optlist[i], opt, dim_mod );
        h->dims[i] = parsed_dim;
    }
    return 0;
}

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
    if( !handle || !*handle || !filter || !info || !filter->get_frame || !filter->release_frame || !filter->free )
        return -1;
    FAIL_IF_ERROR( x264_cli_csp_is_invalid( info->csp ), "invalid csp %d\n", info->csp );
    int64_t crop_alloc_size = sizeof(crop_hnd_t);
    crop_hnd_t *h = calloc( 1, crop_alloc_size );
    if( !h )
        return -1;

    h->csp = x264_cli_get_csp( info->csp );
    static const char * const optlist[] = { "left", "top", "right", "bottom", NULL };
    char **opts = x264_split_options( opt_string, optlist );
    if( !opts )
    {
        free( h );
        return -1;
    }

    int err = handle_opts( h, info, opts, optlist );
    free( opts );
    if( err )
    {
        free( h );
        return -1;
    }

    if( info->width <= 0 || info->height <= 0 ||
        h->dims[0] > INT_MAX - h->dims[2] ||
        h->dims[1] > INT_MAX - h->dims[3] ||
        h->dims[0] + h->dims[2] >= info->width ||
        h->dims[1] + h->dims[3] >= info->height )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "invalid output resolution %dx%d\n",
                      info->width - h->dims[0] - h->dims[2],
                      info->height - h->dims[1] - h->dims[3] );
        free( h );
        return -1;
    }

    h->dims[2] = info->width  - h->dims[0] - h->dims[2];
    h->dims[3] = info->height - h->dims[1] - h->dims[3];
    FAIL_IF_ERROR( h->dims[2] <= 0 || h->dims[3] <= 0, "invalid output resolution %dx%d\n", h->dims[2], h->dims[3] );

    if( info->width != h->dims[2] || info->height != h->dims[3] )
        x264_cli_log( NAME, X264_LOG_INFO, "cropping to %dx%d\n", h->dims[2], h->dims[3] );
    else
    {
        /* do nothing as the user supplied 0s for all the values */
        free( h );
        return 0;
    }
    /* done initializing, overwrite values */
    info->width  = h->dims[2];
    info->height = h->dims[3];

    h->prev_filter = *filter;
    h->prev_hnd = *handle;
    *handle = h;
    *filter = crop_filter;

    return 0;
}

static int crop_mul_intptr( intptr_t *dst, intptr_t a, int b )
{
    if( !dst || b < 0 ||
        (b && ((a > 0 && a > INTPTR_MAX / b) ||
               (a < 0 && a < INTPTR_MIN / b))) )
        return -1;
    *dst = a * b;
    return 0;
}

static int crop_add_intptr( intptr_t *dst, intptr_t a, intptr_t b )
{
    if( !dst ||
        (b > 0 && a > INTPTR_MAX - b) ||
        (b < 0 && a < INTPTR_MIN - b) )
        return -1;
    *dst = a + b;
    return 0;
}

static int crop_scale_dimension( int value, double scale, int *out )
{
    double scaled = value * scale;
    if( !out || !isfinite( scaled ) || scaled < 0.0 || scaled > (double)INT_MAX || scaled != floor( scaled ) )
        return -1;
    int scaled_value = (int)scaled;
    *out = scaled_value;
    return 0;
}

static int crop_abs_stride( int stride )
{
    if( stride == INT_MIN )
        return -1;
    return stride < 0 ? -stride : stride;
}

static int crop_plane_width_bytes( int *byte_width, int width, double width_scale, int depth_factor )
{
    int scaled_width;
    if( !byte_width ||
        crop_scale_dimension( width, width_scale, &scaled_width ) ||
        depth_factor <= 0 || scaled_width > INT_MAX / depth_factor )
        return -1;
    *byte_width = scaled_width * depth_factor;
    return 0;
}

static int crop_plane_row_width( int *row_width, int width, double width_scale, int depth_factor )
{
    return crop_plane_width_bytes( row_width, width, width_scale, depth_factor );
}

static int crop_plane_offset( intptr_t *offset, int stride, int top, double height_scale,
                              int left, double width_scale, int depth_factor )
{
    int scaled_top;
    int scaled_left;
    intptr_t row_offset;
    intptr_t col_offset;
    if( !offset ||
        crop_scale_dimension( top, height_scale, &scaled_top ) ||
        crop_scale_dimension( left, width_scale, &scaled_left ) ||
        crop_mul_intptr( &row_offset, stride, scaled_top ) ||
        crop_mul_intptr( &col_offset, scaled_left, depth_factor ) ||
        crop_add_intptr( offset, row_offset, col_offset ) )
        return -1;
    return 0;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
    crop_hnd_t *h = handle;
    if( !h || !output || !h->csp || !h->prev_hnd ||
        !h->prev_filter.get_frame || !h->prev_filter.release_frame || frame < 0 )
        return -1;
    if( h->prev_filter.get_frame( h->prev_hnd, output, frame ) )
        return -1;

    cli_image_t img = output->img;
    if( x264_cli_csp_is_invalid( img.csp ) || img.planes != h->csp->planes ||
        img.width < h->dims[0] + h->dims[2] ||
        img.height < h->dims[1] + h->dims[3] )
    {
        h->prev_filter.release_frame( h->prev_hnd, output, frame );
        return -1;
    }
    for( int i = 0; i < img.planes; i++ )
        if( !img.plane[i] )
        {
            h->prev_filter.release_frame( h->prev_hnd, output, frame );
            return -1;
        }

    img.width  = h->dims[2];
    img.height = h->dims[3];
    /* shift the plane pointers down 'top' rows and right 'left' columns. */
    for( int i = 0; i < img.planes; i++ )
    {
        int depth_factor = x264_cli_csp_depth_factor( img.csp );
        int stride = crop_abs_stride( img.stride[i] );
        int row_width;
        int col_width;
        intptr_t offset;
        if( crop_plane_row_width( &row_width, img.width, h->csp->width[i], depth_factor ) ||
            crop_plane_width_bytes( &col_width, h->dims[0], h->csp->width[i], depth_factor ) ||
            stride < row_width ||
            col_width > stride - row_width ||
            crop_plane_offset( &offset, img.stride[i], h->dims[1], h->csp->height[i],
                               h->dims[0], h->csp->width[i], depth_factor ) )
        {
            h->prev_filter.release_frame( h->prev_hnd, output, frame );
            return -1;
        }
        img.plane[i] += offset;
    }
    output->img = img;
    return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    crop_hnd_t *h = handle;
    if( !h || !pic || !h->prev_hnd || !h->prev_filter.release_frame )
        return -1;
    /* NO filter should ever have a dependent release based on the plane pointers,
     * so avoid unnecessary unshifting */
    return h->prev_filter.release_frame( h->prev_hnd, pic, frame );
}

static void free_filter( hnd_t handle )
{
    crop_hnd_t *h = handle;
    if( !h )
        return;
    if( h->prev_hnd && h->prev_filter.free )
        h->prev_filter.free( h->prev_hnd );
    free( h );
}

cli_vid_filter_t crop_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
