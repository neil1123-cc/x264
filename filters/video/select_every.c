/*****************************************************************************
 * select_every.c: select-every video filter
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

#define NAME "select_every"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

#define MAX_PATTERN_SIZE 100 /* arbitrary */

typedef struct
{
    hnd_t prev_hnd;
    cli_vid_filter_t prev_filter;

    int *pattern;
    int pattern_len;
    int step_size;
    int vfr;
    int64_t pts;
} selvry_hnd_t;

cli_vid_filter_t select_every_filter;

static int select_every_frame_number( selvry_hnd_t *h, int frame, int *pat_frame )
{
    if( frame < 0 || !h->pattern_len || !h->step_size )
        return -1;
    int64_t selected = h->pattern[frame % h->pattern_len] +
                       (int64_t)(frame / h->pattern_len) * h->step_size;
    if( selected > INT_MAX )
        return -1;
    *pat_frame = (int)selected;
    return 0;
}

static int select_every_num_frames( selvry_hnd_t *h, int num_frames )
{
    uint64_t selected = (uint64_t)(num_frames / h->step_size) * (uint64_t)h->pattern_len;
    int tail_frames = num_frames % h->step_size;
    /* The final partial cycle is consumed in pattern order. */
    for( int i = 0; i < h->pattern_len; i++ )
    {
        if( h->pattern[i] >= tail_frames )
            break;
        selected++;
    }
    if( selected > INT_MAX )
        return -1;
    return (int)selected;
}

static void help( int longhelp )
{
    printf( "      "NAME":step,offset1[,...]\n" );
    if( !longhelp )
        return;
    printf( "            apply a selection pattern to input frames\n"
            "            step: the number of frames in the pattern\n"
            "            offsets: the offset into the step to select a frame\n"
            "            see: http://avisynth.nl/index.php/Select#SelectEvery\n" );
}

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
    if( !handle || !*handle || !filter || !info || !param ||
        !filter->get_frame || !filter->release_frame || !filter->free )
        return -1;
    if( !opt_string )
        return -1;
    selvry_hnd_t *h = calloc( 1, sizeof(selvry_hnd_t) );
    if( !h )
        return -1;
    int offsets[MAX_PATTERN_SIZE];
    int parsed_step_size = 0;
    int parsed_pattern_len = 0;
    char *tok = opt_string;
    for( int i = 0; ; i++ )
    {
        char *end = strchr( tok, ',' );
        int val;
        if( end )
            *end = '\0';
        if( !*tok )
        {
            x264_cli_log( NAME, X264_LOG_ERROR, "empty %s\n", i ? "offset" : "step" );
            goto fail;
        }
        int invalid = x264_otoi_checked( tok, &val );
        if( !i )
        {
            if( invalid || val <= 0 )
            {
                x264_cli_log( NAME, X264_LOG_ERROR, "invalid step `%s'\n", tok );
                goto fail;
            }
            parsed_step_size = val;
        }
        else
        {
            if( invalid || val < 0 || val >= parsed_step_size )
            {
                x264_cli_log( NAME, X264_LOG_ERROR, "invalid offset `%s'\n", tok );
                goto fail;
            }
            if( parsed_pattern_len >= MAX_PATTERN_SIZE )
            {
                x264_cli_log( NAME, X264_LOG_ERROR, "max pattern size %d reached\n", MAX_PATTERN_SIZE );
                goto fail;
            }
            offsets[parsed_pattern_len++] = val;
        }
        if( !end )
            break;
        tok = end + 1;
    }
    if( !parsed_step_size )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "no step size provided\n" );
        goto fail;
    }
    if( !parsed_pattern_len )
    {
        x264_cli_log( NAME, X264_LOG_ERROR, "no offsets supplied\n" );
        goto fail;
    }

    h->pattern = malloc( (size_t)parsed_pattern_len * sizeof(int) );
    if( !h->pattern )
        goto fail;
    memcpy( h->pattern, offsets, (size_t)parsed_pattern_len * sizeof(int) );
    h->step_size = parsed_step_size;
    h->pattern_len = parsed_pattern_len;

    /* determine required cache size to maintain pattern. */
    intptr_t max_rewind = 0;
    int min = h->step_size;
    for( int i = h->pattern_len-1; i >= 0; i-- )
    {
         min = X264_MIN( min, offsets[i] );
         if( i )
             max_rewind = X264_MAX( max_rewind, offsets[i-1] - min + 1 );
         /* reached maximum rewind size */
         if( max_rewind == h->step_size )
             break;
    }
    char name[20];
    int name_len = snprintf( name, sizeof(name), "cache_%d", param->i_bitdepth );
    if( name_len < 0 || (size_t)name_len >= sizeof(name) )
        goto fail;

    /* done initing, overwrite properties */
    video_info_t updated_info = *info;
    if( h->step_size != h->pattern_len )
    {
        if( updated_info.num_frames > 0 )
        {
            int selected_frames = select_every_num_frames( h, updated_info.num_frames );
            if( selected_frames < 0 )
            {
                x264_cli_log( NAME, X264_LOG_ERROR, "selected frame count is too large\n" );
                goto fail;
            }
            updated_info.num_frames = selected_frames;
        }
        if( updated_info.fps_den > UINT32_MAX / (uint32_t)h->step_size ||
            updated_info.fps_num > UINT32_MAX / (uint32_t)h->pattern_len )
        {
            x264_cli_log( NAME, X264_LOG_ERROR, "selected framerate is too large\n" );
            goto fail;
        }
        updated_info.fps_den *= (uint32_t)h->step_size;
        updated_info.fps_num *= (uint32_t)h->pattern_len;
        if( !param->b_accurate_fps )
            x264_ntsc_fps( &updated_info.fps_num, &updated_info.fps_den );
        x264_reduce_fraction( &updated_info.fps_num, &updated_info.fps_den );
        if( updated_info.vfr )
        {
            if( updated_info.timebase_den > UINT32_MAX / (uint32_t)h->pattern_len ||
                updated_info.timebase_num > UINT32_MAX / (uint32_t)h->step_size )
            {
                x264_cli_log( NAME, X264_LOG_ERROR, "selected timebase is too large\n" );
                goto fail;
            }
            updated_info.timebase_den *= (uint32_t)h->pattern_len;
            updated_info.timebase_num *= (uint32_t)h->step_size;
            x264_reduce_fraction( &updated_info.timebase_num, &updated_info.timebase_den );
        }
    }

    if( x264_init_vid_filter( name, handle, filter, &updated_info, param, (void*)max_rewind ) )
        goto fail;
    *info = updated_info;

    h->pts = 0;
    h->vfr = updated_info.vfr;
    h->prev_filter = *filter;
    h->prev_hnd = *handle;
    *filter = select_every_filter;
    *handle = h;

    return 0;

fail:
    free( h->pattern );
    free( h );
    return -1;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
    selvry_hnd_t *h = handle;
    if( !h || !output || !h->prev_hnd || !h->prev_filter.get_frame ||
        (h->vfr && !h->prev_filter.release_frame) )
        return -1;
    int pat_frame;
    if( select_every_frame_number( h, frame, &pat_frame ) )
        return -1;
    if( h->prev_filter.get_frame( h->prev_hnd, output, pat_frame ) )
        return -1;
    if( h->vfr )
    {
        int64_t duration = output->duration;
        if( duration < 0 || h->pts > INT64_MAX - duration )
        {
            h->prev_filter.release_frame( h->prev_hnd, output, pat_frame );
            return -1;
        }
        output->pts = h->pts;
        h->pts += duration;
    }
    return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
    selvry_hnd_t *h = handle;
    if( !h || !pic || !h->prev_hnd || !h->prev_filter.release_frame )
        return -1;
    int pat_frame;
    if( select_every_frame_number( h, frame, &pat_frame ) )
        return -1;
    return h->prev_filter.release_frame( h->prev_hnd, pic, pat_frame );
}

static void free_filter( hnd_t handle )
{
    selvry_hnd_t *h = handle;
    if( !h )
        return;
    if( h->prev_hnd && h->prev_filter.free )
        h->prev_filter.free( h->prev_hnd );
    free( h->pattern );
    free( h );
}

cli_vid_filter_t select_every_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
