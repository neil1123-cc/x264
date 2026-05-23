/*****************************************************************************
 * raw.c: raw input
 *****************************************************************************
 * Copyright (C) 2003-2025 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
 *          Steven Walters <kemuri9@gmail.com>
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

#include "input.h"
#include <limits.h>

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "raw", __VA_ARGS__ )
#define FAIL_IF_ERROR_CLEANUP( cond, ... )\
do\
{\
    if( cond )\
    {\
        x264_cli_log( "raw", X264_LOG_ERROR, __VA_ARGS__ );\
        goto fail;\
    }\
} while( 0 )

typedef struct
{
    FILE *fh;
    int next_frame;
    int64_t plane_size[4];
    int64_t frame_size;
    int bit_depth;
    cli_mmap_t mmap;
    int use_mmap;
	int x264_bit_depth;
} raw_hnd_t;

static int raw_parse_dimension( const char *p, char **end, int *dst )
{
    long value;

    if( !p || *p < '0' || *p > '9' )
        return -1;

    errno = 0;
    value = strtol( p, end, 10 );
    if( *end == p || errno == ERANGE || value <= 0 || value > MAX_RESOLUTION )
        return -1;

    *dst = (int)value;
    return 0;
}

static int raw_parse_resolution( const char *p, char **end, int *width, int *height )
{
    int parsed_width, parsed_height;

    if( !end || !width || !height ||
        raw_parse_dimension( p, end, &parsed_width ) || **end != 'x' ||
        raw_parse_dimension( *end + 1, end, &parsed_height ) )
        return -1;

    *width = parsed_width;
    *height = parsed_height;
    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    if( !psz_filename || !p_handle || !info || !opt )
        return -1;
    *p_handle = NULL;

    raw_hnd_t *h = calloc( 1, sizeof(raw_hnd_t) );
    if( !h )
        return -1;

    video_info_t updated_info = *info;

    if( !opt->resolution )
    {
        /* try to parse the file name */
        for( char *p = psz_filename; *p; p++ )
            if( *p >= '0' && *p <= '9' )
            {
                char *end;
                int width, height;
                if( !raw_parse_resolution( p, &end, &width, &height ) )
                {
                    updated_info.width = width;
                    updated_info.height = height;
                    break;
                }
            }
    }
    else
    {
        char *end;
        int width, height;
        FAIL_IF_ERROR_CLEANUP( raw_parse_resolution( opt->resolution, &end, &width, &height ) || *end,
                               "invalid resolution `%s'\n", opt->resolution );
        updated_info.width = width;
        updated_info.height = height;
    }
    FAIL_IF_ERROR_CLEANUP( !updated_info.width || !updated_info.height, "raw input requires a resolution.\n" );
    if( opt->colorspace )
    {
        int csp;
        for( csp = X264_CSP_CLI_MAX-1; csp > X264_CSP_NONE; csp-- )
        {
            if( x264_cli_csps[csp].name && !strcasecmp( x264_cli_csps[csp].name, opt->colorspace ) )
                break;
        }
        FAIL_IF_ERROR_CLEANUP( csp == X264_CSP_NONE, "unsupported colorspace `%s'\n", opt->colorspace );
        updated_info.csp = csp;
    }
    else /* default */
        updated_info.csp = X264_CSP_I420;

    h->bit_depth = opt->bit_depth;
	h->x264_bit_depth = opt->x264_bit_depth;
    FAIL_IF_ERROR_CLEANUP( h->bit_depth < 8 || h->bit_depth > 16, "unsupported bit depth `%d'\n", h->bit_depth );
    if( h->bit_depth > 8 )
	{
        updated_info.csp |= X264_CSP_HIGH_DEPTH;
        if( h->bit_depth == opt->x264_bit_depth )
        {
            /* HACK: totally skips depth filter to prevent dither error */
            updated_info.csp |= X264_CSP_SKIP_DEPTH_FILTER;
        }
	}

    if( !strcmp( psz_filename, "-" ) )
        h->fh = stdin;
    else
        h->fh = x264_fopen( psz_filename, "rb" );
    if( h->fh == NULL )
        goto fail;

    updated_info.thread_safe = 1;
    updated_info.num_frames  = 0;
    updated_info.vfr         = 0;

    const x264_cli_csp_t *csp = x264_cli_get_csp( updated_info.csp );
    FAIL_IF_ERROR_CLEANUP( !csp, "unsupported colorspace\n" );
    int pixel_depth = x264_cli_csp_depth_factor( updated_info.csp );
    FAIL_IF_ERROR_CLEANUP( pixel_depth <= 0, "invalid pixel depth\n" );
    for( int i = 0; i < csp->planes; i++ )
    {
        int64_t plane_size = x264_cli_pic_plane_size( updated_info.csp, updated_info.width, updated_info.height, i );
        FAIL_IF_ERROR_CLEANUP( plane_size <= 0 || h->frame_size > INT64_MAX - plane_size, "invalid frame size\n" );
        h->frame_size += plane_size;
        /* x264_cli_pic_plane_size returns the size in bytes, we need the value in pixels from here on */
        h->plane_size[i] = plane_size / pixel_depth;
    }

    if( x264_is_regular_file( h->fh ) )
    {
        FAIL_IF_ERROR_CLEANUP( fseek( h->fh, 0, SEEK_END ), "unable to seek input file\n" );
        int64_t size = ftell( h->fh );
        FAIL_IF_ERROR_CLEANUP( size < 0, "unable to determine input file size\n" );
        FAIL_IF_ERROR_CLEANUP( fseek( h->fh, 0, SEEK_SET ), "unable to seek input file\n" );
        int64_t num_frames = size / h->frame_size;
        FAIL_IF_ERROR_CLEANUP( num_frames > INT_MAX, "too many frames\n" );
        updated_info.num_frames = (int)num_frames;
        FAIL_IF_ERROR_CLEANUP( !updated_info.num_frames, "empty input file\n" );

        /* Attempt to use memory-mapped input frames if possible */
        if( !(h->bit_depth & 7) )
            h->use_mmap = !x264_cli_mmap_init( &h->mmap, h->fh );
    }

    *info = updated_info;
    *p_handle = h;
    return 0;

fail:
    if( h->use_mmap )
        x264_cli_mmap_close( &h->mmap );
    if( h->fh && h->fh != stdin )
    {
        if( fclose( h->fh ) )
            x264_cli_log( "raw", X264_LOG_ERROR, "failed to close input file `%s'\n", psz_filename );
    }
    free( h );
    return -1;
}

static int read_frame_internal( cli_pic_t *pic, raw_hnd_t *h, int bit_depth_uc )
{
    if( !pic || !h || (!h->use_mmap && !h->fh) || pic->img.planes <= 0 || pic->img.planes > 4 )
        return -1;
    if( h->use_mmap && !pic->img.plane[0] )
        return -1;
    int pixel_depth = x264_cli_csp_depth_factor( pic->img.csp );
    if( pixel_depth <= 0 )
        return -1;

    for( int i = 0; i < pic->img.planes; i++ )
    {
        if( h->plane_size[i] < 0 )
            return -1;
        if( h->use_mmap )
        {
            if( i )
                pic->img.plane[i] = pic->img.plane[i-1] + pixel_depth * h->plane_size[i-1];
        }
        else if( (h->plane_size[i] && !pic->img.plane[i]) ||
                 fread( pic->img.plane[i], (size_t)pixel_depth, (size_t)h->plane_size[i], h->fh ) != (size_t)h->plane_size[i] )
            return -1;

        if( bit_depth_uc && h->bit_depth != h->x264_bit_depth )
        {
            /* upconvert non 16bit high depth planes to 16bit using the same
             * algorithm as used in the depth filter. */
            uint16_t *plane = (uint16_t*)pic->img.plane[i];
            int64_t pixel_count = h->plane_size[i];
            int lshift = 16 - h->bit_depth;
            for( int64_t j = 0; j < pixel_count; j++ )
                plane[j] = (unsigned)plane[j] << lshift;
        }
    }
    return 0;
}

static int read_frame( cli_pic_t *pic, hnd_t handle, int i_frame )
{
    raw_hnd_t *h = handle;
    uint8_t *mapped_frame = NULL;
    if( !pic || !h || !h->fh || i_frame < 0 || i_frame == INT_MAX )
        return -1;

    if( h->use_mmap )
    {
        if( h->frame_size <= 0 || i_frame > INT64_MAX / h->frame_size )
            return -1;
        pic->img.plane[0] = x264_cli_mmap( &h->mmap, (int64_t)i_frame * h->frame_size, h->frame_size );
        if( !pic->img.plane[0] )
            return -1;
        mapped_frame = pic->img.plane[0];
    }
    else if( i_frame > h->next_frame )
    {
        if( x264_is_regular_file( h->fh ) )
        {
            int64_t offset;
            if( h->frame_size <= 0 || i_frame > INT64_MAX / h->frame_size )
                return -1;
            offset = (int64_t)i_frame * h->frame_size;
            if( offset > LONG_MAX || fseek( h->fh, (long)offset, SEEK_SET ) )
                return -1;
        }
        else
            while( i_frame > h->next_frame )
            {
                if( read_frame_internal( pic, h, 0 ) )
                    return -1;
                h->next_frame++;
            }
    }

    if( read_frame_internal( pic, h, h->bit_depth & 7 ) )
    {
        if( h->use_mmap && mapped_frame )
        {
            x264_cli_munmap( &h->mmap, mapped_frame, h->frame_size );
            memset( pic->img.plane, 0, sizeof(pic->img.plane) );
        }
        return -1;
    }

    h->next_frame = i_frame+1;
    return 0;
}

static int release_frame( cli_pic_t *pic, hnd_t handle )
{
    raw_hnd_t *h = handle;
    if( !pic || !h )
        return -1;
    if( h->use_mmap )
    {
        if( !pic->img.plane[0] )
            return -1;
        return x264_cli_munmap( &h->mmap, pic->img.plane[0], h->frame_size );
    }
    return 0;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    raw_hnd_t *h = handle;
    if( !pic || !h )
        return -1;
    return (h->use_mmap ? x264_cli_pic_init_noalloc : x264_cli_pic_alloc)( pic, csp, width, height );
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    raw_hnd_t *h = handle;
    if( !pic || !h )
        return;
    if( h->use_mmap )
        memset( pic, 0, sizeof(cli_pic_t) );
    else
        x264_cli_pic_clean( pic );
}

static int close_file( hnd_t handle )
{
    raw_hnd_t *h = handle;
    int ret = 0;
    if( !h || !h->fh )
        return 0;
    if( h->use_mmap )
        x264_cli_mmap_close( &h->mmap );
    if( h->fh != stdin )
        ret = fclose( h->fh );
    free( h );
    return ret;
}

const cli_input_t raw_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
