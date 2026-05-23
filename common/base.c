/*****************************************************************************
 * base.c: misc common functions (bit depth independent)
 *****************************************************************************
 * Copyright (C) 2003-2025 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
 *          Laurent Aimar <fenrir@via.ecp.fr>
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

#include "base.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>

#if HAVE_MALLOC_H
#include <malloc.h>
#endif
#if HAVE_THP
#include <sys/mman.h>
#endif

#define X264_ISDIGIT(x) isdigit((unsigned char)(x))

/****************************************************************************
 * x264_reduce_fraction:
 ****************************************************************************/
#define REDUCE_FRACTION( name, type )\
void name( type *n, type *d )\
{                   \
    if( !n || !d )  \
        return;     \
    type a = *n;    \
    type b = *d;    \
    type c;         \
    if( !a || !b )  \
        return;     \
    c = a % b;      \
    while( c )      \
    {               \
        a = b;      \
        b = c;      \
        c = a % b;  \
    }               \
    *n /= b;        \
    *d /= b;        \
}

REDUCE_FRACTION( x264_reduce_fraction  , uint32_t )
REDUCE_FRACTION( x264_reduce_fraction64, uint64_t )

/****************************************************************************
 * x264_ntsc_fps:
 ****************************************************************************/
void x264_ntsc_fps( uint32_t *fps_num, uint32_t *fps_den )
{
    if( !fps_num || !fps_den )
        return;
    if( !*fps_num || !*fps_den )
        return;

#define X264_NTSC_TIMEBASE 1001
    const double f_ntsc_mod6_quotient = (double) X264_NTSC_TIMEBASE / 6.0;
    const double f_fps                = (double) *fps_num / (double) *fps_den;
    const double f_interval           = 1000.0 / f_fps;

    const long double f_nearest_ntsc_mod6_num = (long double)f_ntsc_mod6_quotient / (long double)f_interval + 0.5L;
    if( f_nearest_ntsc_mod6_num < 1.0L ||
        f_nearest_ntsc_mod6_num >= (long double)( UINT32_MAX / 6000 ) + 1.0L )
        return;
    const uint32_t i_nearest_ntsc_mod6_num = (uint32_t)f_nearest_ntsc_mod6_num;
    if( !i_nearest_ntsc_mod6_num )
        return;
    const double   f_nearest_ntsc_interval = f_ntsc_mod6_quotient / (double) i_nearest_ntsc_mod6_num;

    if ( fabs ( f_interval - f_nearest_ntsc_interval ) < ( 1.0 / ( f_fps + 1.0 ) ) )    // error < ( 1 / fps + 1 )
    {
        *fps_num = i_nearest_ntsc_mod6_num * 6000;
        *fps_den = X264_NTSC_TIMEBASE;
    }
#undef X264_NTSC_TIMEBASE

    return;
}

/****************************************************************************
 * x264_log:
 ****************************************************************************/
void x264_log_default( void *p_unused, int i_level, const char *psz_fmt, va_list arg )
{
    char *psz_prefix;
    switch( i_level )
    {
        case X264_LOG_ERROR:
            psz_prefix = "error";
            break;
        case X264_LOG_WARNING:
            psz_prefix = "warning";
            break;
        case X264_LOG_INFO:
            psz_prefix = "info";
            break;
        case X264_LOG_DEBUG:
            psz_prefix = "debug";
            break;
        default:
            psz_prefix = "unknown";
            break;
    }
    fprintf( stderr, "x264 [%s]: ", psz_prefix );
    x264_vfprintf( stderr, psz_fmt, arg );
}

void x264_log_internal( int i_level, const char *psz_fmt, ... )
{
    va_list arg;
    va_start( arg, psz_fmt );
    x264_log_default( NULL, i_level, psz_fmt, arg );
    va_end( arg );
}

/****************************************************************************
 * x264_malloc:
 ****************************************************************************/
void *x264_malloc( int64_t i_size )
{
#define HUGE_PAGE_SIZE 2*1024*1024
#define HUGE_PAGE_THRESHOLD HUGE_PAGE_SIZE*7/8 /* FIXME: Is this optimal? */
    if( i_size < 0 || (uint64_t)i_size > (SIZE_MAX - HUGE_PAGE_SIZE) /*|| (uint64_t)i_size > (SIZE_MAX - NATIVE_ALIGN - sizeof(void **))*/ )
    {
        x264_log_internal( X264_LOG_ERROR, "invalid size of malloc: %"PRId64"\n", i_size );
        return NULL;
    }
    uint8_t *align_buf = NULL;
#if HAVE_MALLOC_H
#if HAVE_THP
    /* Attempt to allocate huge pages to reduce TLB misses. */
    if( i_size >= HUGE_PAGE_THRESHOLD )
    {
        align_buf = memalign( HUGE_PAGE_SIZE, i_size );
        if( align_buf )
        {
            /* Round up to the next huge page boundary if we are close enough. */
            size_t madv_size = (i_size + HUGE_PAGE_SIZE - HUGE_PAGE_THRESHOLD) & ~(HUGE_PAGE_SIZE-1);
            madvise( align_buf, madv_size, MADV_HUGEPAGE );
        }
    }
    else
#endif
        align_buf = memalign( NATIVE_ALIGN, i_size );
#else
    int64_t align_alloc_size = i_size + (NATIVE_ALIGN-1) + sizeof(void **);
    uint8_t *buf = malloc( align_alloc_size );
    if( buf )
    {
        align_buf = buf + (NATIVE_ALIGN-1) + sizeof(void **);
        align_buf -= (intptr_t) align_buf & (NATIVE_ALIGN-1);
        *( (void **) ( align_buf - sizeof(void **) ) ) = buf;
    }
#endif
    if( !align_buf )
        x264_log_internal( X264_LOG_ERROR, "malloc of size %"PRId64" failed\n", i_size );
    return align_buf;
#undef HUGE_PAGE_SIZE
#undef HUGE_PAGE_THRESHOLD
}

/****************************************************************************
 * x264_free:
 ****************************************************************************/
void x264_free( void *p )
{
    if( p )
    {
#if HAVE_MALLOC_H
        free( p );
#else
        free( *( ( ( void **) p ) - 1 ) );
#endif
    }
}

/****************************************************************************
 * x264_slurp_file:
 ****************************************************************************/
char *x264_slurp_file( const char *filename )
{
    int b_error = 0;
    int64_t i_size;
    char *buf;
    if( !filename )
        return NULL;
    FILE *fh = x264_fopen( filename, "rb" );
    if( !fh )
        return NULL;

    b_error |= fseek( fh, 0, SEEK_END ) < 0;
    b_error |= ( i_size = ftell( fh ) ) <= 0;
    if( WORD_SIZE == 4 )
        b_error |= i_size > INT32_MAX;
    b_error |= i_size > INT64_MAX - 2;
    b_error |= fseek( fh, 0, SEEK_SET ) < 0;
    if( b_error )
        goto error;

    int64_t slurp_alloc_size = i_size + 2;
    buf = x264_malloc( slurp_alloc_size );
    if( !buf )
        goto error;

    size_t file_size = (size_t)i_size;
    b_error |= fread( buf, 1, file_size, fh ) != file_size;
    b_error |= fclose( fh );
    if( b_error )
    {
        x264_free( buf );
        return NULL;
    }

    if( buf[file_size-1] != '\n' )
        buf[file_size++] = '\n';
    buf[file_size] = '\0';

    return buf;
error:
    if( fclose( fh ) )
        x264_log_internal( X264_LOG_ERROR, "failed to close slurped file `%s'\n", filename );
    return NULL;
}

/****************************************************************************
 * x264_param_strdup:
 ****************************************************************************/
typedef struct {
    int size;
    int count;
    void *ptr[];
} strdup_buffer;

#define BUFFER_OFFSET (int)offsetof(strdup_buffer, ptr)
#define BUFFER_DEFAULT_SIZE 16

char *x264_param_strdup( x264_param_t *param, const char *src )
{
    if( !param || !src )
        goto fail;
    strdup_buffer *buf = param->opaque;
    if( !buf )
    {
        int64_t strdup_buffer_alloc_size = BUFFER_OFFSET + BUFFER_DEFAULT_SIZE * (int64_t)sizeof(void *);
        buf = malloc( strdup_buffer_alloc_size );
        if( !buf )
            goto fail;
        buf->size = BUFFER_DEFAULT_SIZE;
        buf->count = 0;
        param->opaque = buf;
    }
    else if( buf->count == buf->size )
    {
        if( buf->size > (INT_MAX - BUFFER_OFFSET) / 2 / (int)sizeof(void *) )
            goto fail;
        int new_size = buf->size * 2;
        int64_t strdup_buffer_realloc_size = BUFFER_OFFSET + new_size * (int64_t)sizeof(void *);
        buf = realloc( buf, strdup_buffer_realloc_size );
        if( !buf )
            goto fail;
        buf->size = new_size;
        param->opaque = buf;
    }
    char *res = strdup( src );
    if( !res )
        goto fail;
    buf->ptr[buf->count++] = res;
    return res;
fail:
    x264_log_internal( X264_LOG_ERROR, "x264_param_strdup failed\n" );
    return NULL;
}

/****************************************************************************
 * x264_param_cleanup:
 ****************************************************************************/
REALIGN_STACK void x264_param_cleanup( x264_param_t *param )
{
    if( !param )
        return;
    strdup_buffer *buf = param->opaque;
    if( buf )
    {
        for( int i = 0; i < buf->count; i++ )
            free( buf->ptr[i] );
        free( buf );
        param->opaque = NULL;
    }
}

/****************************************************************************
 * x264_picture_init:
 ****************************************************************************/
REALIGN_STACK void x264_picture_init( x264_picture_t *pic )
{
    if( !pic )
        return;
    memset( pic, 0, sizeof( x264_picture_t ) );
    pic->i_type = X264_TYPE_AUTO;
    pic->i_qpplus1 = X264_QP_AUTO;
    pic->i_pic_struct = PIC_STRUCT_AUTO;
}

/****************************************************************************
 * x264_picture_alloc:
 ****************************************************************************/
REALIGN_STACK int x264_picture_alloc( x264_picture_t *pic, int i_csp, int i_width, int i_height )
{
    typedef struct
    {
        int planes;
        int width_fix8[3];
        int height_fix8[3];
    } x264_csp_tab_t;

    static const x264_csp_tab_t csp_tab[] =
    {
        [X264_CSP_I400] = { 1, { 256*1 },               { 256*1 }               },
        [X264_CSP_I420] = { 3, { 256*1, 256/2, 256/2 }, { 256*1, 256/2, 256/2 } },
        [X264_CSP_YV12] = { 3, { 256*1, 256/2, 256/2 }, { 256*1, 256/2, 256/2 } },
        [X264_CSP_NV12] = { 2, { 256*1, 256*1 },        { 256*1, 256/2 },       },
        [X264_CSP_NV21] = { 2, { 256*1, 256*1 },        { 256*1, 256/2 },       },
        [X264_CSP_I422] = { 3, { 256*1, 256/2, 256/2 }, { 256*1, 256*1, 256*1 } },
        [X264_CSP_YV16] = { 3, { 256*1, 256/2, 256/2 }, { 256*1, 256*1, 256*1 } },
        [X264_CSP_NV16] = { 2, { 256*1, 256*1 },        { 256*1, 256*1 },       },
        [X264_CSP_YUYV] = { 1, { 256*2 },               { 256*1 },              },
        [X264_CSP_UYVY] = { 1, { 256*2 },               { 256*1 },              },
        [X264_CSP_I444] = { 3, { 256*1, 256*1, 256*1 }, { 256*1, 256*1, 256*1 } },
        [X264_CSP_YV24] = { 3, { 256*1, 256*1, 256*1 }, { 256*1, 256*1, 256*1 } },
        [X264_CSP_BGR]  = { 1, { 256*3 },               { 256*1 },              },
        [X264_CSP_BGRA] = { 1, { 256*4 },               { 256*1 },              },
        [X264_CSP_RGB]  = { 1, { 256*3 },               { 256*1 },              },
    };

    int csp = i_csp & X264_CSP_MASK;
    if( !pic || i_width <= 0 || i_height <= 0 ||
        csp <= X264_CSP_NONE || csp >= X264_CSP_MAX || csp == X264_CSP_V210 )
        return -1;

    x264_picture_t staged_pic;
    x264_picture_init( &staged_pic );
    staged_pic.img.i_csp = i_csp;
    staged_pic.img.i_plane = csp_tab[csp].planes;
    int depth_factor = i_csp & X264_CSP_HIGH_DEPTH ? 2 : 1;
    int64_t plane_offset[3] = {0};
    int64_t frame_size = 0;
    for( int i = 0; i < staged_pic.img.i_plane; i++ )
    {
        int64_t plane_width = ((int64_t)i_width * csp_tab[csp].width_fix8[i]) >> 8;
        int64_t plane_height = ((int64_t)i_height * csp_tab[csp].height_fix8[i]) >> 8;
        if( plane_width > INT64_MAX / depth_factor )
            return -1;
        int64_t stride = plane_width * depth_factor;
        if( stride > INT_MAX || (plane_height && stride > INT64_MAX / plane_height) )
            return -1;
        int64_t plane_size = plane_height * stride;
        if( frame_size > INT64_MAX - plane_size )
            return -1;
        int plane_stride = (int)stride;
        staged_pic.img.i_stride[i] = plane_stride;
        plane_offset[i] = frame_size;
        frame_size += plane_size;
    }
    int64_t frame_alloc_size = frame_size;
    staged_pic.img.plane[0] = x264_malloc( frame_alloc_size );
    if( !staged_pic.img.plane[0] )
        return -1;
    for( int i = 1; i < staged_pic.img.i_plane; i++ )
        staged_pic.img.plane[i] = staged_pic.img.plane[0] + plane_offset[i];
    *pic = staged_pic;
    return 0;
}

/****************************************************************************
 * x264_picture_clean:
 ****************************************************************************/
REALIGN_STACK void x264_picture_clean( x264_picture_t *pic )
{
    if( !pic )
        return;
    x264_free( pic->img.plane[0] );

    /* just to be safe */
    memset( pic, 0, sizeof( x264_picture_t ) );
}

/****************************************************************************
 * x264_param_default:
 ****************************************************************************/
REALIGN_STACK void x264_param_default( x264_param_t *param )
{
    if( !param )
        return;

    /* */
    memset( param, 0, sizeof( x264_param_t ) );

    /* CPU autodetect */
    param->cpu = x264_cpu_detect();
    param->i_threads = X264_THREADS_AUTO;
    param->i_lookahead_threads = X264_THREADS_AUTO;
    param->b_deterministic = 1;
    param->i_sync_lookahead = X264_SYNC_LOOKAHEAD_AUTO;

    /* Video properties */
    param->i_csp           = X264_CHROMA_FORMAT ? X264_CHROMA_FORMAT : X264_CSP_I420;
    param->i_width         = 0;
    param->i_height        = 0;
    param->vui.i_sar_width = 0;
    param->vui.i_sar_height= 0;
    param->vui.i_overscan  = 0;  /* undef */
    param->vui.i_vidformat = 5;  /* undef */
    param->vui.b_fullrange = -1; /* default depends on input */
    param->vui.i_colorprim = 2;  /* undef */
    param->vui.i_transfer  = 2;  /* undef */
    param->vui.i_colmatrix = -1; /* default depends on input */
    param->vui.i_chroma_loc= 0;  /* left center */
    param->i_fps_num       = 25;
    param->i_fps_den       = 1;
    param->i_level_idc     = -1;
    param->b_level_force   = 0;
    param->i_profile       = 0;
    param->b_profile_force = 0;
    param->i_slice_max_size = 0;
    param->i_slice_max_mbs = 0;
    param->i_slice_count = 0;
#if HAVE_BITDEPTH8
    param->i_bitdepth = 8;
#elif HAVE_BITDEPTH10
    param->i_bitdepth = 10;
#else
    param->i_bitdepth = 8;
#endif

    /* Encoder parameters */
    param->i_frame_reference = 3;
    param->i_keyint_max = 250;
    param->i_keyint_min = X264_KEYINT_MIN_AUTO;
    param->i_bframe = 3;
    param->i_scenecut_threshold = 40;
    param->i_bframe_adaptive = X264_B_ADAPT_FAST;
    param->i_bframe_bias = 0;
    param->i_bframe_pyramid = X264_B_PYRAMID_NORMAL;
    param->b_interlaced = 0;
    param->b_constrained_intra = 0;

    param->b_deblocking_filter = 1;
    param->i_deblocking_filter_alphac0 = 0;
    param->i_deblocking_filter_beta = 0;

    param->b_cabac = 1;
    param->i_cabac_init_idc = 0;

    param->rc.i_rc_method = X264_RC_CRF;
    param->rc.i_bitrate = 0;
    param->rc.f_rate_tolerance = 1.0;
    param->rc.i_vbv_max_bitrate = 0;
    param->rc.i_vbv_buffer_size = 0;
    param->rc.f_vbv_buffer_init = 0.9;
    param->rc.i_qp_constant = -1;
    param->rc.f_rf_constant = 23;
    param->rc.i_qp_min_min           =
    param->rc.i_qp_min[SLICE_TYPE_I] =
    param->rc.i_qp_min[SLICE_TYPE_P] =
    param->rc.i_qp_min[SLICE_TYPE_B] = 0;
    param->rc.i_qp_max_max           =
    param->rc.i_qp_max[SLICE_TYPE_I] =
    param->rc.i_qp_max[SLICE_TYPE_P] =
    param->rc.i_qp_max[SLICE_TYPE_B] = INT_MAX;
    param->rc.i_qp_step = 4;
    param->rc.f_ip_factor = 1.4;
    param->rc.f_pb_factor = 1.3;
    param->rc.i_aq_mode = X264_AQ_VARIANCE;
    param->rc.f_aq_strength = 1.0;
    param->rc.f_aq_bias_strength = 1.0;
    param->rc.f_aq_sensitivity = 10;
    param->rc.f_aq_ifactor = 1.0;
    param->rc.f_aq_pfactor = 1.0;
    param->rc.f_aq_bfactor = 1.0;
    param->rc.b_aq2 = 0;
    param->rc.f_aq2_strength = 0.0;
    param->rc.f_aq2_sensitivity = 15.0;
    param->rc.f_aq2_ifactor = 1.0;
    param->rc.f_aq2_pfactor = 1.0;
    param->rc.f_aq2_bfactor = 1.0;
    param->rc.i_aq3_mode = X264_AQ_NONE;
    param->rc.f_aq3_strength = 0.5;
    param->rc.f_aq3_strengths[0][0] = 0;
    param->rc.f_aq3_strengths[0][1] = 0;
    param->rc.f_aq3_strengths[0][2] = 0;
    param->rc.f_aq3_strengths[0][3] = 0;
    param->rc.f_aq3_strengths[1][0] = 0;
    param->rc.f_aq3_strengths[1][1] = 0;
    param->rc.f_aq3_strengths[1][2] = 0;
    param->rc.f_aq3_strengths[1][3] = 0;
    param->rc.f_aq3_sensitivity = 10;
    param->rc.f_aq3_ifactor[0] = 1.0;
    param->rc.f_aq3_ifactor[1] = 1.0;
    param->rc.f_aq3_pfactor[0] = 1.0;
    param->rc.f_aq3_pfactor[1] = 1.0;
    param->rc.f_aq3_bfactor[0] = 1.0;
    param->rc.f_aq3_bfactor[1] = 1.0;
    param->rc.b_aq3_boundary = 0;
    param->rc.i_aq3_boundary[0] = 192;
    param->rc.i_aq3_boundary[1] = 64;
    param->rc.i_aq3_boundary[2] = 24;
    param->rc.i_lookahead = 40;

    param->rc.b_stat_write = 0;
    param->rc.psz_stat_out = "x264_2pass.log";
    param->rc.b_stat_read = 0;
    param->rc.psz_stat_in = "x264_2pass.log";
    param->rc.f_qcompress = 0.6;
    param->rc.f_qblur = 0.5;
    param->rc.f_complexity_blur = 20;
    param->rc.i_zones = 0;
    param->rc.b_mb_tree = 1;

    /* Log */
    param->pf_log = x264_log_default;
    param->p_log_private = NULL;
    param->i_log_level = X264_LOG_INFO;
	param->b_stylish = 0;
	param->i_log_file_level = X264_LOG_INFO;

    /* */
    param->analyse.intra = X264_ANALYSE_I4x4 | X264_ANALYSE_I8x8;
    param->analyse.inter = X264_ANALYSE_I4x4 | X264_ANALYSE_I8x8
                         | X264_ANALYSE_PSUB16x16 | X264_ANALYSE_BSUB16x16;
    param->analyse.i_direct_mv_pred = X264_DIRECT_PRED_SPATIAL;
    param->analyse.i_me_method = X264_ME_HEX;
    param->analyse.f_psy_rd = 1.0;
    param->analyse.b_psy = 1;
    param->analyse.f_psy_trellis = 0;
    param->analyse.i_fgo = 0;
    param->analyse.i_me_range = 16;
    param->analyse.i_subpel_refine = 7;
    param->analyse.b_mixed_references = 1;
    param->analyse.b_chroma_me = 1;
    param->analyse.i_mv_range_thread = -1;
    param->analyse.i_mv_range = -1; // set from level_idc
    param->analyse.i_chroma_qp_offset = 0;
    param->analyse.b_fast_pskip = 1;
    param->analyse.b_weighted_bipred = 1;
    param->analyse.i_weighted_pred = X264_WEIGHTP_SMART;
    param->analyse.b_dct_decimate = 1;
    param->analyse.b_transform_8x8 = 1;
    param->analyse.i_trellis = 1;
    param->analyse.i_luma_deadzone[0] = 21;
    param->analyse.i_luma_deadzone[1] = 11;
    param->analyse.b_psnr = 0;
    param->analyse.b_ssim = 0;

    param->i_cqm_preset = X264_CQM_FLAT;
    memset( param->cqm_4iy, 16, sizeof( param->cqm_4iy ) );
    memset( param->cqm_4py, 16, sizeof( param->cqm_4py ) );
    memset( param->cqm_4ic, 16, sizeof( param->cqm_4ic ) );
    memset( param->cqm_4pc, 16, sizeof( param->cqm_4pc ) );
    memset( param->cqm_8iy, 16, sizeof( param->cqm_8iy ) );
    memset( param->cqm_8py, 16, sizeof( param->cqm_8py ) );
    memset( param->cqm_8ic, 16, sizeof( param->cqm_8ic ) );
    memset( param->cqm_8pc, 16, sizeof( param->cqm_8pc ) );

    param->b_repeat_headers = 1;
    param->b_annexb = 1;
    param->b_aud = 0;
    param->b_vfr_input = 1;
    param->i_nal_hrd = X264_NAL_HRD_NONE;
    param->b_tff = 1;
    param->b_pic_struct = 0;
    param->b_fake_interlaced = 0;
    param->i_frame_packing = -1;
    param->i_alternative_transfer = 2; /* undef */
    param->b_opencl = 0;
    param->i_opencl_device = 0;
    param->opencl_device_id = NULL;
    param->psz_clbin_file = NULL;
    param->filters.b_sub = 0;

    param->i_opts_write = X264_OPTS_FULL;
    for( int i = 0; i < X264_OPTS_MAX; i++ )
        param->psz_opts[i] = NULL;

    param->i_avcintra_class = 0;
    param->i_avcintra_flavor = X264_AVCINTRA_FLAVOR_PANASONIC;
}

static int param_apply_preset( x264_param_t *param, const char *preset )
{
    char *end;
    long i;

    if( preset[0] >= '0' && preset[0] <= '9' )
    {
        errno = 0;
        i = strtol( preset, &end, 10 );
        if( *end == 0 && errno != ERANGE && i >= 0 && i < ARRAY_ELEMS(x264_preset_names)-1 )
            preset = x264_preset_names[i];
    }

    if( !strcasecmp( preset, "ultrafast" ) )
    {
        param->i_frame_reference = 1;
        param->i_scenecut_threshold = 0;
        param->b_deblocking_filter = 0;
        param->b_cabac = 0;
        param->i_bframe = 0;
        param->analyse.intra = 0;
        param->analyse.inter = 0;
        param->analyse.b_transform_8x8 = 0;
        param->analyse.i_me_method = X264_ME_DIA;
        param->analyse.i_subpel_refine = 0;
        param->rc.i_aq_mode = 0;
        param->analyse.b_mixed_references = 0;
        param->analyse.i_trellis = 0;
        param->i_bframe_adaptive = X264_B_ADAPT_NONE;
        param->rc.b_mb_tree = 0;
        param->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
        param->analyse.b_weighted_bipred = 0;
        param->rc.i_lookahead = 0;
    }
    else if( !strcasecmp( preset, "superfast" ) )
    {
        param->analyse.inter = X264_ANALYSE_I8x8|X264_ANALYSE_I4x4;
        param->analyse.i_me_method = X264_ME_DIA;
        param->analyse.i_subpel_refine = 1;
        param->i_frame_reference = 1;
        param->analyse.b_mixed_references = 0;
        param->analyse.i_trellis = 0;
        param->rc.b_mb_tree = 0;
        param->analyse.i_weighted_pred = X264_WEIGHTP_SIMPLE;
        param->rc.i_lookahead = 0;
    }
    else if( !strcasecmp( preset, "veryfast" ) )
    {
        param->analyse.i_subpel_refine = 2;
        param->i_frame_reference = 1;
        param->analyse.b_mixed_references = 0;
        param->analyse.i_trellis = 0;
        param->analyse.i_weighted_pred = X264_WEIGHTP_SIMPLE;
        param->rc.i_lookahead = 10;
    }
    else if( !strcasecmp( preset, "faster" ) )
    {
        param->analyse.b_mixed_references = 0;
        param->i_frame_reference = 2;
        param->analyse.i_subpel_refine = 4;
        param->analyse.i_weighted_pred = X264_WEIGHTP_SIMPLE;
        param->rc.i_lookahead = 20;
    }
    else if( !strcasecmp( preset, "fast" ) )
    {
        param->i_frame_reference = 2;
        param->analyse.i_subpel_refine = 6;
        param->analyse.i_weighted_pred = X264_WEIGHTP_SIMPLE;
        param->rc.i_lookahead = 30;
    }
    else if( !strcasecmp( preset, "medium" ) )
    {
        /* Default is medium */
    }
    else if( !strcasecmp( preset, "slow" ) )
    {
        param->analyse.i_subpel_refine = 8;
        param->i_frame_reference = 5;
        param->analyse.i_direct_mv_pred = X264_DIRECT_PRED_AUTO;
        param->analyse.i_trellis = 2;
        param->rc.i_lookahead = 50;
    }
    else if( !strcasecmp( preset, "slower" ) )
    {
        param->analyse.i_me_method = X264_ME_UMH;
        param->analyse.i_subpel_refine = 9;
        param->i_frame_reference = 8;
        param->i_bframe_adaptive = X264_B_ADAPT_TRELLIS;
        param->analyse.i_direct_mv_pred = X264_DIRECT_PRED_AUTO;
        param->analyse.inter |= X264_ANALYSE_PSUB8x8;
        param->analyse.i_trellis = 2;
        param->rc.i_lookahead = 60;
    }
    else if( !strcasecmp( preset, "veryslow" ) )
    {
        param->analyse.i_me_method = X264_ME_UMH;
        param->analyse.i_subpel_refine = 10;
        param->analyse.i_me_range = 24;
        param->i_frame_reference = 16;
        param->i_bframe_adaptive = X264_B_ADAPT_TRELLIS;
        param->analyse.i_direct_mv_pred = X264_DIRECT_PRED_AUTO;
        param->analyse.inter |= X264_ANALYSE_PSUB8x8;
        param->analyse.i_trellis = 2;
        param->i_bframe = 8;
        param->rc.i_lookahead = 60;
    }
    else if( !strcasecmp( preset, "placebo" ) )
    {
        param->analyse.i_me_method = X264_ME_TESA;
        param->analyse.i_subpel_refine = 11;
        param->analyse.i_me_range = 24;
        param->i_frame_reference = 16;
        param->i_bframe_adaptive = X264_B_ADAPT_TRELLIS;
        param->analyse.i_direct_mv_pred = X264_DIRECT_PRED_AUTO;
        param->analyse.inter |= X264_ANALYSE_PSUB8x8;
        param->analyse.b_fast_pskip = 0;
        param->analyse.i_trellis = 2;
        param->i_bframe = 16;
        param->rc.i_lookahead = 60;
    }
    else
    {
        x264_log_internal( X264_LOG_ERROR, "invalid preset '%s'\n", preset );
        return -1;
    }
    return 0;
}

static int param_apply_tune( x264_param_t *param, const char *tune )
{
    int psy_tuning_used = 0;
    for( int len; tune += strspn( tune, ",./-+" ), (len = strcspn( tune, ",./-+" )); tune += len )
    {
        if( len == 4 && !strncasecmp( tune, "film", 4 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->i_deblocking_filter_alphac0 = -1;
            param->i_deblocking_filter_beta = -1;
            param->analyse.f_psy_trellis = 0.15;
        }
        else if( len == 9 && !strncasecmp( tune, "animation", 9 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->i_frame_reference = param->i_frame_reference > 1 ? param->i_frame_reference*2 : 1;
            param->i_deblocking_filter_alphac0 = 1;
            param->i_deblocking_filter_beta = 1;
            param->analyse.f_psy_rd = 0.4;
            param->rc.f_aq_strength = 0.6;
            param->i_bframe += 2;
        }
        else if( len == 5 && !strncasecmp( tune, "grain", 5 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->i_deblocking_filter_alphac0 = -2;
            param->i_deblocking_filter_beta = -2;
            param->analyse.f_psy_trellis = 0.25;
            param->analyse.b_dct_decimate = 0;
            param->rc.f_pb_factor = 1.1;
            param->rc.f_ip_factor = 1.1;
            param->rc.f_aq_strength = 0.5;
            param->analyse.i_luma_deadzone[0] = 6;
            param->analyse.i_luma_deadzone[1] = 6;
            param->rc.f_qcompress = 0.8;
        }
        else if( len == 10 && !strncasecmp( tune, "stillimage", 10 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->i_deblocking_filter_alphac0 = -3;
            param->i_deblocking_filter_beta = -3;
            param->analyse.f_psy_rd = 2.0;
            param->analyse.f_psy_trellis = 0.7;
            param->rc.f_aq_strength = 1.2;
        }
        else if( len == 4 && !strncasecmp( tune, "psnr", 4 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->rc.i_aq_mode = X264_AQ_NONE;
            param->analyse.b_psy = 0;
        }
        else if( len == 4 && !strncasecmp( tune, "ssim", 4 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->rc.i_aq_mode = X264_AQ_AUTOVARIANCE;
            param->analyse.b_psy = 0;
        }
        else if( len == 10 && !strncasecmp( tune, "fastdecode", 10 ) )
        {
            param->b_deblocking_filter = 0;
            param->b_cabac = 0;
            param->analyse.b_weighted_bipred = 0;
            param->analyse.i_weighted_pred = X264_WEIGHTP_NONE;
        }
        else if( len == 11 && !strncasecmp( tune, "zerolatency", 11 ) )
        {
            param->rc.i_lookahead = 0;
            param->i_sync_lookahead = 0;
            param->i_bframe = 0;
            param->b_sliced_threads = 1;
            param->b_vfr_input = 0;
            param->rc.b_mb_tree = 0;
        }
        else if( len == 6 && !strncasecmp( tune, "touhou", 6 ) )
        {
            if( psy_tuning_used++ ) goto psy_failure;
            param->i_frame_reference = param->i_frame_reference > 1 ? param->i_frame_reference*2 : 1;
            param->i_deblocking_filter_alphac0 = -1;
            param->i_deblocking_filter_beta = -1;
            param->analyse.f_psy_trellis = 0.2;
            param->rc.f_aq_strength = 1.3;
            if( param->analyse.inter & X264_ANALYSE_PSUB16x16 )
                param->analyse.inter |= X264_ANALYSE_PSUB8x8;
        }
        else
        {
            x264_log_internal( X264_LOG_ERROR, "invalid tune '%.*s'\n", len, tune );
            return -1;
    psy_failure:
            x264_log_internal( X264_LOG_WARNING, "only 1 psy tuning can be used: ignoring tune %.*s\n", len, tune );
        }
    }
    return 0;
}

REALIGN_STACK int x264_param_default_preset( x264_param_t *param, const char *preset, const char *tune )
{
    if( !param )
        return -1;
    x264_param_default( param );

    if( preset && param_apply_preset( param, preset ) < 0 )
        return -1;
    if( tune )
    {
        x264_param_t adjusted = *param;
        if( param_apply_tune( &adjusted, tune ) < 0 )
            return -1;
        *param = adjusted;
    }
    return 0;
}

REALIGN_STACK void x264_param_apply_fastfirstpass( x264_param_t *param )
{
    if( !param )
        return;

    /* Set faster options in case of turbo firstpass. */
    if( param->rc.b_stat_write && !param->rc.b_stat_read )
    {
        param->i_frame_reference = 1;
        param->analyse.b_transform_8x8 = 0;
        param->analyse.inter = 0;
        param->analyse.i_me_method = X264_ME_DIA;
        param->analyse.i_subpel_refine = X264_MIN( 2, param->analyse.i_subpel_refine );
        param->analyse.i_trellis = 0;
        param->analyse.b_fast_pskip = 1;
        param->analyse.i_fgo = 0;
    }
}

static int profile_string_to_int( const char *str )
{
    if( !strcasecmp( str, "baseline" ) )
        return PROFILE_BASELINE;
    if( !strcasecmp( str, "main" ) )
        return PROFILE_MAIN;
    if( !strcasecmp( str, "high" ) )
        return PROFILE_HIGH;
    if( !strcasecmp( str, "high10" ) )
        return PROFILE_HIGH10;
    if( !strcasecmp( str, "high422" ) )
        return PROFILE_HIGH422;
    if( !strcasecmp( str, "high444" ) )
        return PROFILE_HIGH444_PREDICTIVE;
    return -1;
}

REALIGN_STACK int x264_generic_device_check( x264_param_t *param, const char *device, int device_mask )
{
    if( !param || !device )
        return -1;
    const int qp_bd_offset = 6 * (param->i_bitdepth-8);

    if( (device_mask & X264_DEVICE_LEVEL_FREE) == 0 )
        param->b_level_force = 1;

    if( (device_mask & X264_DEVICE_LOSSLESS) == 0 && (
          (param->rc.i_rc_method == X264_RC_CQP && param->rc.i_qp_constant <= 0) ||
          (param->rc.i_rc_method == X264_RC_CRF && (int)(param->rc.f_rf_constant + qp_bd_offset) <= 0)
        )
      )
    {
        x264_log_internal( X264_LOG_ERROR, "%s doesn't support lossless\n", device );
        return -1;
    }

    if( (device_mask & X264_DEVICE_444) == 0 && (param->i_csp & X264_CSP_MASK) >= X264_CSP_I444 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s doesn't support 4:4:4\n", device );
        return -1;
    }

    if( (device_mask & X264_DEVICE_422) == 0 && (param->i_csp & X264_CSP_MASK) >= X264_CSP_I422 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s doesn't support 4:2:2\n", device );
        return -1;
    }

    if( (device_mask & X264_DEVICE_HIGH_DEPTH) == 0 && param->i_bitdepth > 8 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s doesn't support a bit depth of %d\n", device, param->i_bitdepth );
        return -1;
    }

    return 0;
}


REALIGN_STACK int x264_param_restrict_device( x264_param_t *param, int i_profile, const char *device )
{
    if( !param || !device )
        return -1;
    x264_param_t adjusted = *param;
    size_t device_len = strlen( device );
    if( device_len > INT64_MAX - 1 )
        return -1;
    int64_t device_alloc_size = (int64_t)device_len + 1;
    char *tmp = x264_malloc( device_alloc_size );
    if( !tmp )
        return -1;
    memcpy( tmp, device, device_len + 1 );
    char *s = tmp;
    if( !*s )
    {
        x264_log_internal( X264_LOG_ERROR, "empty device\n" );
        x264_free( tmp );
        return -1;
    }
    while( *s )
    {
        size_t len = strcspn( s, ",./-+" );
        char *next = s + len;
        if( !len )
        {
            x264_log_internal( X264_LOG_ERROR, "empty device\n" );
            x264_free( tmp );
            return -1;
        }
        if( *next )
        {
            *next++ = '\0';
            if( !*next || strchr( ",./-+", *next ) )
            {
                x264_log_internal( X264_LOG_ERROR, "empty device\n" );
                x264_free( tmp );
                return -1;
            }
        }
        if( !strcasecmp( s, "dxva" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_DXVA ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_level_idc = ( adjusted.i_level_idc < 0 ) ?
                                   X264_LEVEL_IDC_DXVA : X264_MIN( adjusted.i_level_idc, X264_LEVEL_IDC_DXVA );

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_HIGH ) : PROFILE_HIGH;
        }
        else if( !strcasecmp( s, "bluray" ) || !strcasecmp( s, "blu-ray" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_BLURAY ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_level_idc = ( adjusted.i_level_idc < 0 ) ?
                                   X264_LEVEL_IDC_BLURAY : X264_MIN( adjusted.i_level_idc, X264_LEVEL_IDC_BLURAY );
            adjusted.b_bluray_compat = 1;

            adjusted.rc.i_vbv_max_bitrate = ( adjusted.rc.i_vbv_max_bitrate <= 0 ) ?
                                            X264_VBV_MAXRATE_BLURAY : X264_MIN( adjusted.rc.i_vbv_max_bitrate, X264_VBV_MAXRATE_BLURAY );

            adjusted.rc.i_vbv_buffer_size = ( adjusted.rc.i_vbv_buffer_size <= 0 ) ?
                                            X264_VBV_BUFSIZE_BLURAY : X264_MIN( adjusted.rc.i_vbv_buffer_size, X264_VBV_BUFSIZE_BLURAY );

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_HIGH ) : PROFILE_HIGH;
        }
        else if( !strcasecmp( s, "psp" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_PSP ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_frame_reference = X264_MIN( adjusted.i_frame_reference, 3 );
            adjusted.i_bframe_pyramid = X264_B_PYRAMID_NONE;
            adjusted.analyse.i_weighted_pred = X264_MIN( adjusted.analyse.i_weighted_pred, X264_WEIGHTP_SIMPLE );

            adjusted.rc.i_vbv_max_bitrate = ( adjusted.rc.i_vbv_max_bitrate <= 0 ) ?
                                            X264_VBV_MAXRATE_PSP : X264_MIN( adjusted.rc.i_vbv_max_bitrate, X264_VBV_MAXRATE_PSP );

            adjusted.rc.i_vbv_buffer_size = ( adjusted.rc.i_vbv_buffer_size <= 0 ) ?
                                            X264_VBV_BUFSIZE_PSP : X264_MIN( adjusted.rc.i_vbv_buffer_size, X264_VBV_BUFSIZE_PSP );

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_MAIN ) : PROFILE_MAIN;
        }
        else if( !strcasecmp( s, "psv" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_PSV ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_HIGH ) : PROFILE_HIGH;
        }
        else if( !strcasecmp( s, "ps3" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_PS3 ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_level_idc = ( adjusted.i_level_idc < 0 ) ?
                                   X264_LEVEL_IDC_PS3 : X264_MIN( adjusted.i_level_idc, X264_LEVEL_IDC_PS3 );

            adjusted.rc.i_vbv_max_bitrate = ( adjusted.rc.i_vbv_max_bitrate <= 0 ) ?
                                            X264_VBV_MAXRATE_PS3 : X264_MIN( adjusted.rc.i_vbv_max_bitrate, X264_VBV_MAXRATE_PS3 );

            adjusted.rc.i_vbv_buffer_size = ( adjusted.rc.i_vbv_buffer_size <= 0 ) ?
                                            X264_VBV_BUFSIZE_PS3 : X264_MIN( adjusted.rc.i_vbv_buffer_size, X264_VBV_BUFSIZE_PS3 );

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_HIGH ) : PROFILE_HIGH;
        }
        else if( !strcasecmp( s, "xbox" ) || !strcasecmp( s, "xbox360" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_XBOX ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_level_idc = ( adjusted.i_level_idc < 0 ) ?
                                   X264_LEVEL_IDC_XBOX : X264_MIN( adjusted.i_level_idc, X264_LEVEL_IDC_XBOX );

            adjusted.rc.i_vbv_max_bitrate = ( adjusted.rc.i_vbv_max_bitrate <= 0 ) ?
                                            X264_VBV_MAXRATE_XBOX : X264_MIN( adjusted.rc.i_vbv_max_bitrate, X264_VBV_MAXRATE_XBOX );

            adjusted.rc.i_vbv_buffer_size = ( adjusted.rc.i_vbv_buffer_size <= 0 ) ?
                                            X264_VBV_BUFSIZE_XBOX : X264_MIN( adjusted.rc.i_vbv_buffer_size, X264_VBV_BUFSIZE_XBOX );

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_HIGH ) : PROFILE_HIGH;
        }
        else if( !strcasecmp( s, "iphone" ) || !strcasecmp( s, "ipad" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_IPHONE ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_level_idc = ( adjusted.i_level_idc < 0 ) ?
                                   X264_LEVEL_IDC_IPHONE : X264_MIN( adjusted.i_level_idc, X264_LEVEL_IDC_IPHONE );
            /* Fix me: I have no data about iPhone/iPad's vbv restriction */

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_HIGH ) : PROFILE_HIGH;
        }
        else if( !strcasecmp( s, "generic" ) )
        {
            if( x264_generic_device_check( &adjusted, s, X264_DEVICE_GENERIC ) < 0 )
            {
                x264_free( tmp );
                return -1;
            }
            adjusted.i_frame_reference = X264_MIN( adjusted.i_frame_reference, 3 );
            adjusted.i_bframe = X264_MIN( adjusted.i_bframe, 3 );
            adjusted.i_bframe_pyramid = X264_B_PYRAMID_NONE;
            adjusted.analyse.i_weighted_pred = X264_MIN( adjusted.analyse.i_weighted_pred, X264_WEIGHTP_SIMPLE );
            adjusted.i_level_idc = ( adjusted.i_level_idc < 0 ) ?
                                   X264_LEVEL_IDC_GENERIC : X264_MIN( adjusted.i_level_idc, X264_LEVEL_IDC_GENERIC );

            adjusted.rc.i_vbv_max_bitrate = ( adjusted.rc.i_vbv_max_bitrate <= 0 ) ?
                                            X264_VBV_MAXRATE_GENERIC : X264_MIN( adjusted.rc.i_vbv_max_bitrate, X264_VBV_MAXRATE_GENERIC );

            adjusted.rc.i_vbv_buffer_size = ( adjusted.rc.i_vbv_buffer_size <= 0 ) ?
                                            X264_VBV_BUFSIZE_GENERIC : X264_MIN( adjusted.rc.i_vbv_buffer_size, X264_VBV_BUFSIZE_GENERIC );

            i_profile = i_profile ? X264_MIN( i_profile, PROFILE_MAIN ) : PROFILE_MAIN;
        }
        else
        {
            x264_log_internal( X264_LOG_ERROR, "invalid device: %s\n", s );
            x264_free( tmp );
            return -1;
        }
        s = next;
    }
    *param = adjusted;
    x264_free( tmp );
    return i_profile;
}


REALIGN_STACK int x264_param_apply_profile( x264_param_t *param, const char *profile, const char *device )
{
    if( !param )
        return -1;
    x264_param_t adjusted = *param;
    int p = adjusted.i_profile;
	const int qp_bd_offset = 6 * (adjusted.i_bitdepth-8);

    if( profile )
        p = profile_string_to_int( profile );	
    if( p < 0 )
    {
        x264_log_internal( X264_LOG_ERROR, "invalid profile: %s\n", profile );
        return -1;
    }
	
    if( device )
        p = x264_param_restrict_device( &adjusted, p, device );
    if( !p )                     // auto profile
    {
        *param = adjusted;
        return 0;
    }
    if( p < 0 )
        return -1;

    if( p < PROFILE_HIGH444_PREDICTIVE && ((adjusted.rc.i_rc_method == X264_RC_CQP && adjusted.rc.i_qp_constant <= 0) ||
        (adjusted.rc.i_rc_method == X264_RC_CRF && (int)(adjusted.rc.f_rf_constant + qp_bd_offset) <= 0)) )
    {
        x264_log_internal( X264_LOG_ERROR, "%s profile doesn't support lossless\n", profile );
        return -1;
    }
    if( p < PROFILE_HIGH444_PREDICTIVE && (adjusted.i_csp & X264_CSP_MASK) >= X264_CSP_I444 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s profile doesn't support 4:4:4\n", profile );
        return -1;
    }
    if( p < PROFILE_HIGH422 && (adjusted.i_csp & X264_CSP_MASK) >= X264_CSP_I422 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s profile doesn't support 4:2:2\n", profile );
        return -1;
    }
    if( p < PROFILE_HIGH10 && adjusted.i_bitdepth > 8 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s profile doesn't support a bit depth of %d\n", profile, adjusted.i_bitdepth );
        return -1;
    }
    if( p < PROFILE_HIGH && (adjusted.i_csp & X264_CSP_MASK) == X264_CSP_I400 )
    {
        x264_log_internal( X264_LOG_ERROR, "%s profile doesn't support 4:0:0\n", profile );
        return -1;
    }

    if( p == PROFILE_BASELINE )
    {
        adjusted.analyse.b_transform_8x8 = 0;
        adjusted.b_cabac = 0;
        adjusted.i_cqm_preset = X264_CQM_FLAT;
        adjusted.psz_cqm_file = NULL;
        adjusted.i_bframe = 0;
        adjusted.analyse.i_weighted_pred = X264_WEIGHTP_NONE;
        if( adjusted.b_interlaced )
        {
            x264_log_internal( X264_LOG_ERROR, "baseline profile doesn't support interlacing\n" );
            return -1;
        }
        if( adjusted.b_fake_interlaced )
        {
            x264_log_internal( X264_LOG_ERROR, "baseline profile doesn't support fake interlacing\n" );
            return -1;
        }
    }
    else if( p == PROFILE_MAIN )
    {
        adjusted.analyse.b_transform_8x8 = 0;
        adjusted.i_cqm_preset = X264_CQM_FLAT;
        adjusted.psz_cqm_file = NULL;
    }
	adjusted.i_profile = p;
    *param = adjusted;
    return 0;
}

static int parse_enum( const char *arg, const char * const *names, int *dst )
{
    for( int i = 0; names[i]; i++ )
        if( *names[i] && !strcasecmp( arg, names[i] ) )
        {
            *dst = i;
            return 0;
        }
    return -1;
}

static int parse_cpu_name_list( const char *arg, uint32_t *dst )
{
    uint32_t cpu = 0;

    for( ;; )
    {
        size_t len = strcspn( arg, "," );
        int i = 0;

        if( !len )
            return -1;
        while( x264_cpu_names[i].flags &&
               ( strlen( x264_cpu_names[i].name ) != len ||
                 strncasecmp( arg, x264_cpu_names[i].name, len ) ) )
            i++;
        if( !x264_cpu_names[i].flags )
            return -1;

        cpu |= x264_cpu_names[i].flags;
        arg += len;
        if( !*arg )
            break;
        arg++;
        if( !*arg )
            return -1;
    }

    if( (cpu&X264_CPU_SSSE3) && !(cpu&X264_CPU_SSE2_IS_SLOW) )
        cpu |= X264_CPU_SSE2_IS_FAST;
    *dst = cpu;
    return 0;
}

static int parse_partitions( const char *arg, unsigned int *dst )
{
    unsigned int inter = 0;
    int all = 0;

    if( !*arg )
        return -1;

    while( *arg )
    {
        const char *end = strchr( arg, ',' );
        size_t len = end ? (size_t)(end - arg) : strlen( arg );

        if( len == 0 )
            return -1;
        if( len == 4 && !strncmp( arg, "none", 4 ) )
        {
            /* "none" contributes no flags; exact companion tokens still apply. */
        }
        else if( len == 3 && !strncmp( arg, "all", 3 ) )
            all = 1;
        else if( len == 4 && !strncmp( arg, "i4x4", 4 ) )
            inter |= X264_ANALYSE_I4x4;
        else if( len == 4 && !strncmp( arg, "i8x8", 4 ) )
            inter |= X264_ANALYSE_I8x8;
        else if( len == 4 && !strncmp( arg, "p8x8", 4 ) )
            inter |= X264_ANALYSE_PSUB16x16;
        else if( len == 4 && !strncmp( arg, "p4x4", 4 ) )
            inter |= X264_ANALYSE_PSUB8x8;
        else if( len == 4 && !strncmp( arg, "b8x8", 4 ) )
            inter |= X264_ANALYSE_BSUB16x16;
        else
            return -1;

        if( !end )
            break;
        arg = end + 1;
    }

    *dst = all ? ~0u : inter;
    return 0;
}

static int parse_int_end_base( const char *str, char **end, int base, int *dst )
{
    long value;

    errno = 0;
    value = strtol( str, end, base );
    if( *end == str || errno == ERANGE || value < INT_MIN || value > INT_MAX )
        return -1;

    int parsed_value = (int)value;
    *dst = parsed_value;
    return 0;
}

static int parse_int_token( const char *str, int base, int *dst )
{
    char *end;
    const char *p = str;
    int value;

    if( *p == '-' )
        p++;
    if( !X264_ISDIGIT( *p ) )
        return -1;

    if( parse_int_end_base( str, &end, base, &value ) )
        return -1;
    while( isspace( (unsigned char)*end ) )
        end++;
    if( *end )
        return -1;
    *dst = value;
    return 0;
}

static int int_token_starts_valid( const char *str )
{
    if( *str == '-' )
        str++;
    return X264_ISDIGIT( *str );
}

static int is_special_float_token( const char *str )
{
    while( isspace( (unsigned char)*str ) )
        str++;
    if( *str == '-' || *str == '+' )
        str++;
    return !strncasecmp( str, "nan", 3 ) || !strncasecmp( str, "inf", 3 );
}

static int parse_param_int_list( const char *str, int *dst, int count, const char *separators )
{
    char *end;
    char separator = 0;

    if( count <= 0 )
        return 0;
    int parsed[count];

    for( int i = 0; i < count; i++ )
    {
        if( i )
            while( isspace( (unsigned char)*str ) )
                str++;
        if( !int_token_starts_valid( str ) )
            return -1;
        if( parse_int_end_base( str, &end, 10, &parsed[i] ) )
            return -1;
        if( i == count - 1 )
        {
            while( isspace( (unsigned char)*end ) )
                end++;
            if( *end )
                return -1;
            memcpy( dst, parsed, count * sizeof(*dst) );
            return 0;
        }
        if( !separator )
        {
            if( !*end || !strchr( separators, *end ) )
                return -1;
            separator = *end;
        }
        else if( *end != separator )
            return -1;
        str = end + 1;
    }

    return 0;
}

static int parse_float_end( const char *str, char **end, float *dst )
{
    double value;

    errno = 0;
    if( is_special_float_token( str ) )
        return -1;
    value = strtod( str, end );
    if( *end == str || errno == ERANGE || value < -FLT_MAX || value > FLT_MAX )
        return -1;

    *dst = (float)value;
    return 0;
}

static int parse_float_token( const char *str, float *dst )
{
    char *end;
    const char *p = str;
    float value;

    if( *p == '-' )
        p++;
    if( !X264_ISDIGIT( *p ) && *p != '.' )
        return -1;

    if( parse_float_end( str, &end, &value ) )
        return -1;
    while( isspace( (unsigned char)*end ) )
        end++;
    if( *end )
        return -1;
    *dst = value;
    return 0;
}

static int float_token_starts_valid( const char *str )
{
    if( *str == '-' )
        str++;
    return X264_ISDIGIT( *str ) || *str == '.';
}

static int parse_double_token( const char *str, double *dst )
{
    char *end;
    const char *p = str;
    double value;

    if( *p == '-' )
        p++;
    if( !X264_ISDIGIT( *p ) && *p != '.' )
        return -1;

    errno = 0;
    if( is_special_float_token( str ) )
        return -1;
    value = strtod( str, &end );
    if( end == str || errno == ERANGE )
        return -1;
    while( isspace( (unsigned char)*end ) )
        end++;
    if( *end )
        return -1;
    *dst = value;
    return 0;
}

static int parse_param_float_list( const char *str, float *dst, int count, const char *separators )
{
    char *end;
    char separator = 0;

    if( count <= 0 )
        return 0;
    float parsed[count];

    for( int i = 0; i < count; i++ )
    {
        if( i )
            while( isspace( (unsigned char)*str ) )
                str++;
        if( !float_token_starts_valid( str ) )
            return -1;
        if( parse_float_end( str, &end, &parsed[i] ) )
            return -1;
        if( i == count - 1 )
        {
            while( isspace( (unsigned char)*end ) )
                end++;
            if( *end )
                return -1;
            memcpy( dst, parsed, count * sizeof(*dst) );
            return 0;
        }
        if( !separator )
        {
            if( !*end || !strchr( separators, *end ) )
                return -1;
            separator = *end;
        }
        else if( *end != separator )
            return -1;
        str = end + 1;
    }

    return 0;
}

static int parse_uint32_end_base( const char *str, char **end, int base, uint32_t *dst )
{
    uintmax_t value;

    errno = 0;
    value = strtoumax( str, end, base );
    if( *end == str || errno == ERANGE || value > UINT32_MAX )
        return -1;

    *dst = (uint32_t)value;
    return 0;
}

static int parse_uint32_token( const char *str, int base, uint32_t *dst )
{
    char *end;
    uint32_t value;

    if( !X264_ISDIGIT( *str ) )
        return -1;

    if( parse_uint32_end_base( str, &end, base, &value ) )
        return -1;
    while( isspace( (unsigned char)*end ) )
        end++;
    if( *end )
        return -1;
    *dst = value;
    return 0;
}

static int parse_int64_end_base( const char *str, char **end, int base, int64_t *dst )
{
    long long value;

    errno = 0;
    value = strtoll( str, end, base );
    if( *end == str || errno == ERANGE || value < INT64_MIN || value > INT64_MAX )
        return -1;

    *dst = (int64_t)value;
    return 0;
}

static int parse_uint32_pair_token( const char *str, char separator, uint32_t *num, uint32_t *den )
{
    char *end;
    uint32_t parsed_num;
    uint32_t parsed_den;

    if( !X264_ISDIGIT( *str ) )
        return -1;
    if( parse_uint32_end_base( str, &end, 10, &parsed_num ) || *end++ != separator ||
        !X264_ISDIGIT( *end ) ||
        parse_uint32_end_base( end, &end, 10, &parsed_den ) )
        return -1;
    while( isspace( (unsigned char)*end ) )
        end++;
    if( *end )
        return -1;
    *num = parsed_num;
    *den = parsed_den;
    return 0;
}

static int parse_mastering_display( const char *value, x264_param_t *p )
{
    const char *str = value;
    char *end;
    int green_x, green_y;
    int blue_x, blue_y;
    int red_x, red_y;
    int white_x, white_y;
    int64_t display_max, display_min;

    if( str[0] != 'G' || str[1] != '(' )
        return -1;
    str += 2;
    if( !int_token_starts_valid( str ) ||
        parse_int_end_base( str, &end, 10, &green_x ) || *end++ != ',' ||
        !int_token_starts_valid( end ) ||
        parse_int_end_base( end, &end, 10, &green_y ) || *end++ != ')' )
        return -1;
    str = end;

    if( str[0] != 'B' || str[1] != '(' )
        return -1;
    str += 2;
    if( !int_token_starts_valid( str ) ||
        parse_int_end_base( str, &end, 10, &blue_x ) || *end++ != ',' ||
        !int_token_starts_valid( end ) ||
        parse_int_end_base( end, &end, 10, &blue_y ) || *end++ != ')' )
        return -1;
    str = end;

    if( str[0] != 'R' || str[1] != '(' )
        return -1;
    str += 2;
    if( !int_token_starts_valid( str ) ||
        parse_int_end_base( str, &end, 10, &red_x ) || *end++ != ',' ||
        !int_token_starts_valid( end ) ||
        parse_int_end_base( end, &end, 10, &red_y ) || *end++ != ')' )
        return -1;
    str = end;

    if( str[0] != 'W' || str[1] != 'P' || str[2] != '(' )
        return -1;
    str += 3;
    if( !int_token_starts_valid( str ) ||
        parse_int_end_base( str, &end, 10, &white_x ) || *end++ != ',' ||
        !int_token_starts_valid( end ) ||
        parse_int_end_base( end, &end, 10, &white_y ) || *end++ != ')' )
        return -1;
    str = end;

    if( str[0] != 'L' || str[1] != '(' )
        return -1;
    str += 2;
    if( !int_token_starts_valid( str ) ||
        parse_int64_end_base( str, &end, 10, &display_max ) || *end++ != ',' ||
        !int_token_starts_valid( end ) ||
        parse_int64_end_base( end, &end, 10, &display_min ) || *end++ != ')' )
        return -1;
    while( isspace( (unsigned char)*end ) )
        end++;
    if( *end )
        return -1;

    p->mastering_display.i_green_x = green_x;
    p->mastering_display.i_green_y = green_y;
    p->mastering_display.i_blue_x = blue_x;
    p->mastering_display.i_blue_y = blue_y;
    p->mastering_display.i_red_x = red_x;
    p->mastering_display.i_red_y = red_y;
    p->mastering_display.i_white_x = white_x;
    p->mastering_display.i_white_y = white_y;
    p->mastering_display.i_display_max = display_max;
    p->mastering_display.i_display_min = display_min;
    return 0;
}

static int parse_cqm( const char *str, uint8_t *cqm, int length )
{
    for( int i = 0; i < length; i++ )
    {
        char *end;
        int coef;

        if( i )
            while( isspace( (unsigned char)*str ) )
                str++;
        if( !int_token_starts_valid( str ) )
            return -1;
        if( parse_int_end_base( str, &end, 10, &coef ) || coef < 1 || coef > 255 )
            return -1;
        cqm[i] = coef;
        while( isspace( (unsigned char)*end ) )
            end++;
        if( i == length - 1 )
            return *end ? -1 : 0;
        if( *end != ',' )
            return -1;
        str = end + 1;
    }

    return 0;
}

static int atobool_internal( const char *str, int *b_error )
{
    if( !strcmp(str, "1") ||
        !strcasecmp(str, "true") ||
        !strcasecmp(str, "yes") )
        return 1;
    if( !strcmp(str, "0") ||
        !strcasecmp(str, "false") ||
        !strcasecmp(str, "no") )
        return 0;
    *b_error = 1;
    return 0;
}

#define atobool(str) ( name_was_bool = 1, atobool_internal( str, &b_error ) )
#define OPT_BOOL(var) do {\
    int bool_value = atobool(value);\
    if( !b_error )\
        var = bool_value;\
} while( 0 )
#define OPT_BOOL_INV(var) do {\
    int bool_value = atobool(value);\
    if( !b_error )\
        var = !bool_value;\
} while( 0 )
#define CHECKED_ERROR_PARAM_STRDUP( var, param, src )\
do {\
    if( value_was_null )\
        b_error = 1;\
    else\
    {\
        char *param_strdup_value = x264_param_strdup( param, src );\
        if( !param_strdup_value )\
        {\
            b_error = 1;\
            errortype = X264_PARAM_ALLOC_FAILED;\
        }\
        else\
            var = param_strdup_value;\
    }\
} while( 0 )

REALIGN_STACK int x264_param_parse( x264_param_t *p, const char *name, const char *value )
{
    char *name_buf = NULL;
    int b_error = 0;
    int errortype = X264_PARAM_BAD_VALUE;
    int name_was_bool;
    int value_was_null = !value;

    if( !p )
        return X264_PARAM_BAD_VALUE;
    if( !name )
        return X264_PARAM_BAD_NAME;
    if( !value )
        value = "true";

    if( value[0] == '=' )
        value++;

    if( strchr( name, '_' ) ) // s/_/-/g
    {
        char *c;
        name_buf = strdup(name);
        if( !name_buf )
            return X264_PARAM_ALLOC_FAILED;
        while( (c = strchr( name_buf, '_' )) )
            *c = '-';
        name = name_buf;
    }

    if( !strncmp( name, "no", 2 ) )
    {
        name += 2;
        if( name[0] == '-' )
            name++;
        value = atobool(value) ? "false" : "true";
    }
    name_was_bool = 0;

#define OPT(STR) else if( !strcmp( name, STR ) )
#define OPT2(STR0, STR1) else if( !strcmp( name, STR0 ) || !strcmp( name, STR1 ) )
    if( 0 );
    OPT("asm")
    {
        if( X264_ISDIGIT(value[0]) )
            b_error |= parse_uint32_token( value, 0, &p->cpu );
        else
        {
            int cpu_bool = !strcasecmp(value, "auto") || atobool(value);
            if( b_error )
            {
                uint32_t cpu;
                b_error = parse_cpu_name_list( value, &cpu );
                if( !b_error )
                    p->cpu = cpu;
            }
            else
                p->cpu = cpu_bool ? x264_cpu_detect() : 0;
        }
    }
    OPT("threads")
    {
        if( !strcasecmp(value, "auto") )
            p->i_threads = X264_THREADS_AUTO;
        else
            b_error |= parse_int_token( value, 0, &p->i_threads );
    }
    OPT("lookahead-threads")
    {
        if( !strcasecmp(value, "auto") )
            p->i_lookahead_threads = X264_THREADS_AUTO;
        else
            b_error |= parse_int_token( value, 0, &p->i_lookahead_threads );
    }
    OPT("sliced-threads")
        OPT_BOOL( p->b_sliced_threads );
    OPT("sync-lookahead")
    {
        if( !strcasecmp(value, "auto") )
            p->i_sync_lookahead = X264_SYNC_LOOKAHEAD_AUTO;
        else
            b_error |= parse_int_token( value, 0, &p->i_sync_lookahead );
    }
    OPT2("deterministic", "n-deterministic")
        OPT_BOOL( p->b_deterministic );
    OPT("cpu-independent")
        OPT_BOOL( p->b_cpu_independent );
    OPT2("level", "level-idc")
    {
        if( !strcmp(value, "1b") )
            p->i_level_idc = 9;
        else
        {
            double level;
            if( !parse_double_token( value, &level ) )
            {
                if( level < 7 )
                    p->i_level_idc = (int)(10*level+.5);
                else
                    b_error |= parse_int_token( value, 0, &p->i_level_idc );
            }
            else
                b_error = 1;
        }
    }
    OPT("level-force")
        OPT_BOOL( p->b_level_force );
    OPT("profile-force")
        OPT_BOOL( p->b_profile_force );
    OPT("bluray-compat")
        OPT_BOOL( p->b_bluray_compat );
    OPT("avcintra-class")
        b_error |= parse_int_token( value, 0, &p->i_avcintra_class );
    OPT("avcintra-flavor")
        b_error |= parse_enum( value, x264_avcintra_flavor_names, &p->i_avcintra_flavor );
    OPT("sar")
    {
        int sar[2];
        b_error |= parse_param_int_list( value, sar, 2, ":/" );
        if( !b_error )
        {
            p->vui.i_sar_width = sar[0];
            p->vui.i_sar_height = sar[1];
        }
    }
    OPT("overscan")
        b_error |= parse_enum( value, x264_overscan_names, &p->vui.i_overscan );
    OPT("videoformat")
        b_error |= parse_enum( value, x264_vidformat_names, &p->vui.i_vidformat );
    OPT("fullrange")
        b_error |= parse_enum( value, x264_fullrange_names, &p->vui.b_fullrange );
    OPT("colorprim")
        b_error |= parse_enum( value, x264_colorprim_names, &p->vui.i_colorprim );
    OPT("transfer")
        b_error |= parse_enum( value, x264_transfer_names, &p->vui.i_transfer );
    OPT("colormatrix")
        b_error |= parse_enum( value, x264_colmatrix_names, &p->vui.i_colmatrix );
    OPT("chromaloc")
    {
        int chroma_loc;
        b_error |= parse_int_token( value, 0, &chroma_loc ) || chroma_loc < 0 || chroma_loc > 5;
        if( !b_error )
            p->vui.i_chroma_loc = chroma_loc;
    }
    OPT("mastering-display")
    {
        if( strcasecmp( value, "undef" ) )
        {
            b_error |= parse_mastering_display( value, p );
            if( !b_error )
                p->mastering_display.b_mastering_display = 1;
        }
        else
            p->mastering_display.b_mastering_display = 0;
    }
    OPT("cll")
    {
        if( strcasecmp( value, "undef" ) )
        {
            int cll[2];
            b_error |= parse_param_int_list( value, cll, 2, "," );
            if( !b_error )
            {
                p->content_light_level.i_max_cll = cll[0];
                p->content_light_level.i_max_fall = cll[1];
                p->content_light_level.b_cll = 1;
            }
        }
        else
            p->content_light_level.b_cll = 0;
    }
    OPT("alternative-transfer")
        b_error |= parse_enum( value, x264_transfer_names, &p->i_alternative_transfer );
    OPT("fps")
    {
        uint32_t i_fps_num;
        uint32_t i_fps_den;
        if( !parse_uint32_pair_token( value, '/', &i_fps_num, &i_fps_den ) )
        {
            if( i_fps_num && i_fps_den )
            {
                p->i_fps_num = i_fps_num;
                p->i_fps_den = i_fps_den;
            }
            else
                b_error = 1;
        }
        else
        {
            double fps;
            if( parse_double_token( value, &fps ) || fps < 0.0005 || fps > INT_MAX )
                b_error = 1;
            else if( fps <= INT_MAX/1000.0 )
            {
                long double fps_num = (long double)fps * 1000.0L + 0.5L;
                if( fps_num > INT_MAX )
                {
                    b_error = 1;
                }
                else
                {
                    int parsed_fps_num = (int)fps_num;
                    p->i_fps_num = parsed_fps_num;
                    p->i_fps_den = 1000;
                }
            }
            else
            {
                int fps_num;
                if( !parse_int_token( value, 0, &fps_num ) )
                {
                    p->i_fps_num = fps_num;
                    p->i_fps_den = 1;
                }
                else
                    b_error = 1;
            }
        }
    }
    OPT2("ref", "frameref")
        b_error |= parse_int_token( value, 0, &p->i_frame_reference );
    OPT("dpb-size")
        b_error |= parse_int_token( value, 0, &p->i_dpb_size );
    OPT("keyint")
    {
        if( !strcmp( value, "infinite" ) )
            p->i_keyint_max = X264_KEYINT_MAX_INFINITE;
        else
            b_error |= parse_int_token( value, 0, &p->i_keyint_max );
    }
    OPT2("min-keyint", "keyint-min")
    {
        int keyint_min;
        b_error |= parse_int_token( value, 0, &keyint_min );
        if( !b_error )
        {
            p->i_keyint_min = keyint_min;
            if( p->i_keyint_max < p->i_keyint_min )
                p->i_keyint_max = p->i_keyint_min;
        }
    }
    OPT("scenecut")
    {
        int scenecut_threshold = atobool(value);
        if( b_error || scenecut_threshold )
        {
            b_error = 0;
            b_error |= parse_int_token( value, 0, &scenecut_threshold );
        }
        if( !b_error )
            p->i_scenecut_threshold = scenecut_threshold;
    }
    OPT("intra-refresh")
        OPT_BOOL( p->b_intra_refresh );
    OPT("bframes")
        b_error |= parse_int_token( value, 0, &p->i_bframe );
    OPT("b-adapt")
    {
        int bframe_adaptive = atobool(value);
        if( b_error )
        {
            b_error = 0;
            b_error |= parse_int_token( value, 0, &bframe_adaptive );
        }
        if( !b_error )
            p->i_bframe_adaptive = bframe_adaptive;
    }
    OPT("b-bias")
        b_error |= parse_int_token( value, 0, &p->i_bframe_bias );
    OPT("b-pyramid")
    {
        int bframe_pyramid;
        if( parse_enum( value, x264_b_pyramid_names, &bframe_pyramid ) )
        {
            b_error |= parse_int_token( value, 0, &bframe_pyramid );
        }
        if( !b_error )
            p->i_bframe_pyramid = bframe_pyramid;
    }
    OPT("open-gop")
        OPT_BOOL( p->b_open_gop );
    OPT("nf")
        OPT_BOOL_INV( p->b_deblocking_filter );
    OPT2("filter", "deblock")
    {
        int deblock[2];
        if( !parse_param_int_list( value, deblock, 2, ":," ) )
        {
            p->i_deblocking_filter_alphac0 = deblock[0];
            p->i_deblocking_filter_beta = deblock[1];
            p->b_deblocking_filter = 1;
        }
        else if( !parse_int_token( value, 10, &deblock[0] ) )
        {
            p->i_deblocking_filter_alphac0 = deblock[0];
            p->b_deblocking_filter = 1;
            p->i_deblocking_filter_beta = deblock[0];
        }
        else
        {
            int deblock_bool = atobool(value);
            if( !b_error )
                p->b_deblocking_filter = deblock_bool;
        }
    }
    OPT("slice-max-size")
        b_error |= parse_int_token( value, 0, &p->i_slice_max_size );
    OPT("slice-max-mbs")
        b_error |= parse_int_token( value, 0, &p->i_slice_max_mbs );
    OPT("slice-min-mbs")
        b_error |= parse_int_token( value, 0, &p->i_slice_min_mbs );
    OPT("slices")
        b_error |= parse_int_token( value, 0, &p->i_slice_count );
    OPT("slices-max")
        b_error |= parse_int_token( value, 0, &p->i_slice_count_max );
    OPT("cabac")
        OPT_BOOL( p->b_cabac );
    OPT("cabac-idc")
        b_error |= parse_int_token( value, 0, &p->i_cabac_init_idc );
    OPT("interlaced")
        OPT_BOOL( p->b_interlaced );
    OPT("tff")
    {
        int interlaced = atobool(value);
        if( !b_error )
            p->b_interlaced = p->b_tff = interlaced;
    }
    OPT("bff")
    {
        int interlaced = atobool(value);
        if( !b_error )
        {
            p->b_interlaced = interlaced;
            p->b_tff = !interlaced;
        }
    }
    OPT("constrained-intra")
        OPT_BOOL( p->b_constrained_intra );
    OPT("cqm")
    {
        if( !strcmp( value, "flat" ) )
            p->i_cqm_preset = X264_CQM_FLAT;
        else if( !strcmp( value, "jvt" ) )
            p->i_cqm_preset = X264_CQM_JVT;
        else
            CHECKED_ERROR_PARAM_STRDUP( p->psz_cqm_file, p, value );
    }
    OPT("cqmfile")
        CHECKED_ERROR_PARAM_STRDUP( p->psz_cqm_file, p, value );
    OPT("cqm4")
    {
        uint8_t cqm[4][16];
        b_error |= parse_cqm( value, cqm[0], 16 );
        b_error |= parse_cqm( value, cqm[1], 16 );
        b_error |= parse_cqm( value, cqm[2], 16 );
        b_error |= parse_cqm( value, cqm[3], 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4iy, cqm[0], sizeof(p->cqm_4iy) );
            memcpy( p->cqm_4py, cqm[1], sizeof(p->cqm_4py) );
            memcpy( p->cqm_4ic, cqm[2], sizeof(p->cqm_4ic) );
            memcpy( p->cqm_4pc, cqm[3], sizeof(p->cqm_4pc) );
        }
    }
    OPT("cqm8")
    {
        uint8_t cqm[4][64];
        b_error |= parse_cqm( value, cqm[0], 64 );
        b_error |= parse_cqm( value, cqm[1], 64 );
        b_error |= parse_cqm( value, cqm[2], 64 );
        b_error |= parse_cqm( value, cqm[3], 64 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_8iy, cqm[0], sizeof(p->cqm_8iy) );
            memcpy( p->cqm_8py, cqm[1], sizeof(p->cqm_8py) );
            memcpy( p->cqm_8ic, cqm[2], sizeof(p->cqm_8ic) );
            memcpy( p->cqm_8pc, cqm[3], sizeof(p->cqm_8pc) );
        }
    }
    OPT("cqm4i")
    {
        uint8_t cqm[2][16];
        b_error |= parse_cqm( value, cqm[0], 16 );
        b_error |= parse_cqm( value, cqm[1], 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4iy, cqm[0], sizeof(p->cqm_4iy) );
            memcpy( p->cqm_4ic, cqm[1], sizeof(p->cqm_4ic) );
        }
    }
    OPT("cqm4p")
    {
        uint8_t cqm[2][16];
        b_error |= parse_cqm( value, cqm[0], 16 );
        b_error |= parse_cqm( value, cqm[1], 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4py, cqm[0], sizeof(p->cqm_4py) );
            memcpy( p->cqm_4pc, cqm[1], sizeof(p->cqm_4pc) );
        }
    }
    OPT("cqm4iy")
    {
        uint8_t cqm[16];
        b_error |= parse_cqm( value, cqm, 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4iy, cqm, sizeof(p->cqm_4iy) );
        }
    }
    OPT("cqm4ic")
    {
        uint8_t cqm[16];
        b_error |= parse_cqm( value, cqm, 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4ic, cqm, sizeof(p->cqm_4ic) );
        }
    }
    OPT("cqm4py")
    {
        uint8_t cqm[16];
        b_error |= parse_cqm( value, cqm, 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4py, cqm, sizeof(p->cqm_4py) );
        }
    }
    OPT("cqm4pc")
    {
        uint8_t cqm[16];
        b_error |= parse_cqm( value, cqm, 16 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_4pc, cqm, sizeof(p->cqm_4pc) );
        }
    }
    OPT("cqm8i")
    {
        uint8_t cqm[2][64];
        b_error |= parse_cqm( value, cqm[0], 64 );
        b_error |= parse_cqm( value, cqm[1], 64 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_8iy, cqm[0], sizeof(p->cqm_8iy) );
            memcpy( p->cqm_8ic, cqm[1], sizeof(p->cqm_8ic) );
        }
    }
    OPT("cqm8p")
    {
        uint8_t cqm[2][64];
        b_error |= parse_cqm( value, cqm[0], 64 );
        b_error |= parse_cqm( value, cqm[1], 64 );
        if( !b_error )
        {
            p->i_cqm_preset = X264_CQM_CUSTOM;
            memcpy( p->cqm_8py, cqm[0], sizeof(p->cqm_8py) );
            memcpy( p->cqm_8pc, cqm[1], sizeof(p->cqm_8pc) );
        }
    }
    OPT("log")
        b_error |= parse_int_token( value, 0, &p->i_log_level );
    OPT("log-file")
        CHECKED_ERROR_PARAM_STRDUP( p->psz_log_file, p, value );
    OPT("log-file-level")
        if( !parse_enum( value, x264_log_level_names, &p->i_log_file_level ) )
            p->i_log_file_level += X264_LOG_NONE;
        else
            b_error |= parse_int_token( value, 0, &p->i_log_file_level );
    OPT("dump-yuv")
        CHECKED_ERROR_PARAM_STRDUP( p->psz_dump_yuv, p, value );
    OPT2("analyse", "partitions")
    {
        b_error |= parse_partitions( value, &p->analyse.inter );
    }
    OPT("8x8dct")
        OPT_BOOL( p->analyse.b_transform_8x8 );
    OPT2("weightb", "weight-b")
        OPT_BOOL( p->analyse.b_weighted_bipred );
    OPT("weightp")
        b_error |= parse_int_token( value, 0, &p->analyse.i_weighted_pred );
    OPT2("direct", "direct-pred")
        b_error |= parse_enum( value, x264_direct_pred_names, &p->analyse.i_direct_mv_pred );
    OPT("chroma-qp-offset")
        b_error |= parse_int_token( value, 0, &p->analyse.i_chroma_qp_offset );
    OPT("me")
        b_error |= parse_enum( value, x264_motion_est_names, &p->analyse.i_me_method );
    OPT2("merange", "me-range")
        b_error |= parse_int_token( value, 0, &p->analyse.i_me_range );
    OPT2("mvrange", "mv-range")
        b_error |= parse_int_token( value, 0, &p->analyse.i_mv_range );
    OPT2("mvrange-thread", "mv-range-thread")
        b_error |= parse_int_token( value, 0, &p->analyse.i_mv_range_thread );
    OPT2("subme", "subq")
        b_error |= parse_int_token( value, 0, &p->analyse.i_subpel_refine );
    OPT("psy-rd")
    {
        float psy_rd[2];
        if( !parse_param_float_list( value, psy_rd, 2, ":,|" ) )
        {
            p->analyse.f_psy_rd = psy_rd[0];
            p->analyse.f_psy_trellis = psy_rd[1];
        }
        else if( !parse_float_token( value, &psy_rd[0] ) )
        {
            p->analyse.f_psy_rd = psy_rd[0];
            p->analyse.f_psy_trellis = 0;
        }
        else
            b_error = 1;
    }
    OPT("psy")
        OPT_BOOL( p->analyse.b_psy );
    OPT("chroma-me")
        OPT_BOOL( p->analyse.b_chroma_me );
    OPT("mixed-refs")
        OPT_BOOL( p->analyse.b_mixed_references );
    OPT("trellis")
        b_error |= parse_int_token( value, 0, &p->analyse.i_trellis );
    OPT("fast-pskip")
        OPT_BOOL( p->analyse.b_fast_pskip );
    OPT("dct-decimate")
        OPT_BOOL( p->analyse.b_dct_decimate );
    OPT("deadzone-inter")
        b_error |= parse_int_token( value, 0, &p->analyse.i_luma_deadzone[0] );
    OPT("deadzone-intra")
        b_error |= parse_int_token( value, 0, &p->analyse.i_luma_deadzone[1] );
    OPT("nr")
        b_error |= parse_int_token( value, 0, &p->analyse.i_noise_reduction );
    OPT("bitrate")
    {
        int bitrate;
        if( !parse_int_token( value, 0, &bitrate ) )
        {
            p->rc.i_bitrate = bitrate;
            p->rc.i_rc_method = X264_RC_ABR;
        }
        else
            b_error = 1;
    }
    OPT2("qp", "qp-constant")
    {
        int qp_constant;
        if( !parse_int_token( value, 0, &qp_constant ) )
        {
            p->rc.i_qp_constant = qp_constant;
            p->rc.i_rc_method = X264_RC_CQP;
        }
        else
            b_error = 1;
    }
    OPT("crf")
    {
        float rf_constant;
        if( !parse_float_token( value, &rf_constant ) )
        {
            p->rc.f_rf_constant = rf_constant;
            p->rc.i_rc_method = X264_RC_CRF;
        }
        else
            b_error = 1;
    }
    OPT("crf-max")
        b_error |= parse_float_token( value, &p->rc.f_rf_constant_max );
    OPT("rc-lookahead")
        b_error |= parse_int_token( value, 0, &p->rc.i_lookahead );
    OPT2("qpmin", "qp-min")
    {
        int qp[3];
        if( !parse_param_int_list( value, qp, 3, ":," ) )
        {
            p->rc.i_qp_min[SLICE_TYPE_I] = qp[0];
            p->rc.i_qp_min[SLICE_TYPE_P] = qp[1];
            p->rc.i_qp_min[SLICE_TYPE_B] = qp[2];
            p->rc.i_qp_min_min = X264_MIN3( p->rc.i_qp_min[SLICE_TYPE_I], p->rc.i_qp_min[SLICE_TYPE_P], p->rc.i_qp_min[SLICE_TYPE_B] );
        }
        else if( !parse_int_token( value, 10, &p->rc.i_qp_min_min ) )
            p->rc.i_qp_min[SLICE_TYPE_I] = p->rc.i_qp_min[SLICE_TYPE_P] = p->rc.i_qp_min[SLICE_TYPE_B] = p->rc.i_qp_min_min;
        else
            b_error = 1;
    }
    OPT2("qpmax", "qp-max")
    {
        int qp[3];
        if( !parse_param_int_list( value, qp, 3, ":," ) )
        {
            p->rc.i_qp_max[SLICE_TYPE_I] = qp[0];
            p->rc.i_qp_max[SLICE_TYPE_P] = qp[1];
            p->rc.i_qp_max[SLICE_TYPE_B] = qp[2];
            p->rc.i_qp_max_max = X264_MAX3( p->rc.i_qp_max[SLICE_TYPE_I], p->rc.i_qp_max[SLICE_TYPE_P], p->rc.i_qp_max[SLICE_TYPE_B] );
        }
        else if( !parse_int_token( value, 10, &p->rc.i_qp_max_max ) )
            p->rc.i_qp_max[SLICE_TYPE_I] = p->rc.i_qp_max[SLICE_TYPE_P] = p->rc.i_qp_max[SLICE_TYPE_B] = p->rc.i_qp_max_max;
        else
            b_error = 1;
    }
    OPT2("qpstep", "qp-step")
        b_error |= parse_int_token( value, 0, &p->rc.i_qp_step );
    OPT("ratetol")
        if( !strcmp( value, "inf" ) )
            p->rc.f_rate_tolerance = 1e9;
        else
            b_error |= parse_float_token( value, &p->rc.f_rate_tolerance );
    OPT("vbv-maxrate")
        if( !strcmp(value, "auto_high444") )
            p->rc.i_vbv_max_bitrate = X264_VBV_MAXRATE_HIGH444;
        else if( !strcmp(value, "auto_high422") )
            p->rc.i_vbv_max_bitrate = X264_VBV_MAXRATE_HIGH422;
        else if( !strcmp(value, "auto_high10") )
            p->rc.i_vbv_max_bitrate = X264_VBV_MAXRATE_HIGH10;
        else if( !strcmp(value, "auto_high") )
            p->rc.i_vbv_max_bitrate = X264_VBV_MAXRATE_HIGH;
        else if( !strcmp(value, "auto_main") )
            p->rc.i_vbv_max_bitrate = X264_VBV_MAXRATE_MAIN;
        else
            b_error |= parse_int_token( value, 0, &p->rc.i_vbv_max_bitrate );
    OPT("vbv-bufsize")
        if( !strcmp(value, "auto_high444") )
            p->rc.i_vbv_buffer_size = X264_VBV_BUFSIZE_HIGH444;
        else if( !strcmp(value, "auto_high422") )
            p->rc.i_vbv_buffer_size = X264_VBV_BUFSIZE_HIGH422;
        else if( !strcmp(value, "auto_high10") )
            p->rc.i_vbv_buffer_size = X264_VBV_BUFSIZE_HIGH10;
        else if( !strcmp(value, "auto_high") )
            p->rc.i_vbv_buffer_size = X264_VBV_BUFSIZE_HIGH;
        else if( !strcmp(value, "auto_main") )
            p->rc.i_vbv_buffer_size = X264_VBV_BUFSIZE_MAIN;
        else
            b_error |= parse_int_token( value, 0, &p->rc.i_vbv_buffer_size );
    OPT("vbv-init")
        b_error |= parse_float_token( value, &p->rc.f_vbv_buffer_init );
    OPT2("ipratio", "ip-factor")
        b_error |= parse_float_token( value, &p->rc.f_ip_factor );
    OPT2("pbratio", "pb-factor")
        b_error |= parse_float_token( value, &p->rc.f_pb_factor );
    OPT("aq-mode")
        b_error |= parse_int_token( value, 0, &p->rc.i_aq_mode );
    OPT("aq-strength")
        b_error |= parse_float_token( value, &p->rc.f_aq_strength );
    OPT("aq-bias-strength")
        b_error |= parse_float_token( value, &p->rc.f_aq_bias_strength );
    OPT("aq-sensitivity")
        b_error |= parse_float_token( value, &p->rc.f_aq_sensitivity );
    OPT("aq-ifactor")
        b_error |= parse_float_token( value, &p->rc.f_aq_ifactor );
    OPT("aq-pfactor")
        b_error |= parse_float_token( value, &p->rc.f_aq_pfactor );
    OPT("aq-bfactor")
        b_error |= parse_float_token( value, &p->rc.f_aq_bfactor );
    OPT("aq2-strength")
    {
        if( !parse_float_token( value, &p->rc.f_aq2_strength ) )
            p->rc.b_aq2 = 1;
        else
            b_error = 1;
    }
    OPT("aq2-sensitivity")
        b_error |= parse_float_token( value, &p->rc.f_aq2_sensitivity );
    OPT("aq2-ifactor")
        b_error |= parse_float_token( value, &p->rc.f_aq2_ifactor );
    OPT("aq2-pfactor")
        b_error |= parse_float_token( value, &p->rc.f_aq2_pfactor );
    OPT("aq2-bfactor")
        b_error |= parse_float_token( value, &p->rc.f_aq2_bfactor );
    OPT("aq3-mode")
        b_error |= parse_int_token( value, 0, &p->rc.i_aq3_mode );
    OPT("aq3-strength")
    {
        float aq3_strengths[8];
        if( !parse_param_float_list( value, aq3_strengths, 8, ":," ) )
        {
            p->rc.f_aq3_strength = 0.0;
            for( int j = 0; j < 4; j++ )
            {
                p->rc.f_aq3_strengths[0][j] = aq3_strengths[j*2+0];
                p->rc.f_aq3_strengths[1][j] = aq3_strengths[j*2+1];
            }
        }
        else if( !parse_param_float_list( value, aq3_strengths, 2, ":," ) )
        {
            p->rc.f_aq3_strength = 0.0;
            p->rc.f_aq3_strengths[0][0] = aq3_strengths[0];
            p->rc.f_aq3_strengths[1][0] = aq3_strengths[1];
            for( int i = 0; i < 2; i++ )
                for( int j = 1; j < 4; j++ )
                    p->rc.f_aq3_strengths[i][j] = p->rc.f_aq3_strengths[i][0];
        }
        else if( !parse_float_token( value, &p->rc.f_aq3_strength ) )
            for( int i = 0; i < 2; i++ )
                for( int j = 0; j < 4; j++ )
                    p->rc.f_aq3_strengths[i][j] = p->rc.f_aq3_strength;
        else
            b_error = 1;
    }
    OPT("aq3-sensitivity")
        b_error |= parse_float_token( value, &p->rc.f_aq3_sensitivity );
    OPT("aq3-ifactor")
    {
        float factor[2];
        if( !parse_param_float_list( value, factor, 2, ":," ) )
        {
            p->rc.f_aq3_ifactor[0] = factor[0];
            p->rc.f_aq3_ifactor[1] = factor[1];
        }
        else if( !parse_float_token( value, &factor[0] ) )
        {
            p->rc.f_aq3_ifactor[0] = factor[0];
            p->rc.f_aq3_ifactor[1] = factor[0];
        }
        else
            b_error = 1;
    }
    OPT("aq3-pfactor")
    {
        float factor[2];
        if( !parse_param_float_list( value, factor, 2, ":," ) )
        {
            p->rc.f_aq3_pfactor[0] = factor[0];
            p->rc.f_aq3_pfactor[1] = factor[1];
        }
        else if( !parse_float_token( value, &factor[0] ) )
        {
            p->rc.f_aq3_pfactor[0] = factor[0];
            p->rc.f_aq3_pfactor[1] = factor[0];
        }
        else
            b_error = 1;
    }
    OPT("aq3-bfactor")
    {
        float factor[2];
        if( !parse_param_float_list( value, factor, 2, ":," ) )
        {
            p->rc.f_aq3_bfactor[0] = factor[0];
            p->rc.f_aq3_bfactor[1] = factor[1];
        }
        else if( !parse_float_token( value, &factor[0] ) )
        {
            p->rc.f_aq3_bfactor[0] = factor[0];
            p->rc.f_aq3_bfactor[1] = factor[0];
        }
        else
            b_error = 1;
    }
    OPT("aq3-boundary")
    {
        int boundary[3];
        if( !parse_param_int_list( value, boundary, 3, ":," ) )
        {
            p->rc.i_aq3_boundary[0] = boundary[0];
            p->rc.i_aq3_boundary[1] = boundary[1];
            p->rc.i_aq3_boundary[2] = boundary[2];
            p->rc.b_aq3_boundary = 1;
        }
        else
            b_error = 1;
    }
    OPT("fgo")
        b_error |= parse_int_token( value, 0, &p->analyse.i_fgo );
    OPT("fade-compensate")
        b_error |= parse_float_token( value, &p->rc.f_fade_compensate );
    OPT("pass")
    {
        int pass;
        b_error |= parse_int_token( value, 0, &pass );
        if( !b_error )
        {
            pass = x264_clip3( pass, 0, 3 );
            p->rc.b_stat_write = pass & 1;
            p->rc.b_stat_read = pass & 2;
        }
    }
    OPT("stats")
    {
        if( value_was_null )
            b_error = 1;
        else
        {
            char *stat_in = x264_param_strdup( p, value );
            char *stat_out = stat_in ? x264_param_strdup( p, value ) : NULL;
            if( stat_in && stat_out )
            {
                p->rc.psz_stat_in = stat_in;
                p->rc.psz_stat_out = stat_out;
            }
            else
            {
                b_error = 1;
                errortype = X264_PARAM_ALLOC_FAILED;
            }
        }
    }
    OPT("qcomp")
        b_error |= parse_float_token( value, &p->rc.f_qcompress );
    OPT("mbtree")
        OPT_BOOL( p->rc.b_mb_tree );
    OPT("qblur")
        b_error |= parse_float_token( value, &p->rc.f_qblur );
    OPT2("cplxblur", "cplx-blur")
        b_error |= parse_float_token( value, &p->rc.f_complexity_blur );
    OPT("zones")
        CHECKED_ERROR_PARAM_STRDUP( p->rc.psz_zones, p, value );
    OPT("crop-rect")
    {
        int crop[4];
        b_error |= parse_param_int_list( value, crop, 4, "," );
        if( !b_error )
        {
            p->crop_rect.i_left = crop[0];
            p->crop_rect.i_top = crop[1];
            p->crop_rect.i_right = crop[2];
            p->crop_rect.i_bottom = crop[3];
        }
    }
    OPT("psnr")
        OPT_BOOL( p->analyse.b_psnr );
    OPT("ssim")
        OPT_BOOL( p->analyse.b_ssim );
    OPT("aud")
        OPT_BOOL( p->b_aud );
    OPT("sps-id")
        b_error |= parse_int_token( value, 0, &p->i_sps_id );
    OPT("opts")
    {
#define OPTS_SET( psz_x, prefix, flag )                     \
        if( !strncasecmp( value, prefix, strlen(prefix) ) ) \
        {                                                   \
            char *opts_value = x264_param_strdup( p, value + strlen(prefix) );\
            if( opts_value )                                \
            {                                               \
                psz_x = opts_value;                         \
                p->i_opts_write |= flag;                    \
            }                                               \
            else                                            \
            {                                               \
                b_error = 1;                                \
                errortype = X264_PARAM_ALLOC_FAILED;        \
            }                                               \
        }
        OPTS_SET(      p->psz_opts[0], "preinfo:" , X264_OPTS_PREINFO  )
        else OPTS_SET( p->psz_opts[0], "0:"       , X264_OPTS_PREINFO  )
        else OPTS_SET( p->psz_opts[1], "postinfo:", X264_OPTS_POSTINFO )
        else OPTS_SET( p->psz_opts[1], "1:"       , X264_OPTS_POSTINFO )
        else OPTS_SET( p->psz_opts[2], "preopt:"  , X264_OPTS_PREOPT   )
        else OPTS_SET( p->psz_opts[2], "2:"       , X264_OPTS_PREOPT   )
        else OPTS_SET( p->psz_opts[3], "postopt:" , X264_OPTS_POSTOPT  )
        else OPTS_SET( p->psz_opts[3], "3:"       , X264_OPTS_POSTOPT  )
        else
        {
            int flag = strlen(value) == 1 && isdigit(value[0]) ? value[0] - '0' : atobool(value);
            if( !b_error && flag >= X264_OPTS_NONE && flag <= X264_OPTS_FULL )
            {
                p->i_opts_write &= ~X264_OPTS_FULL;    // clear basic flag bit
                p->i_opts_write |= flag;               // write basic flag bit
            }
            else
                b_error = 1;
        }
#undef OPTS_SET
    }
    OPT("global-header")
        OPT_BOOL_INV( p->b_repeat_headers );
    OPT("repeat-headers")
        OPT_BOOL( p->b_repeat_headers );
    OPT("annexb")
        OPT_BOOL( p->b_annexb );
    OPT("force-cfr")
        OPT_BOOL_INV( p->b_vfr_input );
    OPT("nal-hrd")
        b_error |= parse_enum( value, x264_nal_hrd_names, &p->i_nal_hrd );
    OPT("filler")
        OPT_BOOL( p->rc.b_filler );
    OPT("pic-struct")
        OPT_BOOL( p->b_pic_struct );
    OPT("fake-interlaced")
        OPT_BOOL( p->b_fake_interlaced );
    OPT("frame-packing")
        b_error |= parse_int_token( value, 0, &p->i_frame_packing );
    OPT("stitchable")
        OPT_BOOL( p->b_stitchable );
    OPT("opencl")
        OPT_BOOL( p->b_opencl );
    OPT("opencl-clbin")
        CHECKED_ERROR_PARAM_STRDUP( p->psz_clbin_file, p, value );
    OPT("opencl-device")
        b_error |= parse_int_token( value, 0, &p->i_opencl_device );
    else
    {
        b_error = 1;
        errortype = X264_PARAM_BAD_NAME;
    }
#undef OPT
#undef OPT2
#undef OPT_BOOL
#undef OPT_BOOL_INV
#undef atobool

    if( name_buf )
        free( name_buf );

    b_error |= value_was_null && !name_was_bool;
    return b_error ? errortype : 0;
}

/****************************************************************************
 * x264_param2string:
 ****************************************************************************/
static int param2string_sprintf( char *dst, char *end, const char *fmt, ... )
{
    int written;
    size_t size;
    va_list args;

    if( dst >= end )
        return 0;

    size = end - dst;
    va_start( args, fmt );
    written = vsnprintf( dst, size, fmt, args );
    va_end( args );

    if( written < 0 )
    {
        dst[0] = '\0';
        return 0;
    }

    return (size_t)written >= size ? (int)size - 1 : written;
}

char *x264_param2string( x264_param_t *p, int b_res )
{
    int len = 2000;
    char *buf, *s, *end;
    if( !p )
        return NULL;
    if( p->rc.psz_zones )
    {
        size_t zones_len = strlen( p->rc.psz_zones );
        if( zones_len > (size_t)(INT_MAX - len) )
            return NULL;
        int zones_len_int = (int)zones_len;
        len += zones_len_int;
    }
    int64_t param_string_alloc_size = len;
    buf = s = x264_malloc( param_string_alloc_size );
    if( !buf )
        return NULL;
    end = buf + len;

    if( b_res )
    {
        s += param2string_sprintf( s, end, "%dx%d ", p->i_width, p->i_height );
        s += param2string_sprintf( s, end, "fps=%u/%u ", p->i_fps_num, p->i_fps_den );
        s += param2string_sprintf( s, end, "timebase=%u/%u ", p->i_timebase_num, p->i_timebase_den );
        s += param2string_sprintf( s, end, "bitdepth=%d ", p->i_bitdepth );
    }

    if( p->b_opencl )
        s += param2string_sprintf( s, end, "opencl=%d ", p->b_opencl );
    s += param2string_sprintf( s, end, "cabac=%d", p->b_cabac );
    s += param2string_sprintf( s, end, " ref=%d", p->i_frame_reference );
    s += param2string_sprintf( s, end, " deblock=%d:%d:%d", p->b_deblocking_filter,
                  p->i_deblocking_filter_alphac0, p->i_deblocking_filter_beta );
    s += param2string_sprintf( s, end, " analyse=%#x:%#x", p->analyse.intra, p->analyse.inter );
    s += param2string_sprintf( s, end, " me=%s", x264_motion_est_names[ p->analyse.i_me_method ] );
    s += param2string_sprintf( s, end, " subme=%d", p->analyse.i_subpel_refine );
    s += param2string_sprintf( s, end, " psy=%d", p->analyse.b_psy );
    if( p->analyse.b_psy )
	{
		s += param2string_sprintf( s, end, " fade_compensate=%.2f", p->rc.f_fade_compensate );
        s += param2string_sprintf( s, end, " psy_rd=%.2f:%.2f", p->analyse.f_psy_rd, p->analyse.f_psy_trellis );
	}
    s += param2string_sprintf( s, end, " mixed_ref=%d", p->analyse.b_mixed_references );
    s += param2string_sprintf( s, end, " me_range=%d", p->analyse.i_me_range );
    s += param2string_sprintf( s, end, " chroma_me=%d", p->analyse.b_chroma_me );
    s += param2string_sprintf( s, end, " trellis=%d", p->analyse.i_trellis );
    s += param2string_sprintf( s, end, " 8x8dct=%d", p->analyse.b_transform_8x8 );
    s += param2string_sprintf( s, end, " cqm=%d", p->i_cqm_preset );
    s += param2string_sprintf( s, end, " deadzone=%d,%d", p->analyse.i_luma_deadzone[0], p->analyse.i_luma_deadzone[1] );
    s += param2string_sprintf( s, end, " fast_pskip=%d", p->analyse.b_fast_pskip );
    s += param2string_sprintf( s, end, " chroma_qp_offset=%d", p->analyse.i_chroma_qp_offset );
    s += param2string_sprintf( s, end, " threads=%d", p->i_threads );
    s += param2string_sprintf( s, end, " lookahead_threads=%d", p->i_lookahead_threads );
    s += param2string_sprintf( s, end, " sliced_threads=%d", p->b_sliced_threads );
    if( p->i_slice_count )
        s += param2string_sprintf( s, end, " slices=%d", p->i_slice_count );
    if( p->i_slice_count_max )
        s += param2string_sprintf( s, end, " slices_max=%d", p->i_slice_count_max );
    if( p->i_slice_max_size )
        s += param2string_sprintf( s, end, " slice_max_size=%d", p->i_slice_max_size );
    if( p->i_slice_max_mbs )
        s += param2string_sprintf( s, end, " slice_max_mbs=%d", p->i_slice_max_mbs );
    if( p->i_slice_min_mbs )
        s += param2string_sprintf( s, end, " slice_min_mbs=%d", p->i_slice_min_mbs );
    s += param2string_sprintf( s, end, " nr=%d", p->analyse.i_noise_reduction );
    s += param2string_sprintf( s, end, " decimate=%d", p->analyse.b_dct_decimate );
    s += param2string_sprintf( s, end, " interlaced=%s", p->b_interlaced ? p->b_tff ? "tff" : "bff" : p->b_fake_interlaced ? "fake" : "0" );
    s += param2string_sprintf( s, end, " bluray_compat=%d", p->b_bluray_compat );
    if( p->b_stitchable )
        s += param2string_sprintf( s, end, " stitchable=%d", p->b_stitchable );

    s += param2string_sprintf( s, end, " constrained_intra=%d", p->b_constrained_intra );
    s += param2string_sprintf( s, end, " fgo=%d", p->analyse.i_fgo );

    s += param2string_sprintf( s, end, " bframes=%d", p->i_bframe );
    if( p->i_bframe )
    {
        s += param2string_sprintf( s, end, " b_pyramid=%d b_adapt=%d b_bias=%d direct=%d weightb=%d open_gop=%d",
                      p->i_bframe_pyramid, p->i_bframe_adaptive, p->i_bframe_bias,
                      p->analyse.i_direct_mv_pred, p->analyse.b_weighted_bipred, p->b_open_gop );
    }
    s += param2string_sprintf( s, end, " weightp=%d", p->analyse.i_weighted_pred > 0 ? p->analyse.i_weighted_pred : 0 );

    if( p->i_keyint_max == X264_KEYINT_MAX_INFINITE )
        s += param2string_sprintf( s, end, " keyint=infinite" );
    else
        s += param2string_sprintf( s, end, " keyint=%d", p->i_keyint_max );
    s += param2string_sprintf( s, end, " keyint_min=%d scenecut=%d intra_refresh=%d",
                  p->i_keyint_min, p->i_scenecut_threshold, p->b_intra_refresh );

    if( p->rc.b_mb_tree || p->rc.i_vbv_buffer_size )
        s += param2string_sprintf( s, end, " rc_lookahead=%d", p->rc.i_lookahead );

    s += param2string_sprintf( s, end, " rc=%s mbtree=%d", p->rc.i_rc_method == X264_RC_ABR ?
                               ( p->rc.b_stat_read ? "2pass" : p->rc.i_vbv_max_bitrate == p->rc.i_bitrate ? "cbr" : "abr" )
                               : p->rc.i_rc_method == X264_RC_CRF ? "crf" : "cqp", p->rc.b_mb_tree );
    if( p->rc.i_rc_method == X264_RC_ABR || p->rc.i_rc_method == X264_RC_CRF )
    {
        if( p->rc.i_rc_method == X264_RC_CRF )
            s += param2string_sprintf( s, end, " crf=%.4f", p->rc.f_rf_constant );
        else
            s += param2string_sprintf( s, end, " bitrate=%d ratetol=%.1f",
                          p->rc.i_bitrate, p->rc.f_rate_tolerance );
        s += param2string_sprintf( s, end, " qcomp=%.2f qpmin=%d:%d:%d qpmax=%d:%d:%d qpstep=%d",
                      p->rc.f_qcompress,
                      p->rc.i_qp_min[SLICE_TYPE_I], p->rc.i_qp_min[SLICE_TYPE_P], p->rc.i_qp_min[SLICE_TYPE_B],
                      p->rc.i_qp_max[SLICE_TYPE_I], p->rc.i_qp_max[SLICE_TYPE_P], p->rc.i_qp_max[SLICE_TYPE_B],
                      p->rc.i_qp_step );
        if( p->rc.b_stat_read )
            s += param2string_sprintf( s, end, " cplxblur=%.1f qblur=%.1f",
                          p->rc.f_complexity_blur, p->rc.f_qblur );
        if( p->rc.i_vbv_buffer_size )
        {
            s += param2string_sprintf( s, end, " vbv_maxrate=%d vbv_bufsize=%d",
                          p->rc.i_vbv_max_bitrate, p->rc.i_vbv_buffer_size );
            if( p->rc.i_rc_method == X264_RC_CRF )
                s += param2string_sprintf( s, end, " crf_max=%.1f", p->rc.f_rf_constant_max );
        }
    }
    else if( p->rc.i_rc_method == X264_RC_CQP )
        s += param2string_sprintf( s, end, " qp=%d", p->rc.i_qp_constant );

    if( p->rc.i_vbv_buffer_size )
        s += param2string_sprintf( s, end, " nal_hrd=%s filler=%d", x264_nal_hrd_names[p->i_nal_hrd], p->rc.b_filler );
    if( p->crop_rect.i_left | p->crop_rect.i_top | p->crop_rect.i_right | p->crop_rect.i_bottom )
        s += param2string_sprintf( s, end, " crop_rect=%d,%d,%d,%d", p->crop_rect.i_left, p->crop_rect.i_top,
                                                   p->crop_rect.i_right, p->crop_rect.i_bottom );
    if( p->mastering_display.b_mastering_display )
        s += param2string_sprintf( s, end, " mastering-display=G(%d,%d)B(%d,%d)R(%d,%d)WP(%d,%d)L(%"PRId64",%"PRId64")",
                      p->mastering_display.i_green_x, p->mastering_display.i_green_y,
                      p->mastering_display.i_blue_x, p->mastering_display.i_blue_y,
                      p->mastering_display.i_red_x, p->mastering_display.i_red_y,
                      p->mastering_display.i_white_x, p->mastering_display.i_white_y,
                      p->mastering_display.i_display_max, p->mastering_display.i_display_min );
    if( p->content_light_level.b_cll )
        s += param2string_sprintf( s, end, " cll=%d,%d",
                      p->content_light_level.i_max_cll, p->content_light_level.i_max_fall );
    if( p->i_frame_packing >= 0 )
        s += param2string_sprintf( s, end, " frame-packing=%d", p->i_frame_packing );

    if( !(p->rc.i_rc_method == X264_RC_CQP && p->rc.i_qp_constant == 0) )
    {
        s += param2string_sprintf( s, end, " ip_ratio=%.2f", p->rc.f_ip_factor );
        if( p->i_bframe && !p->rc.b_mb_tree )
            s += param2string_sprintf( s, end, " pb_ratio=%.2f", p->rc.f_pb_factor );
        s += param2string_sprintf( s, end, " aq=%d", p->rc.i_aq_mode );
        if( p->rc.i_aq_mode )
		{
            s += param2string_sprintf( s, end, ":%.2f", p->rc.f_aq_strength );
            s += param2string_sprintf( s, end, " aq-sensitivity=%.2f", p->rc.f_aq_sensitivity );
            s += param2string_sprintf( s, end, " aq-factor=%.2f:%.2f:%.2f", p->rc.f_aq_ifactor,
                                                          p->rc.f_aq_pfactor,
                                                          p->rc.f_aq_bfactor );
            if( p->rc.i_aq_mode == X264_AQ_AUTOVARIANCE_BIASED )
                s += param2string_sprintf( s, end, ":%.2f", p->rc.f_aq_bias_strength );
		}
        s += param2string_sprintf( s, end, " aq2=%d", p->rc.b_aq2 );
        if( p->rc.b_aq2 )
        {
            s += param2string_sprintf( s, end, ":%.2f", p->rc.f_aq2_strength );
            s += param2string_sprintf( s, end, " aq2-sensitivity=%.2f", p->rc.f_aq2_sensitivity );
            s += param2string_sprintf( s, end, " aq2-factor=%.2f:%.2f:%.2f", p->rc.f_aq2_ifactor,
                                                           p->rc.f_aq2_pfactor,
                                                           p->rc.f_aq2_bfactor );
        }
        s += param2string_sprintf( s, end, " aq3=%d", p->rc.i_aq3_mode );
        if( p->rc.i_aq3_mode )
        {
            s += param2string_sprintf( s, end, ":[%.2f:%.2f]:[%.2f:%.2f]:[%.2f:%.2f]:[%.2f:%.2f]",
                          p->rc.f_aq3_strengths[0][0], p->rc.f_aq3_strengths[1][0], p->rc.f_aq3_strengths[0][1], p->rc.f_aq3_strengths[1][1],
                          p->rc.f_aq3_strengths[0][2], p->rc.f_aq3_strengths[1][2], p->rc.f_aq3_strengths[0][3], p->rc.f_aq3_strengths[1][3] );
            s += param2string_sprintf( s, end, " aq3-sensitivity=%.2f", p->rc.f_aq3_sensitivity );
            s += param2string_sprintf( s, end, " aq3-factor=[%.2f:%.2f]:[%.2f:%.2f]:[%.2f:%.2f]", p->rc.f_aq3_ifactor[0], p->rc.f_aq3_ifactor[1],
                                                                                p->rc.f_aq3_pfactor[0], p->rc.f_aq3_pfactor[1],
                                                                                p->rc.f_aq3_bfactor[0], p->rc.f_aq3_bfactor[1] );
            s += param2string_sprintf( s, end, " aq3-boundary=%d:%d:%d", p->rc.i_aq3_boundary[0], p->rc.i_aq3_boundary[1], p->rc.i_aq3_boundary[2] );
        }
        if( p->rc.psz_zones )
            s += param2string_sprintf( s, end, " zones=%s", p->rc.psz_zones );
        else if( p->rc.i_zones )
            s += param2string_sprintf( s, end, " zones" );
    }

    return buf;
}
