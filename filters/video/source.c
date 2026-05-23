/*****************************************************************************
 * source.c: source video filter
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

#include "video.h"
#include <limits.h>

/* This filter converts the demuxer API into the filtering API for video frames.
 * Backseeking is prohibited here as not all demuxers are capable of doing so. */

typedef struct
{
    cli_pic_t pic;
    hnd_t hin;
    int cur_frame;
    int have_frame;
} source_hnd_t;

cli_vid_filter_t source_filter;

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
    if( !handle || !*handle || !filter || !info ||
        !cli_input.picture_alloc || !cli_input.read_frame ||
        info->width <= 0 || info->height <= 0 ||
        info->width > MAX_RESOLUTION || info->height > MAX_RESOLUTION )
        return -1;

    source_hnd_t *h = calloc( 1, sizeof(source_hnd_t) );
    if( !h )
        return -1;
    h->cur_frame = -1;

    if( cli_input.picture_alloc( &h->pic, *handle, info->csp, info->width, info->height ) )
    {
        free( h );
        return -1;
    }

    h->hin = *handle;
    h->have_frame = 0;
    *handle = h;
    *filter = source_filter;

    return 0;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
    source_hnd_t *h = handle;
    /* do not allow requesting of frames from before the current position */
    if( !h || !h->hin || !output || !cli_input.read_frame ||
        frame < 0 || frame <= h->cur_frame ||
        cli_input.read_frame( &h->pic, h->hin, frame ) )
        return -1;
    h->cur_frame = frame;
    h->have_frame = 1;
    *output = h->pic;
    return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    source_hnd_t *h = handle;
    if( !h || !h->hin || !pic )
        return -1;
    if( !h->have_frame )
        return 0;
    if( cli_input.release_frame && cli_input.release_frame( &h->pic, h->hin ) )
        return -1;
    h->have_frame = 0;
    return 0;
}

static void free_filter( hnd_t handle )
{
    source_hnd_t *h = handle;
    if( !h )
        return;
    if( h->have_frame && cli_input.release_frame )
        cli_input.release_frame( &h->pic, h->hin );
    if( h->hin && cli_input.picture_clean )
        cli_input.picture_clean( &h->pic, h->hin );
    if( h->hin && cli_input.close_file )
        cli_input.close_file( h->hin );
    free( h );
}

cli_vid_filter_t source_filter = { "source", NULL, init, get_frame, release_frame, free_filter, NULL };
