/*****************************************************************************
 * y4m.c: y4m input
 *****************************************************************************
 * Copyright (C) 2003-2025 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
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
#include <errno.h>
#include <limits.h>

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "y4m", __VA_ARGS__ )
#define FAIL_IF_ERROR_CLEANUP( cond, ... )\
do\
{\
    if( cond )\
    {\
        x264_cli_log( "y4m", X264_LOG_ERROR, __VA_ARGS__ );\
        goto fail;\
    }\
} while( 0 )

typedef struct
{
    FILE *fh;
    int next_frame;
    int seq_header_len;
    int frame_header_len;
    int64_t frame_size;
    int64_t plane_size[3];
    int bit_depth;
    cli_mmap_t mmap;
    int use_mmap;
	int x264_bit_depth;
} y4m_hnd_t;

#define Y4M_MAGIC "YUV4MPEG2"
#define Y4M_FRAME_MAGIC "FRAME"
#define Y4M_MAX_HEADER 256

static int parse_uint32_token( const char *tokstart, char **tokend, uint32_t *dst )
{
    unsigned long value;

    if( *tokstart < '0' || *tokstart > '9' )
        return -1;

    errno = 0;
    value = strtoul( tokstart, tokend, 10 );
    if( errno == ERANGE || value > UINT32_MAX )
        return -1;

    *dst = (uint32_t)value;
    return 0;
}

static int y4m_is_token_end( int c )
{
    return c == 0x20 || c == '\n';
}

static int parse_ratio_token( const char *tokstart, uint32_t *num, uint32_t *den )
{
    char *tokend;
    uint32_t parsed_num;
    uint32_t parsed_den;

    if( parse_uint32_token( tokstart, &tokend, &parsed_num ) || *tokend++ != ':' ||
        parse_uint32_token( tokend, &tokend, &parsed_den ) || !y4m_is_token_end( *tokend ) )
        return -1;

    if( !parsed_num && !parsed_den )
    {
        *num = parsed_num;
        *den = parsed_den;
        return 1;
    }
    if( !parsed_num || !parsed_den )
        return -1;

    *num = parsed_num;
    *den = parsed_den;
    return 0;
}

static int parse_depth_suffix( const char *tokstart, int *bit_depth )
{
    char *tokend;
    uint32_t depth;

    if( parse_uint32_token( tokstart, &tokend, &depth ) || !y4m_is_token_end( *tokend ) || depth > INT_MAX )
        return -1;

    *bit_depth = (int)depth;
    return 0;
}

static int y4m_suffix_is( const char *suffix, const char *name )
{
    size_t len = strlen( name );
    return !strncmp( suffix, name, len ) && y4m_is_token_end( suffix[len] );
}

static int parse_csp_and_depth( char *csp_name, int *bit_depth )
{
    int csp = X264_CSP_MAX;
    char *suffix;

    /* Set colorspace from known variants */
    if( !strncmp( "mono", csp_name, 4 ) )
    {
        csp = X264_CSP_I400;
        suffix = csp_name + 4;
        if( !y4m_is_token_end( *suffix ) && parse_depth_suffix( suffix, bit_depth ) )
            return X264_CSP_MAX;
        if( y4m_is_token_end( *suffix ) )
            *bit_depth = 8;
        return csp;
    }
    else if( !strncmp( "420", csp_name, 3 ) )
        csp = X264_CSP_I420;
    else if( !strncmp( "422", csp_name, 3 ) )
        csp = X264_CSP_I422;
    else if( !strncmp( "444", csp_name, 3 ) && strncmp( "444alpha", csp_name, 8 ) ) // only accept alphaless 4:4:4
        csp = X264_CSP_I444;

    if( csp == X264_CSP_MAX )
        return csp;

    suffix = csp_name + 3;
    if( y4m_is_token_end( *suffix ) )
        *bit_depth = 8;
    else if( (*suffix == 'p' || *suffix == 'P') && suffix[1] >= '0' && suffix[1] <= '9' )
    {
        if( parse_depth_suffix( suffix + 1, bit_depth ) )
            return X264_CSP_MAX;
    }
    else if( csp == X264_CSP_I420 && (y4m_suffix_is( suffix, "jpeg" ) ||
                                      y4m_suffix_is( suffix, "mpeg2" ) ||
                                      y4m_suffix_is( suffix, "paldv" )) )
        *bit_depth = 8;
    else
        return X264_CSP_MAX;

    return csp;
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    if( !psz_filename || !p_handle || !info || !opt )
        return -1;
    *p_handle = NULL;

    y4m_hnd_t *h = calloc( 1, sizeof(y4m_hnd_t) );
    int i;
    uint32_t n, d;
    char header[Y4M_MAX_HEADER+10];
    char *tokend, *header_end;
    int colorspace = X264_CSP_NONE;
    int alt_colorspace = X264_CSP_NONE;
    int alt_bit_depth  = 8;
    if( !h )
        return -1;

    video_info_t updated_info = *info;
    updated_info.vfr = 0;
    updated_info.num_frames = 0;  /* may be overwritten by XLENGTH extension */

    if( !strcmp( psz_filename, "-" ) )
        h->fh = stdin;
    else
        h->fh = x264_fopen(psz_filename, "rb");
    if( h->fh == NULL )
        goto fail;

    /* Read header */
    for( i = 0; i < Y4M_MAX_HEADER; i++ )
    {
        int c = fgetc( h->fh );
        FAIL_IF_ERROR_CLEANUP( c == EOF, "bad sequence header length\n" );
        header[i] = c;
        if( header[i] == '\n' )
        {
            /* Add a space after last option. Makes parsing "444" vs
               "444alpha" easier. */
            header[i+1] = 0x20;
            header[i+2] = 0;
            break;
        }
    }
    FAIL_IF_ERROR_CLEANUP( strncmp( header, Y4M_MAGIC, sizeof(Y4M_MAGIC)-1 ), "bad sequence header magic\n" );
    FAIL_IF_ERROR_CLEANUP( i == Y4M_MAX_HEADER, "bad sequence header length\n" );

    /* Scan properties */
    header_end = &header[i+1]; /* Include space */
    h->seq_header_len = i+1;
	h->x264_bit_depth = opt->x264_bit_depth;
    for( char *tokstart = header + sizeof(Y4M_MAGIC); tokstart < header_end; tokstart++ )
    {
        if( *tokstart == 0x20 )
            continue;
        switch( *tokstart++ )
        {
            case 'W': /* Width. Required. */
            {
                uint32_t width;
                FAIL_IF_ERROR_CLEANUP( parse_uint32_token( tokstart, &tokend, &width ) || !y4m_is_token_end( *tokend ) ||
                                       !width || width > MAX_RESOLUTION, "invalid width `%s'\n", tokstart );
                updated_info.width = (int)width;
                tokstart=tokend;
                break;
            }
            case 'H': /* Height. Required. */
            {
                uint32_t height;
                FAIL_IF_ERROR_CLEANUP( parse_uint32_token( tokstart, &tokend, &height ) || !y4m_is_token_end( *tokend ) ||
                                       !height || height > MAX_RESOLUTION, "invalid height `%s'\n", tokstart );
                updated_info.height = (int)height;
                tokstart=tokend;
                break;
            }
            case 'C': /* Color space */
                colorspace = parse_csp_and_depth( tokstart, &h->bit_depth );
                tokstart = strchr( tokstart, 0x20 );
                break;
            case 'I': /* Interlace type */
            {
                int interlace_type = *tokstart++;
                FAIL_IF_ERROR_CLEANUP( y4m_is_token_end( interlace_type ) || !y4m_is_token_end( *tokstart ),
                                       "invalid interlace type `%s'\n", tokstart - 1 );
                switch( interlace_type )
                {
                    case 't':
                        updated_info.interlaced = 1;
                        updated_info.tff = 1;
                        break;
                    case 'b':
                        updated_info.interlaced = 1;
                        updated_info.tff = 0;
                        break;
                    case 'm':
                        updated_info.interlaced = 1;
                        break;
                    //case '?':
                    //case 'p':
                    default:
                        break;
                }
                tokstart = strchr( tokstart, 0x20 );
                break;
            }
            case 'F': /* Frame rate - 0:0 if unknown */
            {
                int ratio = parse_ratio_token( tokstart, &n, &d );
                FAIL_IF_ERROR_CLEANUP( ratio < 0, "invalid frame rate `%s'\n", tokstart );
                if( !ratio )
                {
                    if( !opt->b_accurate_fps )
                        x264_ntsc_fps( &n, &d );
                    x264_reduce_fraction( &n, &d );
                    updated_info.fps_num = n;
                    updated_info.fps_den = d;
                }
                tokstart = strchr( tokstart, 0x20 );
                break;
            }
            case 'A': /* Pixel aspect - 0:0 if unknown */
            {
                /* Don't override the aspect ratio if sar has been explicitly set on the commandline. */
                int ratio = parse_ratio_token( tokstart, &n, &d );
                FAIL_IF_ERROR_CLEANUP( ratio < 0, "invalid pixel aspect `%s'\n", tokstart );
                if( !ratio )
                {
                    x264_reduce_fraction( &n, &d );
                    updated_info.sar_width  = n;
                    updated_info.sar_height = d;
                }
                tokstart = strchr( tokstart, 0x20 );
                break;
            }
            case 'X': /* Vendor extensions */
                if( !strncmp( "YSCSS=", tokstart, 6 ) )
                {
                    /* Older nonstandard pixel format representation */
                    tokstart += 6;
                    alt_colorspace = parse_csp_and_depth( tokstart, &alt_bit_depth );
                }
                else if( !strncmp( "COLORRANGE=", tokstart, 11 ) )
                {
                    /* ffmpeg's color range extension */
                    tokstart += 11;
                    if( !strncmp( "FULL", tokstart, 4 ) && y4m_is_token_end( tokstart[4] ) )
                        updated_info.fullrange = 1;
                    else if( !strncmp( "LIMITED", tokstart, 7 ) && y4m_is_token_end( tokstart[7] ) )
                        updated_info.fullrange = 0;
                    else
                        FAIL_IF_ERROR_CLEANUP( 1, "invalid color range `%s'\n", tokstart );
                }
                else if( !strncmp( "LENGTH=", tokstart, 7 ) )
                {
                    /* x265 extension: total frame count for ETA */
                    uint32_t num_frames;
                    tokstart += 7;
                    FAIL_IF_ERROR_CLEANUP( parse_uint32_token( tokstart, &tokend, &num_frames ) || !y4m_is_token_end( *tokend ) ||
                                           num_frames > INT_MAX, "invalid frame count `%s'\n", tokstart );
                    updated_info.num_frames = (int)num_frames;
                    tokstart = tokend;
                }
                tokstart = strchr( tokstart, 0x20 );
                break;
        }
    }

    if( colorspace == X264_CSP_NONE )
    {
        colorspace   = alt_colorspace;
        h->bit_depth = alt_bit_depth;
    }

    // default to 8bit 4:2:0 if nothing is specified
    if( colorspace == X264_CSP_NONE )
    {
        colorspace    = X264_CSP_I420;
        h->bit_depth  = 8;
    }

    FAIL_IF_ERROR_CLEANUP( colorspace <= X264_CSP_NONE || colorspace >= X264_CSP_MAX, "colorspace unhandled\n" );
    FAIL_IF_ERROR_CLEANUP( h->bit_depth < 8 || h->bit_depth > 16, "unsupported bit depth `%d'\n", h->bit_depth );

    updated_info.thread_safe = 1;
    updated_info.csp         = colorspace;

    if( h->bit_depth > 8 )
	{
        updated_info.csp |= X264_CSP_HIGH_DEPTH;
        if( h->bit_depth == opt->x264_bit_depth )
        {
            /* HACK: totally skips depth filter to prevent dither error */
            updated_info.csp |= X264_CSP_SKIP_DEPTH_FILTER;
        }
	}

    const x264_cli_csp_t *csp = x264_cli_get_csp( updated_info.csp );
    FAIL_IF_ERROR_CLEANUP( !csp, "colorspace unhandled\n" );

    int pixel_depth = x264_cli_csp_depth_factor( updated_info.csp );
    FAIL_IF_ERROR_CLEANUP( pixel_depth <= 0, "invalid pixel depth\n" );
    for( i = 0; i < csp->planes; i++ )
    {
        int64_t plane_size = x264_cli_pic_plane_size( updated_info.csp, updated_info.width, updated_info.height, i );
        FAIL_IF_ERROR_CLEANUP( plane_size <= 0 || h->frame_size > INT64_MAX - plane_size, "invalid frame size\n" );
        h->frame_size += plane_size;
        /* x264_cli_pic_plane_size returns the size in bytes, we need the value in pixels from here on */
        h->plane_size[i] = plane_size / pixel_depth;
    }

    if( x264_is_regular_file( h->fh ) )
    {
        int64_t init_pos = ftell( h->fh );
        FAIL_IF_ERROR_CLEANUP( init_pos < 0, "unable to determine input file position\n" );

        /* Find out the length of the frame header */
        size_t len = 1;
        int c;
        while( len <= Y4M_MAX_HEADER && (c = fgetc( h->fh )) != '\n' && c != EOF )
            len++;
        FAIL_IF_ERROR_CLEANUP( len > Y4M_MAX_HEADER || len < sizeof(Y4M_FRAME_MAGIC) || c == EOF, "bad frame header length\n" );
        h->frame_header_len = (int)len;
        FAIL_IF_ERROR_CLEANUP( h->frame_size > INT64_MAX - (int64_t)len, "invalid frame size\n" );
        h->frame_size += (int64_t)len;

        FAIL_IF_ERROR_CLEANUP( fseek( h->fh, 0, SEEK_END ), "unable to seek input file\n" );
        int64_t i_size = ftell( h->fh );
        FAIL_IF_ERROR_CLEANUP( i_size < 0, "unable to determine input file size\n" );
        FAIL_IF_ERROR_CLEANUP( fseek( h->fh, init_pos, SEEK_SET ), "unable to seek input file\n" );
        int64_t num_frames = (i_size - h->seq_header_len) / h->frame_size;
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
            x264_cli_log( "y4m", X264_LOG_ERROR, "failed to close input file `%s'\n", psz_filename );
    }
    free( h );
    return -1;
}

static int read_frame_internal( cli_pic_t *pic, y4m_hnd_t *h, int bit_depth_uc )
{
    static const size_t slen = sizeof(Y4M_FRAME_MAGIC)-1;
    if( !pic || !h || (!h->use_mmap && !h->fh) || pic->img.planes <= 0 || pic->img.planes > 3 )
        return -1;
    if( h->use_mmap && (!pic->img.plane[0] || h->frame_header_len <= 0) )
        return -1;
    int pixel_depth = x264_cli_csp_depth_factor( pic->img.csp );
    if( pixel_depth <= 0 )
        return -1;
    int i = (int)sizeof(Y4M_FRAME_MAGIC);
    char header_buf[16];
    char *header;

    /* Verify that the frame header is valid */
    if( h->use_mmap )
    {
        header = (char*)pic->img.plane[0];
        pic->img.plane[0] += h->frame_header_len;

        /* If the header length has changed between frames the size of the mapping will be invalid.
         * It might be possible to work around it, but I'm not aware of any tool beside fuzzers that
         * produces y4m files with variable-length frame headers so just error out if that happens. */
        while( i <= h->frame_header_len && header[i-1] != '\n' )
            i++;
        FAIL_IF_ERROR( i != h->frame_header_len, "bad frame header length\n" );
    }
    else
    {
        header = header_buf;
        if( fread( header, 1, slen, h->fh ) != slen )
            return -1;
        int c;
        while( i <= Y4M_MAX_HEADER && (c = fgetc( h->fh )) != '\n' && c != EOF )
            i++;
        FAIL_IF_ERROR( i > Y4M_MAX_HEADER || c == EOF, "bad frame header length\n" );
    }
    FAIL_IF_ERROR( memcmp( header, Y4M_FRAME_MAGIC, slen ), "bad frame header magic\n" );

    for( i = 0; i < pic->img.planes; i++ )
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
    y4m_hnd_t *h = handle;
    uint8_t *mapped_frame = NULL;
    if( !pic || !h || !h->fh || i_frame < 0 || i_frame == INT_MAX )
        return -1;

    if( h->use_mmap )
    {
        int64_t offset;
        if( h->frame_size <= 0 || h->seq_header_len < 0 ||
            i_frame > INT64_MAX / h->frame_size )
            return -1;
        offset = h->frame_size * i_frame;
        if( offset > INT64_MAX - h->seq_header_len )
            return -1;
        pic->img.plane[0] = x264_cli_mmap( &h->mmap, offset + h->seq_header_len, h->frame_size );
        if( !pic->img.plane[0] )
            return -1;
        mapped_frame = pic->img.plane[0];
    }
    else if( i_frame > h->next_frame )
    {
        if( x264_is_regular_file( h->fh ) )
        {
            int64_t offset;
            if( h->frame_size <= 0 || h->seq_header_len < 0 ||
                i_frame > INT64_MAX / h->frame_size )
                return -1;
            offset = h->frame_size * i_frame;
            if( offset > INT64_MAX - h->seq_header_len )
                return -1;
            offset += h->seq_header_len;
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
    y4m_hnd_t *h = handle;
    if( !pic || !h )
        return -1;
    if( h->use_mmap )
    {
        if( !pic->img.plane[0] )
            return -1;
        return x264_cli_munmap( &h->mmap, pic->img.plane[0] - h->frame_header_len, h->frame_size );
    }
    return 0;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    y4m_hnd_t *h = handle;
    if( !pic || !h )
        return -1;
    return (h->use_mmap ? x264_cli_pic_init_noalloc : x264_cli_pic_alloc)( pic, csp, width, height );
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    y4m_hnd_t *h = handle;
    if( !pic || !h )
        return;
    if( h->use_mmap )
        memset( pic, 0, sizeof(cli_pic_t) );
    else
        x264_cli_pic_clean( pic );
}

static int close_file( hnd_t handle )
{
    y4m_hnd_t *h = handle;
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

const cli_input_t y4m_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
