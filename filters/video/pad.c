/*****************************************************************************
 * pad.c: frame padding filter
 *****************************************************************************
 * Copyright (C) 2003-2014 x264 project
 *
 * Authors: James Darnley <james.darnley@gmail.com>
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
 *****************************************************************************/

/* TODO/ideas:
    - larger choice of padding color:
        - 16bit
        - YcbCr
        a requirement is that it must produce visually similar output
        regardless of the current colorspace and bit depth.
    - correct yuv<->rgb conversion based on VUI, fullrange, colormatrix, etc.
*/

#include "internal.h"
#include "video.h"
#include <limits.h>
#include <math.h>
#define NAME "pad"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

/* From the H.264 standard, Annex E, Table E-5 */
/*
static const double matrix_coeffs[][2] = {
    [1] = { 0.2126, 0.0722 },
    [4] = { 0.30,   0.11   },
    [5] = { 0.299,  0.114  },
    [6] = { 0.299,  0.114  },
    [7] = { 0.212,  0.087  }
};*/
// Not used accorded GCC warning

/* Calculated from the above using equations E-13, E-14, E-15 */
static const double rgb_yuv_coeffs[][3][3] = {
    [1] = { { +0.212600, +0.715200, +0.072200 },
            { -0.114572, -0.385428, +0.500000 },
            { +0.500000, -0.454153, -0.045847 } },
    [4] = { { +0.300000, +0.590000, +0.110000 },
            { -0.168539, -0.331461, +0.500000 },
            { +0.500000, -0.421429, -0.078571 } },
    [5] = { { +0.299000, +0.587000, +0.114000 },
            { -0.168736, -0.331264, +0.500000 },
            { +0.500000, -0.418688, -0.081312 } },
    [6] = { { +0.299000, +0.587000, +0.114000 },
            { -0.168736, -0.331264, +0.500000 },
            { +0.500000, -0.418688, -0.081312 } },
    [7] = { { +0.212000, +0.701000, +0.087000 },
            { -0.116101, -0.383899, +0.500000 },
            { +0.500000, -0.444797, -0.055203 } }
};

cli_vid_filter_t pad_filter;

typedef struct {
    hnd_t prev_handle;
    cli_vid_filter_t prev_filter;
    int width;
    int height;
    int cols;
    int rows;
    uint16_t color[3];
    cli_pic_t buffer;
    const x264_cli_csp_t *csp;
} pad_handle_t;

#define dot_product( a, b ) \
    ((a[0])*(b[0]) + (a[1])*(b[1]) + (a[2])*(b[2]))

static uint16_t get_luma( int *rgb, const double *coeffs, int fr, int bits )
{
    int ret;
    if( fr )
        ret = ((1u<<bits)-1) * dot_product( rgb, coeffs ) / 255.0 + 0.5;
    else
        ret = ((1u<<(bits-8)) * (219.0/255.0 * dot_product( rgb, coeffs ) + 16)) + 0.5;
    return x264_clip3( ret, 0, (1u<<bits)-1 );
}

static uint16_t get_chroma( int *rgb, const double *coeffs, int fr, int bits )
{
    int ret;
    if( fr )
        ret = ((1u<<bits)-1) * dot_product( rgb, coeffs ) / 255.0 + (1u<<(bits-1)) + 0.5;
    else
        ret = ((1u<<(bits-8)) * (224.0/255.0 * dot_product( rgb, coeffs ) + 128)) + 0.5;
    return x264_clip3( ret, 0, (1u<<bits)-1 );
}

static int get_colors( int *rgb, pad_handle_t *h, x264_param_t *param )
{
    int cm;
    int bits = (h->buffer.img.csp&X264_CSP_HIGH_DEPTH) ? 16 : 8;
    int fr = param->vui.b_fullrange;

    /* Ensure a supported color matrix.   */
    if( (param->vui.i_colmatrix >= 4 && param->vui.i_colmatrix <= 7) || param->vui.i_colmatrix == 1 )
        cm = param->vui.i_colmatrix;
    else
    {
        if( h->width >= 1280 || h->height >= 720 )
            cm = 1;
        else
            cm = 5;
    }

    switch( h->buffer.img.csp )
    {
        case X264_CSP_HIGH_DEPTH|X264_CSP_I400:
        case X264_CSP_I400:
            h->color[0] = get_luma( rgb, rgb_yuv_coeffs[cm][0], fr, bits );
			break;
        case X264_CSP_HIGH_DEPTH|X264_CSP_I420:
        case X264_CSP_HIGH_DEPTH|X264_CSP_I422:
        case X264_CSP_HIGH_DEPTH|X264_CSP_I444:
        case X264_CSP_HIGH_DEPTH|X264_CSP_NV12:
        case X264_CSP_I420:
        case X264_CSP_I422:
        case X264_CSP_I444:
        case X264_CSP_NV12:
            h->color[0] = get_luma( rgb, rgb_yuv_coeffs[cm][0], fr, bits );
            h->color[1] = get_chroma( rgb, rgb_yuv_coeffs[cm][1], fr, bits );
            h->color[2] = get_chroma( rgb, rgb_yuv_coeffs[cm][2], fr, bits );
            break;
        case X264_CSP_HIGH_DEPTH|X264_CSP_YV12:
        case X264_CSP_YV12:
            h->color[0] = get_luma( rgb, rgb_yuv_coeffs[cm][0], fr, bits );
            h->color[1] = get_chroma( rgb, rgb_yuv_coeffs[cm][2], fr, bits );
            h->color[2] = get_chroma( rgb, rgb_yuv_coeffs[cm][1], fr, bits );
            break;
        case X264_CSP_HIGH_DEPTH|X264_CSP_BGR:
        case X264_CSP_BGR:
            h->color[0] = (unsigned)rgb[2] << (bits-8) | rgb[2];
            h->color[1] = (unsigned)rgb[1] << (bits-8) | rgb[1];
            h->color[2] = (unsigned)rgb[0] << (bits-8) | rgb[0];
            break;
        case X264_CSP_HIGH_DEPTH|X264_CSP_BGRA:
        case X264_CSP_HIGH_DEPTH|X264_CSP_RGB:
        case X264_CSP_BGRA:
        case X264_CSP_RGB:
            h->color[0] = (unsigned)rgb[0] << (bits-8) | rgb[0];
            h->color[1] = (unsigned)rgb[1] << (bits-8) | rgb[1];
            h->color[2] = (unsigned)rgb[2] << (bits-8) | rgb[2];
            break;
        default:
            return 1;
    }
    return 0;
}

static void set_frame_colors( cli_pic_t *pic, uint16_t *color )
{
    const x264_cli_csp_t *csp = x264_cli_get_csp( pic->img.csp );
    uint16_t *plane;
    int i, i_max, j, j_max;
    switch( pic->img.csp )
    {
/* 16-bit colors */
        case X264_CSP_HIGH_DEPTH|X264_CSP_I400:
		case X264_CSP_HIGH_DEPTH|X264_CSP_I420:
        case X264_CSP_HIGH_DEPTH|X264_CSP_I422:
        case X264_CSP_HIGH_DEPTH|X264_CSP_I444:
        case X264_CSP_HIGH_DEPTH|X264_CSP_YV12:
            for( i = 0; i < pic->img.planes; i++ )
            {
                plane = (uint16_t *)pic->img.plane[i];
                j_max = pic->img.height * csp->height[i] * pic->img.stride[i]/2;
                for( j = 0; j < j_max; j++ )
                    plane[j] = color[i];
            }
            break;
        case X264_CSP_HIGH_DEPTH|X264_CSP_BGR:
        case X264_CSP_HIGH_DEPTH|X264_CSP_BGRA:
        case X264_CSP_HIGH_DEPTH|X264_CSP_RGB:
            plane = (uint16_t *)pic->img.plane[0];
            i_max = pic->img.height * pic->img.stride[0]/2;
            for( i = 0; i < i_max; i += csp->width[0] )
            {
                plane[i]   = color[0];
                plane[i+1] = color[1];
                plane[i+2] = color[2];
            }
            break;
        case X264_CSP_HIGH_DEPTH|X264_CSP_NV12:
            plane = (uint16_t *)pic->img.plane[0];
            i_max = pic->img.height * pic->img.stride[0]/2;
            for( i = 0; i < i_max; i++ )
                plane[i] = color[0];
            plane = (uint16_t *)pic->img.plane[1];
            i_max = pic->img.stride[0] * pic->img.height / 4;
            for( i = 0; i < i_max; i += 2 )
            {
                plane[i]   = color[1];
                plane[i+1] = color[2];
            }
            break;
/* 8-bit colors */
        case X264_CSP_I400:
		case X264_CSP_I420:
        case X264_CSP_I422:
        case X264_CSP_I444:
        case X264_CSP_YV12:
            for( i = 0; i < pic->img.planes; i++ )
                memset( pic->img.plane[i], color[i],
                        pic->img.height * csp->height[i] * pic->img.stride[i] );
            break;
        case X264_CSP_BGR:
        case X264_CSP_BGRA:
        case X264_CSP_RGB:
            i_max = pic->img.stride[0] * pic->img.height;
            for( i = 0; i < i_max; i += csp->width[0] )
            {
                pic->img.plane[0][i]   = color[0];
                pic->img.plane[0][i+1] = color[1];
                pic->img.plane[0][i+2] = color[2];
            }
            break;
        case X264_CSP_NV12:
            memset( pic->img.plane[0], color[0],
                    pic->img.height * pic->img.stride[0] );
            i_max = pic->img.stride[0] * pic->img.height / 2;
            for( i = 0; i < i_max; i += 2 )
            {
                pic->img.plane[1][i]   = color[1];
                pic->img.plane[1][i+1] = color[2];
            }
            break;
    }
}

static int handle_opts(int *arg, const x264_cli_csp_t *csp, video_info_t *info, char **opts, const char * const *optlist )
{
	char *opt;
	
    for( int i = 0; i < 9; i++ )
    {
        int mod = i&1 ? (unsigned)csp->mod_height << info->interlaced : csp->mod_width;
        opt = x264_get_option( optlist[i], opts );
        int parsed_arg = 0;
        if( opt )
            FAIL_IF_ERROR( x264_otoi_checked( opt, &parsed_arg ),
                           "%s pad value '%s' is invalid\n", optlist[i], opt );
        FAIL_IF_ERROR( i < 6 && parsed_arg < 0,
                       "%s pad value '%s' is less than 0\n",
                       optlist[i], x264_otos( opt, "<unset>" ) );
        FAIL_IF_ERROR( i >= 6 && (parsed_arg < 0 || parsed_arg > 255),
                       "%s pad color '%s' is outside the 0..255 range\n",
                       optlist[i], x264_otos( opt, "<unset>" ) );
        FAIL_IF_ERROR( i < 6 && parsed_arg % mod,
                       "%s pad value '%s' is not a multiple of %d\n",
                       optlist[i], x264_otos( opt, "<unset>" ), mod );
        arg[i] = parsed_arg;
    }
	return 0;	
}

static int pad_add3_int( int a, int b, int c, int *out )
{
    if( a < 0 || b < 0 || c < 0 || a > INT_MAX - b || a + b > INT_MAX - c )
        return -1;
    *out = a + b + c;
    return 0;
}

static int pad_mul_int( int *out, int a, int b )
{
    if( !out || a < 0 || b < 0 || (b && a > INT_MAX / b) )
        return -1;
    *out = a * b;
    return 0;
}

static int pad_add_int( int *out, int a, int b )
{
    if( !out || a < 0 || b < 0 || a > INT_MAX - b )
        return -1;
    *out = a + b;
    return 0;
}

static int pad_scale_dimension( int value, double scale, int *out )
{
    double scaled = value * scale;
    if( !out || !isfinite( scaled ) || scaled < 0.0 || scaled > (double)INT_MAX || scaled != floor( scaled ) )
        return -1;
    *out = (int)scaled;
    return 0;
}

static int pad_plane_copy_params( int *offset, int *width, int *height,
                                  int input_width, int input_height, int dst_stride,
                                  int cols, int rows, double width_scale, double height_scale,
                                  int depth_factor )
{
    int scaled_input_width;
    int scaled_input_height;
    int scaled_cols;
    int scaled_rows;
    int byte_width;
    int byte_col_offset;
    int row_offset;
    if( pad_scale_dimension( input_width, width_scale, &scaled_input_width ) ||
        pad_scale_dimension( input_height, height_scale, &scaled_input_height ) ||
        pad_scale_dimension( cols, width_scale, &scaled_cols ) ||
        pad_scale_dimension( rows, height_scale, &scaled_rows ) ||
        pad_mul_int( &byte_width, scaled_input_width, depth_factor ) ||
        pad_mul_int( &byte_col_offset, scaled_cols, depth_factor ) ||
        pad_mul_int( &row_offset, scaled_rows, dst_stride ) ||
        pad_add_int( offset, row_offset, byte_col_offset ) )
        return -1;
    *width = byte_width;
    *height = scaled_input_height;
    return 0;
}

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info,
                 x264_param_t *param, char *opt_string )
{
    int arg[9];
    if( !handle || !*handle || !filter || !info || !param ||
        !filter->get_frame || !filter->release_frame || !filter->free )
        return -1;
    const x264_cli_csp_t *csp = x264_cli_get_csp( info->csp );
    FAIL_IF_ERROR( !csp || info->width <= 0 || info->height <= 0,
                   "invalid input properties\n" );
    static const char * const optlist[] = { "left", "top", "right", "bottom", "width",
                                     "height", "red", "green", "blue", NULL };
    char **opts = x264_split_options( opt_string, optlist );
    if( !opts )
        return -1;

    pad_handle_t *h = calloc( 1, sizeof(pad_handle_t) );
    if( !h )
    {
        free( opts );
        return -1;
    }
	
    int err = handle_opts( arg, csp, info, opts, optlist );
    free( opts );
    if( err )
    {
        free( h );
        return -1;
    }

/* For sanity! */
#define round_a_to_b(a,b) (((a)+(b)/2)/(b))*(b)
#define left   arg[0]
#define top    arg[1]
#define right  arg[2]
#define bottom arg[3]
#define WIDTH  arg[4]
#define HEIGHT arg[5]
    int padded_width;
    int padded_height;
    if( pad_add3_int( info->width, left, right, &padded_width ) ||
        pad_add3_int( info->height, top, bottom, &padded_height ) )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "requested padding is too large\n" );
        free( h );
        return -1;
    }
    if( WIDTH && WIDTH < padded_width )
    {
        x264_cli_log( NAME, X264_LOG_ERROR,
                   "requested width (%d) is less than requested padding (%d + %d + %d)\n",
                   WIDTH, info->width, left, right );
        free( h );
        return -1;
    }

    if( HEIGHT && HEIGHT < padded_height )
    {
        x264_cli_log( NAME, X264_LOG_ERROR,
                   "requested height (%d) is less than requested padding (%d + %d + %d)\n",
                   HEIGHT, info->height, top, bottom );
        free( h );
        return -1;
    }

    h->width = (WIDTH) ? WIDTH : padded_width;
    h->height = (HEIGHT) ? HEIGHT : padded_height;

    h->cols = (left) ? left
            : (right) ? h->width - right - info->width
            : (h->width - info->width)/2;
    h->cols = round_a_to_b( h->cols, csp->mod_width );

    h->rows = (top) ? top
            : (bottom) ? h->height - bottom - info->height
            : (h->height - info->height)/2;
    h->rows = round_a_to_b( h->rows, csp->mod_height );
#undef left
#undef top
#undef right
#undef bottom
#undef WIDTH
#undef HEIGHT
#undef round_a_to_b

    if( h->width <= 0 || h->height <= 0 || h->width > MAX_RESOLUTION || h->height > MAX_RESOLUTION )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "invalid output resolution %dx%d\n", h->width, h->height );
        free( h );
        return -1;
    }

    if( h->width == info->width && h->height == info->height )
    {
        free(h);
        return 0;
    }

    if( x264_cli_pic_alloc( &h->buffer, info->csp, h->width, h->height ) )
    {
        free( h );
        return -1;
    }

    if( get_colors( arg+6, h, param ) )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "unsupported colorspace\n" );
        x264_cli_pic_clean( &h->buffer );
        free( h );
        return -1;
    }

    set_frame_colors( &h->buffer, h->color );

    x264_cli_log( NAME, X264_LOG_INFO,
                  "expanding frame to %dx%d, picture starting at (%d,%d)\n",
                  h->width, h->height, h->cols, h->rows );
    x264_cli_log( NAME, X264_LOG_INFO, "(%d,%d,%d) -> (%d,%d,%d)\n",
                  arg[6], arg[7], arg[8],
                  h->color[0], h->color[1], h->color[2] );

    info->width  = h->width;
    info->height = h->height;
    h->prev_filter = *filter;
    h->prev_handle = *handle;
    h->csp = csp;
    *handle = h;
    *filter = pad_filter;

    return 0;
}

static int get_frame( hnd_t handle, cli_pic_t *out, int frame )
{
    pad_handle_t *h = handle;
    int depth_factor;
    cli_pic_t in;
    cli_pic_t staged;

    if( !h || !out || !h->csp || !h->prev_handle ||
        !h->prev_filter.get_frame || !h->prev_filter.release_frame || frame < 0 )
        return -1;
    if( h->prev_filter.get_frame( h->prev_handle, &in, frame ) )
        return -1;

    depth_factor = x264_cli_csp_depth_factor( in.img.csp );

    staged = h->buffer;
    staged.pts = in.pts;
    staged.duration = in.duration;

    for( int i = 0; i < in.img.planes; i++ )
    {
        int stride[2]  = { in.img.stride[i],
                           staged.img.stride[i] };
        int in_dim[2];
        int offset;
        if( pad_plane_copy_params( &offset, &in_dim[0], &in_dim[1],
                                   in.img.width, in.img.height, stride[1],
                                   h->cols, h->rows, h->csp->width[i],
                                   h->csp->height[i], depth_factor ) )
        {
            h->prev_filter.release_frame( h->prev_handle, &in, frame );
            return -1;
        }

        x264_cli_plane_copy( staged.img.plane[i]+offset, stride[1],
                             in.img.plane[i], stride[0], in_dim[0], in_dim[1] );
    }

    if( h->prev_filter.release_frame( h->prev_handle, &in, frame ) )
        return -1;

    *out = staged;
    return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    if( !handle || !pic )
        return -1;
    /* pad_handle_t *h = handle;
    set_frame_colors( &h->buffer, h->color ); */
    return 0;
}

static void free_filter( hnd_t handle )
{
    pad_handle_t *h = handle;
    if( !h )
        return;
    if( h->prev_handle && h->prev_filter.free )
        h->prev_filter.free( h->prev_handle );
    x264_cli_pic_clean( &h->buffer );
    free( h );
}

static void help( int longhelp )
{
    printf(
"      "NAME":[left][,top][,right][,bottom][,width][,height][,red][,green][,blue]\n" );
    if( !longhelp )
        return;
    printf(
"            adds pixels to the frame edge\n"
"            default values for red, green and blue are 0\n" );
}

cli_vid_filter_t pad_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
