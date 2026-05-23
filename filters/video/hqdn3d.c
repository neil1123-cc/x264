/*****************************************************************************
 * hqdn3d.c: x264 hqdn3d filter
 *****************************************************************************
 * Copyright (C) 2003 Daniel Moreno <comac@comac.darktech.org>
 * Avisynth port (C) 2005 Loren Merritt <lorenm@u.washington.edu>
 * x264 port (C) 2013 James Darnley <james.darnley@gmail.com>
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

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "internal.h"
#include "video.h"

#define NAME "hqdn3d"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )
#define ABS(A) ( (A) > 0 ? (A) : -(A) )
#define PARAM1_DEFAULT 4.0
#define PARAM2_DEFAULT 3.0
#define PARAM3_DEFAULT 6.0

cli_vid_filter_t hqdn3d_filter;

typedef struct {
    hnd_t prev_hnd;
    cli_vid_filter_t prev_filter;
    cli_pic_t buffer;
    int coefs[4][512*16];
    unsigned int *line;
    unsigned short *frame[3];
    int first_frame;
    const x264_cli_csp_t *csp;
} hqdn3d_hnd_t;

static void help(int longhelp)
{
    printf("      "NAME":ls,cs,lt,ct\n");
    if(!longhelp)
        return;
    printf(
"            Denoises the image using mplayer's hqdn3d filter\n"
"            The four arguments are floats and are optional\n"
"            If any options are omitted, they will assume a\n"
"            value based on previous options that you did specify\n"
"            - ls = luma spatial filter strength [%.1lf]\n"
"            - cs = chroma spatial filter strength [%.1lf]\n"
"            - lt = luma temporal filter strength [%.1lf]\n"
"            - ct = chroma temporal filter strength [%.1lf]\n",
    PARAM1_DEFAULT, PARAM2_DEFAULT, PARAM3_DEFAULT,
    PARAM3_DEFAULT * PARAM2_DEFAULT / PARAM1_DEFAULT);
}

static int precalc_coefs(int *ct, double dist25)
{
    if( !ct || !isfinite( dist25 ) || dist25 < 0.0 )
        return -1;
    double log_base = 1.0 - dist25/255.0 - 0.00001;
    if( log_base <= 0.0 )
        return -1;
    double c, simil, gamma_d = log(0.25) / log( log_base );
    if( !isfinite( gamma_d ) )
        return -1;
    for(int i = -256*16; i < 256*16; i++) {
        simil = 1.0 - ABS(i) / (16*255.0);
        if( simil < 0.0 )
            simil = 0.0;
        c = pow(simil, gamma_d) * 65536.0 * (double)i / 16.0;
        if( !isfinite( c ) || c < (double)INT_MIN || c > (double)INT_MAX )
            return -1;
        ct[16*256+i] = (int)((c<0) ? (c-0.5) : (c+0.5));
    }
    ct[0] = (dist25 != 0);
    return 0;
}

static int parse_strength( const char *arg, char **end, double *strength )
{
    if( !arg || !( (*arg >= '0' && *arg <= '9') || *arg == '.' ) )
        return -1;

    errno = 0;
    *strength = strtod( arg, end );
    if( *end == arg || errno == ERANGE || !isfinite( *strength ) || *strength < 0.0 )
        return -1;
    return 0;
}

static int parse_strengths( const char *opt_string, double *lum_spac, double *chrom_spac,
                            double *lum_tmp, double *chrom_tmp )
{
    double strength[4];
    double parsed_lum_spac;
    double parsed_chrom_spac;
    double parsed_lum_tmp;
    double parsed_chrom_tmp;
    int count = 0;
    const char *arg = opt_string;
    char *end;

    if( !arg || !*arg )
        goto defaults;

    while( count < 4 )
    {
        if( parse_strength( arg, &end, &strength[count] ) )
            return -1;
        count++;
        if( !*end || (*end == ',' && !end[1]) )
            break;
        if( *end != ',' )
            return -1;
        arg = end + 1;
    }
    if( count == 4 && *end )
        return -1;

    switch( count )
    {
        case 1:
            parsed_lum_spac = strength[0];
            if( parsed_lum_spac )
            {
                parsed_lum_tmp = PARAM3_DEFAULT * parsed_lum_spac / PARAM1_DEFAULT;
                parsed_chrom_spac = PARAM2_DEFAULT * parsed_lum_spac / PARAM1_DEFAULT;
                parsed_chrom_tmp = parsed_lum_tmp * parsed_chrom_spac / parsed_lum_spac;
            }
            else
                parsed_lum_tmp = parsed_chrom_spac = parsed_chrom_tmp = 0;
            break;
        case 2:
            parsed_lum_spac = strength[0];
            parsed_chrom_spac = strength[1];
            parsed_lum_tmp = PARAM3_DEFAULT * parsed_lum_spac / PARAM1_DEFAULT;
            if( !parsed_lum_spac )
            {
                if( parsed_chrom_spac )
                    return -1;
                parsed_chrom_tmp = 0;
            }
            else
                parsed_chrom_tmp = parsed_lum_tmp * parsed_chrom_spac / parsed_lum_spac;
            break;
        case 3:
            parsed_lum_spac = strength[0];
            parsed_chrom_spac = strength[1];
            parsed_lum_tmp = strength[2];
            if( !parsed_lum_spac )
            {
                if( parsed_chrom_spac || parsed_lum_tmp )
                    return -1;
                parsed_chrom_tmp = 0;
            }
            else
                parsed_chrom_tmp = parsed_lum_tmp * parsed_chrom_spac / parsed_lum_spac;
            break;
        case 4:
            parsed_lum_spac = strength[0];
            parsed_chrom_spac = strength[1];
            parsed_lum_tmp = strength[2];
            parsed_chrom_tmp = strength[3];
            break;
        default:
defaults:
            parsed_lum_spac = PARAM1_DEFAULT;
            parsed_lum_tmp = PARAM3_DEFAULT;
            parsed_chrom_spac = PARAM2_DEFAULT;
            parsed_chrom_tmp = parsed_lum_tmp * parsed_chrom_spac / parsed_lum_spac;
            break;
    }

    if( !isfinite( parsed_lum_spac ) || !isfinite( parsed_chrom_spac ) ||
        !isfinite( parsed_lum_tmp ) || !isfinite( parsed_chrom_tmp ) )
        return -1;

    *lum_spac = parsed_lum_spac;
    *chrom_spac = parsed_chrom_spac;
    *lum_tmp = parsed_lum_tmp;
    *chrom_tmp = parsed_chrom_tmp;
    return 0;
}

static int init(hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info,
                x264_param_t *param, char *opt_string)
{
    double lum_spac, lum_tmp, chrom_spac, chrom_tmp;

    if( !handle || !*handle || !filter || !filter->get_frame || !filter->release_frame || !filter->free )
        return -1;
    FAIL_IF_ERROR( !info || info->width <= 0 || info->height <= 0 ||
        info->width > MAX_RESOLUTION || info->height > MAX_RESOLUTION,
        "invalid input resolution %dx%d\n", info ? info->width : 0, info ? info->height : 0 );
    const x264_cli_csp_t *csp = x264_cli_get_csp(info->csp);
    FAIL_IF_ERROR( !csp, "invalid colorspace\n" );
    FAIL_IF_ERROR(!(info->csp == X264_CSP_I400 || info->csp == X264_CSP_I420 || info->csp == X264_CSP_I422
        || info->csp == X264_CSP_I444 || info->csp == X264_CSP_YV12),
        "Only planar YUV images supported\n");

    if( parse_strengths( opt_string, &lum_spac, &chrom_spac, &lum_tmp, &chrom_tmp ) )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "invalid options `%s'\n", opt_string ? opt_string : "" );
        return -1;
    }

    hqdn3d_hnd_t *h = calloc(1, sizeof(hqdn3d_hnd_t));
    if(!h)
        return -1;

    if( (uint64_t)info->width > SIZE_MAX / sizeof(*h->line) )
        goto fail;
    h->line = calloc(1, (size_t)info->width * sizeof(*h->line));
    if(!h->line)
        goto fail;
    if( x264_cli_pic_alloc( &h->buffer, info->csp, info->width, info->height ) )
        goto fail;
    for(int i = 0; i < csp->planes; i++)
    {
        int64_t plane_size = x264_cli_pic_plane_size( info->csp, info->width, info->height, i );
        if( plane_size <= 0 || (uint64_t)plane_size > SIZE_MAX / sizeof(*h->frame[i]) )
            goto fail;
        h->frame[i] = malloc( (size_t)plane_size * sizeof(*h->frame[i]) );
        if( !h->frame[i] )
            goto fail;
    }

    if( precalc_coefs(h->coefs[0], lum_spac) ||
        precalc_coefs(h->coefs[1], lum_tmp) ||
        precalc_coefs(h->coefs[2], chrom_spac) ||
        precalc_coefs(h->coefs[3], chrom_tmp) )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "filter strength is out of range\n" );
        goto fail;
    }

    x264_cli_log(NAME, X264_LOG_INFO,
        "using strengths %.1lf,%.1lf,%.1lf,%.1lf\n",
        lum_spac, chrom_spac, lum_tmp, chrom_tmp);

    h->csp = csp;
    h->first_frame = 1;
    h->prev_filter = *filter;
    h->prev_hnd = *handle;
    *handle = h;
    *filter = hqdn3d_filter;
    return 0;

fail:
    free(h->line);
    x264_cli_pic_clean( &h->buffer );
    for(int i = 0; i < 3; i++)
        free(h->frame[i]);
    free(h);
    return -1;
}

static inline unsigned int lpm(unsigned int prev_mul,
                               unsigned int curr_mul, int* coef)
{
    int d_mul = prev_mul-curr_mul;
    unsigned int d = ((d_mul+0x10007FF)/(65536/16));
    return curr_mul + coef[d];
}

static void denoise_temporal(unsigned char *frame, unsigned short *frame_ant,
                             int w, int h, int stride, int *temporal)
{
    intptr_t x, y;
    unsigned int pixel_dst;
    for(y = 0; y < h; y++) {
        for(x = 0; x < w; x++) {
            pixel_dst = lpm( (unsigned)frame_ant[x] << 8, (unsigned)frame[x] << 16, temporal );
            frame_ant[x] = ((pixel_dst+0x1000007F)>>8);
            frame[x] = ((pixel_dst+0x10007FFF)>>16);
        }
        frame += stride;
        frame_ant += w;
    }
}

static void denoise_spacial(unsigned char *frame, unsigned int *line_ant,
                            int w, int h, int stride, int *spacial)
{
    intptr_t x, y, line_offs = 0;
    unsigned int pixel_ant, pixel_dst;

    /* First pixel has no left nor top neighbor. */
    pixel_dst = line_ant[0] = pixel_ant = (unsigned)frame[0] << 16;
    frame[0] = ((pixel_dst+0x10007FFF)>>16);

    /* First line has no top neighbor, only left. */
    for(x = 1; x < w; x++) {
        pixel_dst = line_ant[x] = lpm( pixel_ant, (unsigned)frame[x] << 16, spacial );
        frame[x] = ((pixel_dst+0x10007FFF)>>16);
    }

    for(y = 1; y < h; y++) {
        line_offs += stride;
        /* First pixel on each line doesn't have previous pixel */
        pixel_ant = (unsigned)frame[line_offs] << 16;
        pixel_dst = line_ant[0] = lpm(line_ant[0], pixel_ant, spacial);
        frame[line_offs] = ((pixel_dst+0x10007FFF)>>16);

        for(x = 1; x < w; x++) {
            /* The rest are normal */
            pixel_ant = lpm( pixel_ant, (unsigned)frame[line_offs+x] << 16, spacial );
            pixel_dst = line_ant[x] = lpm(line_ant[x], pixel_ant, spacial);
            frame[line_offs+x] = ((pixel_dst+0x10007FFF)>>16);
        }
    }
}

static void denoise(unsigned char *frame, unsigned int *line_ant,
                    unsigned short *frame_ant, int w, int h, int stride,
                    int *spacial, int *temporal)
{
    intptr_t x, y, line_offs = 0;
    unsigned int pixel_ant,pixel_dst;

    if(!spacial[0]) {
        denoise_temporal(frame, frame_ant, w, h, stride, temporal);
        return;
    }
    if(!temporal[0]) {
        denoise_spacial(frame, line_ant, w, h, stride, spacial);
        return;
    }

    /* First pixel has no left nor top neightbour. Only previous frame */
    line_ant[0] = pixel_ant = (unsigned)frame[0] << 16;
    pixel_dst = lpm( (unsigned)frame_ant[0] << 8, pixel_ant, temporal );
    frame_ant[0] = (pixel_dst + 0x1000007Fu) >> 8;
    frame[0] = (pixel_dst + 0x10007FFFu) >> 16;

    /* Fist line has no top neightbour. Only left one for each pixel and
     * last frame */
    for (x = 1; x < w; x++){
        line_ant[x] = pixel_ant = lpm( pixel_ant, (unsigned)frame[x] << 16, spacial );
        pixel_dst = lpm( (unsigned)frame_ant[x] << 8, pixel_ant, temporal );
        frame_ant[x] = (pixel_dst + 0x1000007Fu) >> 8;
        frame[x] = (pixel_dst + 0x10007FFFu) >> 16;
    }

    for (y = 1; y < h; y++){
        unsigned short* line_prev = &frame_ant[y*w];
        line_offs += stride;
        /* First pixel on each line doesn't have previous pixel */
        pixel_ant = (unsigned)frame[line_offs] << 16;
        line_ant[0] = lpm(line_ant[0], pixel_ant, spacial);
        pixel_dst = lpm( (unsigned)line_prev[0] << 8, line_ant[0], temporal );
        line_prev[0] = (pixel_dst + 0x1000007Fu) >> 8;
        frame[line_offs] = (pixel_dst + 0x10007FFFu) >> 16;

        for (x = 1; x < w; x++){
            /* The rest are normal */
            pixel_ant = lpm( pixel_ant, (unsigned)frame[line_offs+x] << 16, spacial );
            line_ant[x] = lpm( line_ant[x], pixel_ant, spacial );
            pixel_dst = lpm( (unsigned)line_prev[x] << 8, line_ant[x], temporal );
            line_prev[x] = (pixel_dst + 0x1000007Fu) >> 8;
            frame[line_offs+x] = (pixel_dst + 0x10007FFFu) >> 16;
        }
    }
}

static void init_data(uint8_t *source, unsigned short *dest,
                      int width, int height, int stride)
{
    for(int y = 0; y < height; y++)
        for(int x = 0; x < width; x++)
            dest[y*width+x] = (unsigned)source[y*stride+x] << 8;
}

static int hqdn3d_abs_stride( int stride )
{
    if( stride == INT_MIN )
        return -1;
    return stride < 0 ? -stride : stride;
}

static int hqdn3d_plane_dims( cli_image_t *img, const x264_cli_csp_t *csp, int plane,
                              int *width, int *height )
{
    int64_t plane_width;
    int64_t plane_height;

    if( !img || !csp || !width || !height || plane < 0 || plane >= img->planes )
        return -1;

    plane_width = (int64_t)img->width * csp->width[plane];
    plane_height = (int64_t)img->height * csp->height[plane];
    if( plane_width <= 0 || plane_height <= 0 ||
        plane_width > INT_MAX || plane_height > INT_MAX )
        return -1;

    int plane_width_int = (int)plane_width;
    int plane_height_int = (int)plane_height;
    *width = plane_width_int;
    *height = plane_height_int;
    return 0;
}

static int get_frame(hnd_t handle, cli_pic_t *out, int frame)
{
    hqdn3d_hnd_t *h = handle;
    cli_pic_t in;
    int ret;

    if( !h || !out || !h->csp || !h->line || !h->prev_hnd ||
        !h->prev_filter.get_frame || !h->prev_filter.release_frame ||
        frame < 0 || h->prev_filter.get_frame(h->prev_hnd, &in, frame) )
        return -1;

    ret = x264_cli_pic_copy( &h->buffer, &in );
    ret |= h->prev_filter.release_frame(h->prev_hnd, &in, frame);
    if( ret )
        return -1;

    if( h->buffer.img.planes != h->csp->planes || h->buffer.img.planes < 0 ||
        h->buffer.img.planes > 3 )
        return -1;

    for(int i = 0; i < h->buffer.img.planes; i++)
    {
        int width, height;
        int stride = hqdn3d_abs_stride( h->buffer.img.stride[i] );
        if( hqdn3d_plane_dims( &h->buffer.img, h->csp, i, &width, &height ) ||
            stride < width || !h->frame[i] || !h->buffer.img.plane[i] )
            return -1;
    }

    *out = h->buffer;
    for(int i = 0; i < out->img.planes; i++) {
        int width, height;
        int stride = out->img.stride[i];
        if( hqdn3d_plane_dims( &out->img, h->csp, i, &width, &height ) )
            return -1;
        if(h->first_frame)
            init_data(out->img.plane[i], h->frame[i], width, height, stride);
        denoise(out->img.plane[i], h->line, h->frame[i], width, height, stride,
            h->coefs[(i+1)&2], h->coefs[((i+1)&2)+1]);
    }

    h->first_frame = 0;
    return 0;
}

static int release_frame(hnd_t handle, cli_pic_t *pic, int frame)
{
    if( !handle || !pic )
        return -1;
    return 0;
}

static void free_filter(hnd_t handle)
{
    hqdn3d_hnd_t *h = handle;
    if( !h )
        return;
    if( h->prev_hnd && h->prev_filter.free )
        h->prev_filter.free(h->prev_hnd);
    x264_cli_pic_clean( &h->buffer );
    free(h->line);
    for(int i = 0; i < 3; i++)
        free(h->frame[i]);
    free(h);
}

cli_vid_filter_t hqdn3d_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
