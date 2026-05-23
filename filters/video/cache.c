/*****************************************************************************
 * cache.c: cache video filter
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
#include "internal.h"
#include "common/common.h"
#include <limits.h>
#include <stdint.h>

#define cache_filter x264_glue3(cache, BIT_DEPTH, filter)
#if BIT_DEPTH == 8
#define NAME "cache_8"
#else
#define NAME "cache_10"
#endif

typedef struct
{
    hnd_t prev_hnd;
    cli_vid_filter_t prev_filter;

    int max_size;
    int first_frame; /* first cached frame */
    cli_pic_t **cache;
    cli_pic_t *scratch;
    int cur_size;
    int eof;         /* frame beyond end of the file */
} cache_hnd_t;

cli_vid_filter_t cache_filter;

static int64_t cache_last_frame( cache_hnd_t *h )
{
    return (int64_t)h->first_frame + h->cur_size - 1;
}

static void cache_free_partial( cache_hnd_t *h )
{
    if( !h )
        return;
    if( h->cache )
    {
        for( int i = 0; i < h->max_size; i++ )
        {
            if( h->cache[i] )
            {
                x264_cli_pic_clean( h->cache[i] );
                free( h->cache[i] );
            }
        }
        free( h->cache );
    }
    if( h->scratch )
    {
        x264_cli_pic_clean( h->scratch );
        free( h->scratch );
    }
    free( h );
}

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
    intptr_t size = (intptr_t)opt_string;
    /* upon a <= 0 cache request, do nothing */
    if( size <= 0 )
        return 0;
    if( !handle || !*handle || !filter || !info ||
        !filter->get_frame || !filter->release_frame || !filter->free )
        return -1;
    if( size > INT_MAX || (uintmax_t)size + 1 > SIZE_MAX / sizeof(cli_pic_t*) )
        return -1;
    int64_t cache_alloc_size = sizeof(cache_hnd_t);
    cache_hnd_t *h = calloc( 1, cache_alloc_size );
    if( !h )
        return -1;

    int max_size = (int)size;
    h->max_size = max_size;
    int64_t cache_slots_alloc_size = (int64_t)(((size_t)h->max_size + 1) * sizeof(cli_pic_t*));
    h->cache = calloc( 1, cache_slots_alloc_size );
    if( !h->cache )
    {
        free( h );
        return -1;
    }

    for( int i = 0; i < h->max_size; i++ )
    {
        int64_t pic_alloc_size = sizeof(cli_pic_t);
        h->cache[i] = calloc( 1, pic_alloc_size );
        if( !h->cache[i] || x264_cli_pic_alloc( h->cache[i], info->csp, info->width, info->height ) )
        {
            cache_free_partial( h );
            return -1;
        }
    }
    int64_t scratch_alloc_size = sizeof(cli_pic_t);
    h->scratch = calloc( 1, scratch_alloc_size );
    if( !h->scratch || x264_cli_pic_alloc( h->scratch, info->csp, info->width, info->height ) )
    {
        cache_free_partial( h );
        return -1;
    }

    h->prev_filter = *filter;
    h->prev_hnd = *handle;
    *handle = h;
    *filter = cache_filter;

    return 0;
}

static void fill_cache( cache_hnd_t *h, int frame )
{
    /* shift frames out of the cache as the frame request is beyond the filled cache */
    int64_t shift = (int64_t)frame - cache_last_frame( h );
    /* no frames to shift or no frames left to read */
    if( shift <= 0 || h->eof )
        return;
    if( shift > INT_MAX )
    {
        h->eof = frame;
        return;
    }
    /* the next frames to read are either
     * A) starting at the end of the current cache, or
     * B) starting at a new frame that has the end of the cache at the desired frame
     * and proceeding to fill the entire cache */
    int64_t next_frame = (int64_t)h->first_frame + h->cur_size;
    int64_t refill_start = (int64_t)frame - h->max_size + 1;
    if( next_frame > INT_MAX || refill_start > INT_MAX )
    {
        h->eof = frame;
        return;
    }
    int64_t refill_frame = X264_MAX( next_frame, refill_start );
    int refill_frame_int = (int)refill_frame;
    int cur_frame = refill_frame_int;
    /* the new starting point is either
     * A) the current one shifted the number of frames entering/leaving the cache, or
     * B) at a new frame that has the end of the cache at the desired frame. */
    int64_t shifted_first = (int64_t)h->first_frame + shift;
    if( shifted_first > INT_MAX )
    {
        h->eof = frame;
        return;
    }
    int shifted_first_int = (int)shifted_first;
    int first_frame = X264_MIN( shifted_first_int, refill_frame_int );
    int64_t remaining_size = X264_MAX( (int64_t)h->cur_size - shift, 0 );
    int cur_size = (int)remaining_size;
    h->first_frame = first_frame;
    h->cur_size = cur_size;
    while( h->cur_size < h->max_size )
    {
        cli_pic_t temp;
        if( h->prev_filter.get_frame( h->prev_hnd, &temp, cur_frame ) )
        {
            h->eof = cur_frame;
            return;
        }
        int copy_failed = x264_cli_pic_copy( h->scratch, &temp );
        int release_failed = h->prev_filter.release_frame( h->prev_hnd, &temp, cur_frame );
        if( copy_failed || release_failed )
        {
            h->eof = cur_frame;
            return;
        }
        /* the read was successful, shift the frame off the front to the end */
        XCHG( cli_pic_t*, h->scratch, h->cache[0] );
        x264_frame_push( (void*)h->cache, x264_frame_shift( (void*)h->cache ) );
        h->cur_size++;
        if( cur_frame == INT_MAX )
            return;
        cur_frame++;
    }
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
    cache_hnd_t *h = handle;
    if( !h || !output || !h->cache || !h->prev_hnd ||
        !h->prev_filter.get_frame || !h->prev_filter.release_frame )
        return -1;
    FAIL_IF_ERR( frame < h->first_frame, NAME, "frame %d is before first cached frame %d \n", frame, h->first_frame );
    fill_cache( h, frame );
    if( (int64_t)frame > cache_last_frame( h ) ) /* eof */
        return -1;
    int64_t cache_start = h->eof ? (int64_t)h->eof - h->max_size : h->first_frame;
    int64_t idx = (int64_t)frame - cache_start;
    if( idx < 0 || idx >= h->max_size )
        return -1;
    int cache_index = (int)idx;
    *output = *h->cache[cache_index];
    return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    /* the parent filter's frame has already been released so do nothing here */
    return 0;
}

static void free_filter( hnd_t handle )
{
    cache_hnd_t *h = handle;
    if( !h )
        return;
    if( h->prev_hnd && h->prev_filter.free )
        h->prev_filter.free( h->prev_hnd );
    cache_free_partial( h );
}

cli_vid_filter_t cache_filter = { NAME, NULL, init, get_frame, release_frame, free_filter, NULL };
