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
#include <errno.h>

#ifdef _MSC_VER
    #include <windows.h>
    #define sleep(x) Sleep((x) * 1000)
#else
    #include <unistd.h>
#endif

#define TIME_WAIT 30
#define GOP_ISOM_MATRIX_INDEX_UNSPECIFIED 2

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
        int dir_len = (int)(last_slash - psz_filename + 1);
        hnd->dir_prefix = malloc( dir_len + 1 );
        if( !hnd->dir_prefix )
        {
            gop_clean_up( hnd );
            free( hnd );
            return -1;
        }
        strncpy( hnd->dir_prefix, psz_filename, dir_len );
        hnd->dir_prefix[dir_len] = '\0';
        psz_filename = last_slash + 1;
    }
    else
    {
        hnd->dir_prefix = strdup( "" );
    }

    /* Extract filename without extension */
    char *dot = strrchr( psz_filename, '.' );
    int prefix_len;
    if( dot )
        prefix_len = (int)(dot - psz_filename);
    else
        prefix_len = (int)strlen( psz_filename );

    hnd->filename_prefix = malloc( prefix_len + 1 );
    if( !hnd->filename_prefix )
    {
        gop_clean_up( hnd );
        free( hnd );
        return -1;
    }
    strncpy( hnd->filename_prefix, psz_filename, prefix_len );
    hnd->filename_prefix[prefix_len] = '\0';

    hnd->i_numframe = 0;
    hnd->b_fail = 0;

    *p_handle = hnd;
    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;

    /* Force Annex-B off and no repeat headers for GOP output */
    p_param->b_annexb = 0;
    p_param->b_repeat_headers = 0;

    /* Build options filename */
    int opt_len = (hnd->dir_prefix ? (int)strlen(hnd->dir_prefix) : 0) +
                  (int)strlen(hnd->filename_prefix) + 8; /* .options */
    char *opt_filename = malloc( opt_len + 1 );
    if( !opt_filename )
        return -1;

    snprintf( opt_filename, opt_len + 1, "%s%s.options",
              hnd->dir_prefix ? hnd->dir_prefix : "", hnd->filename_prefix );

    FILE *opt_file = gop_open_file_for_write( opt_filename, 0, hnd );
    free( opt_filename );
    if( !opt_file )
        return -1;

    fprintf( hnd->gop_file, "#options %s.options\n", hnd->filename_prefix );

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

    fclose( opt_file );
    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;
    int size = 0;

    /* Build headers filename */
    int hdr_len = (hnd->dir_prefix ? (int)strlen(hnd->dir_prefix) : 0) +
                  (int)strlen(hnd->filename_prefix) + 8; /* .headers */
    char *hdr_filename = malloc( hdr_len + 1 );
    if( !hdr_filename )
        return -1;

    snprintf( hdr_filename, hdr_len + 1, "%s%s.headers",
              hnd->dir_prefix ? hnd->dir_prefix : "", hnd->filename_prefix );

    FILE *hdr_file = gop_open_file_for_write( hdr_filename, 0, hnd );
    free( hdr_filename );
    if( !hdr_file )
        return -1;

    fprintf( hnd->gop_file, "#headers %s.headers\n", hnd->filename_prefix );

    /* Write all NALs (SPS, PPS, etc) */
    int nal_count = 3; /* SPS, PPS, and potentially SEI */
    for( int i = 0; i < nal_count; i++ )
    {
        if( fwrite( p_nal[i].p_payload, p_nal[i].i_payload, 1, hdr_file ) )
            size += p_nal[i].i_payload;
    }

    fclose( hdr_file );
    return size;
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    gop_hnd_t *hnd = (gop_hnd_t *)handle;
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
        int data_len = (hnd->dir_prefix ? (int)strlen(hnd->dir_prefix) : 0) +
                       (int)strlen(hnd->filename_prefix) + 20; /* -NNNNNN.264-gop-data */
        char *data_filename = malloc( data_len + 1 );
        if( !data_filename )
            return -1;

        snprintf( data_filename, data_len + 1, "%s%s-%06d.264-gop-data",
                  hnd->dir_prefix ? hnd->dir_prefix : "", hnd->filename_prefix, hnd->i_numframe );

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

        fprintf( hnd->gop_file, "%s\n", basename );
        fflush( hnd->gop_file );

        free( data_filename );
    }

    if( !hnd->data_file )
        return -1;

    /* Write timestamp header (PTS + DTS) */
    int8_t ts_len = 2 * sizeof(int64_t);
    int8_t ts_header[4] = { 0, 0, 0, ts_len };
    fwrite( ts_header, 4, 1, hnd->data_file );
    fwrite( &p_picture->i_pts, sizeof(int64_t), 1, hnd->data_file );
    fwrite( &p_picture->i_dts, sizeof(int64_t), 1, hnd->data_file );

    /* Write NAL data */
    if( fwrite( p_nalu, i_size, 1, hnd->data_file ) )
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
