/*****************************************************************************
 * gop.c: GOP output muxer
 *****************************************************************************
 * MIT License
 *
 * Copyright (c) 2018-2019 Xinyue Lu
 * Adapted for x264 by x264 project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *****************************************************************************
 * The MIT License applies to this file only.
 *****************************************************************************/

#include "gop.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>

#ifdef _MSC_VER
    #include <windows.h>
    #define sleep(x) Sleep((x) * 1000)
#else
    #include <unistd.h>
#endif

#define TIME_WAIT 30
#define GOP_ISOM_MATRIX_INDEX_UNSPECIFIED 2
#define GOP_FRAME_DIGITS_MIN 6

static int gop_add_size( size_t *dst, size_t add )
{
    if( *dst > SIZE_MAX - add )
        return -1;
    *dst += add;
    return 0;
}

static size_t gop_frame_digits( int frame )
{
    size_t digits = 1;
    while( frame >= 10 )
    {
        frame /= 10;
        digits++;
    }
    return digits > GOP_FRAME_DIGITS_MIN ? digits : GOP_FRAME_DIGITS_MIN;
}

static char *gop_alloc_prefixed_filename( gop_hnd_t *hnd, const char *suffix )
{
    size_t dir_len = hnd->dir_prefix ? strlen( hnd->dir_prefix ) : 0;
    size_t prefix_len = strlen( hnd->filename_prefix );
    size_t suffix_len = strlen( suffix );
    size_t filename_len = dir_len;
    if( gop_add_size( &filename_len, prefix_len ) ||
        gop_add_size( &filename_len, suffix_len ) ||
        gop_add_size( &filename_len, 1 ) )
        return NULL;

    char *filename = malloc( filename_len );
    if( !filename )
        return NULL;

    int written = snprintf( filename, filename_len, "%s%s%s",
                            hnd->dir_prefix ? hnd->dir_prefix : "", hnd->filename_prefix, suffix );
    if( written < 0 || (size_t)written != filename_len - 1 )
    {
        free( filename );
        return NULL;
    }

    return filename;
}

static char *gop_alloc_data_filename( gop_hnd_t *hnd )
{
    size_t dir_len = hnd->dir_prefix ? strlen( hnd->dir_prefix ) : 0;
    size_t prefix_len = strlen( hnd->filename_prefix );
    size_t frame_digits = gop_frame_digits( hnd->i_numframe );
    size_t suffix_len = sizeof(".264-gop-data") - 1;
    size_t filename_len = dir_len;
    if( gop_add_size( &filename_len, prefix_len ) ||
        gop_add_size( &filename_len, 1 ) ||
        gop_add_size( &filename_len, frame_digits ) ||
        gop_add_size( &filename_len, suffix_len ) ||
        gop_add_size( &filename_len, 1 ) )
        return NULL;

    char *filename = malloc( filename_len );
    if( !filename )
        return NULL;

    int written = snprintf( filename, filename_len, "%s%s-%06d.264-gop-data",
                            hnd->dir_prefix ? hnd->dir_prefix : "", hnd->filename_prefix, hnd->i_numframe );
    if( written < 0 || (size_t)written != filename_len - 1 )
    {
        free( filename );
        return NULL;
    }

    return filename;
}

static FILE* gop_open_file_for_write( const char *fname, int retry, gop_hnd_t *hnd )
{
    while( 1 )
    {
        FILE *fp = x264_fopen( fname, "wb" );
        if( fp != NULL )
            return fp;
        if( !retry )
            break;
        /* Retrying */
        x264_cli_log( "gop", X264_LOG_WARNING,
            "unable to open file %s for writing, error %d, retrying in %d seconds.\n",
            fname, errno, TIME_WAIT );
        sleep( TIME_WAIT );
    }
    /* Failed */
    hnd->b_fail = 1;
    x264_cli_log( "gop", X264_LOG_ERROR,
        "unable to open file %s for writing, error %d.\n", fname, errno );
    return NULL;
}

static void gop_clean_up( gop_hnd_t *hnd )
{
    if( hnd->data_file )
    {
        fclose( hnd->data_file );
        hnd->data_file = NULL;
    }
    if( hnd->gop_file )
    {
        fclose( hnd->gop_file );
        hnd->gop_file = NULL;
    }
    if( hnd->filename_prefix )
    {
        free( hnd->filename_prefix );
        hnd->filename_prefix = NULL;
    }
    if( hnd->dir_prefix )
    {
        free( hnd->dir_prefix );
        hnd->dir_prefix = NULL;
    }
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_encoder, char *audio_parameters )
{
    gop_hnd_t *hnd = calloc( 1, sizeof(gop_hnd_t) );
    if( !hnd )
        return -1;

    hnd->gop_file = gop_open_file_for_write( psz_filename, 0, hnd );
    if( !hnd->gop_file )
    {
        free( hnd );
        return -1;
    }

    /* Extract directory and filename prefix */
    char *last_slash = strrchr( psz_filename, '/' );
    if( !last_slash )
        last_slash = strrchr( psz_filename, '\\' );

    if( last_slash )
    {
        ptrdiff_t dir_diff = last_slash - psz_filename + 1;
        if( dir_diff <= 0 )
        {
            gop_clean_up( hnd );
            free( hnd );
            return -1;
        }
        size_t dir_len = (size_t)dir_diff;
        hnd->dir_prefix = malloc( dir_len + 1 );
        if( !hnd->dir_prefix )
        {
            gop_clean_up( hnd );
            free( hnd );
            return -1;
        }
        memcpy( hnd->dir_prefix, psz_filename, dir_len );
        hnd->dir_prefix[dir_len] = '\0';
        psz_filename = last_slash + 1;
    }
    else
    {
        hnd->dir_prefix = strdup( "" );
        if( !hnd->dir_prefix )
        {
            gop_clean_up( hnd );
            free( hnd );
            return -1;
        }
    }

    /* Extract filename without extension */
    char *dot = strrchr( psz_filename, '.' );
    size_t prefix_len = dot ? (size_t)(dot - psz_filename) : strlen( psz_filename );
    if( prefix_len == SIZE_MAX )
    {
        gop_clean_up( hnd );
        free( hnd );
        return -1;
    }

    hnd->filename_prefix = malloc( prefix_len + 1 );
    if( !hnd->filename_prefix )
    {
        gop_clean_up( hnd );
        free( hnd );
        return -1;
    }
    memcpy( hnd->filename_prefix, psz_filename, prefix_len );
    hnd->filename_prefix[prefix_len] = '\0';

    hnd->i_numframe = 0;
    hnd->b_fail = 0;

    *p_handle = hnd;
    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;
    if( !hnd || !hnd->filename_prefix || !hnd->gop_file || !p_param )
        return -1;

    /* Force Annex-B off and no repeat headers for GOP output */
    p_param->b_annexb = 0;
    p_param->b_repeat_headers = 0;

    /* Build options filename */
    char *opt_filename = gop_alloc_prefixed_filename( hnd, ".options" );
    if( !opt_filename )
        return -1;

    FILE *opt_file = gop_open_file_for_write( opt_filename, 0, hnd );
    free( opt_filename );
    if( !opt_file )
        return -1;

    if( fprintf( hnd->gop_file, "#options %s.options\n", hnd->filename_prefix ) < 0 )
    {
        fclose( opt_file );
        return -1;
    }

    fprintf( opt_file, "b-frames %d\n", p_param->i_bframe );
    fprintf( opt_file, "b-pyramid %d\n", p_param->i_bframe_pyramid );
    fprintf( opt_file, "input-timebase-num %d\n", p_param->i_timebase_num );
    fprintf( opt_file, "input-timebase-den %d\n", p_param->i_timebase_den );
    fprintf( opt_file, "output-fps-num %d\n", p_param->i_fps_num );
    fprintf( opt_file, "output-fps-den %d\n", p_param->i_fps_den );
    fprintf( opt_file, "source-width %d\n", p_param->i_width );
    fprintf( opt_file, "source-height %d\n", p_param->i_height );
    fprintf( opt_file, "sar-width %d\n", p_param->vui.i_sar_width );
    fprintf( opt_file, "sar-height %d\n", p_param->vui.i_sar_height );
    fprintf( opt_file, "primaries-index %d\n", p_param->vui.i_colorprim );
    fprintf( opt_file, "transfer-index %d\n", p_param->vui.i_transfer );
    fprintf( opt_file, "matrix-index %d\n",
             p_param->vui.i_colmatrix >= 0 ? p_param->vui.i_colmatrix : GOP_ISOM_MATRIX_INDEX_UNSPECIFIED );
    fprintf( opt_file, "full-range %d\n",
             p_param->vui.b_fullrange >= 0 ? p_param->vui.b_fullrange : 0 );

    int opt_error = ferror( opt_file );
    opt_error |= fclose( opt_file );
    if( opt_error )
        return -1;
    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;
    int size = 0;
    if( !hnd || !hnd->filename_prefix || !hnd->gop_file || !p_nal )
        return -1;

    /* Build headers filename */
    char *hdr_filename = gop_alloc_prefixed_filename( hnd, ".headers" );
    if( !hdr_filename )
        return -1;

    FILE *hdr_file = gop_open_file_for_write( hdr_filename, 0, hnd );
    free( hdr_filename );
    if( !hdr_file )
        return -1;

    if( fprintf( hnd->gop_file, "#headers %s.headers\n", hnd->filename_prefix ) < 0 )
    {
        fclose( hdr_file );
        return -1;
    }

    /* Write all NALs (SPS, PPS, etc) */
    int nal_count = 3; /* SPS, PPS, and potentially SEI */
    for( int i = 0; i < nal_count; i++ )
    {
        int payload = p_nal[i].i_payload;
        if( payload < 0 || (payload && !p_nal[i].p_payload) || size > INT_MAX - payload ||
            (payload > 0 && fwrite( p_nal[i].p_payload, (size_t)payload, 1, hdr_file ) != 1) )
        {
            fclose( hdr_file );
            return -1;
        }
        size += payload;
    }

    if( fclose( hdr_file ) )
        return -1;
    return size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;
    enum { GOP_TIMESTAMP_PAYLOAD_SIZE = 2 * sizeof(int64_t) };
    if( !hnd || !hnd->filename_prefix || !hnd->gop_file || !p_picture || i_size < 0 || (i_size && !p_nalu) ||
        hnd->i_numframe == INT_MAX || GOP_TIMESTAMP_PAYLOAD_SIZE > UINT8_MAX )
        return -1;
    int is_keyframe = (p_picture->i_type == X264_TYPE_IDR);

    if( is_keyframe )
    {
        /* Close previous data file if exists */
        if( hnd->data_file )
        {
            fclose( hnd->data_file );
            hnd->data_file = NULL;
        }

        /* Create new data file for this GOP */
        char *data_filename = gop_alloc_data_filename( hnd );
        if( !data_filename )
            return -1;

        hnd->data_file = gop_open_file_for_write( data_filename, hnd->i_numframe > 0, hnd );
        if( !hnd->data_file )
        {
            free( data_filename );
            return -1;
        }

        /* Add entry to GOP index file */
        char *basename = strrchr( data_filename, '/' );
        if( !basename )
            basename = strrchr( data_filename, '\\' );
        basename = basename ? basename + 1 : data_filename;

        if( fprintf( hnd->gop_file, "%s\n", basename ) < 0 || fflush( hnd->gop_file ) )
        {
            fclose( hnd->data_file );
            hnd->data_file = NULL;
            free( data_filename );
            return -1;
        }

        free( data_filename );
    }

    if( !hnd->data_file )
        return -1;

    /* Write timestamp header (PTS + DTS) */
    const uint8_t ts_header[4] = { 0, 0, 0, GOP_TIMESTAMP_PAYLOAD_SIZE };
    if( fwrite( ts_header, sizeof(ts_header), 1, hnd->data_file ) != 1 ||
        fwrite( &p_picture->i_pts, sizeof(int64_t), 1, hnd->data_file ) != 1 ||
        fwrite( &p_picture->i_dts, sizeof(int64_t), 1, hnd->data_file ) != 1 )
        return -1;

    /* Write NAL data */
    if( !i_size || fwrite( p_nalu, (size_t)i_size, 1, hnd->data_file ) == 1 )
    {
        hnd->i_numframe++;
        return i_size;
    }

    return -1;
}

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;

    if( !hnd )
        return 0;

    gop_clean_up( hnd );
    free( hnd );
    return 0;
}

const cli_output_t gop_output = { open_file, set_param, write_headers, write_frame, close_file };
