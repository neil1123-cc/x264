/*****************************************************************************
 * thread.c: threaded input
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
#include "common/common.h"

#define thread_input x264_glue3(thread, BIT_DEPTH, input)

typedef struct
{
    cli_input_t input;
    hnd_t p_handle;
    cli_pic_t pic;
    x264_threadpool_t *pool;
    int next_frame;
    int frame_total;
    struct thread_input_arg_t *next_args;
} thread_hnd_t;

typedef struct thread_input_arg_t
{
    thread_hnd_t *h;
    cli_pic_t *pic;
    int i_frame;
    int status;
} thread_input_arg_t;

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    thread_hnd_t *h;
    if( !p_handle || !*p_handle || !info ||
        !cli_input.picture_alloc || !cli_input.picture_clean || !cli_input.read_frame )
        return -1;

    h = calloc( 1, sizeof(thread_hnd_t) );
    FAIL_IF_ERR( !h, "x264", "malloc failed\n" );
    h->input = cli_input;
    h->p_handle = *p_handle;
    h->next_frame = -1;
    h->frame_total = info->num_frames;

    if( h->input.picture_alloc( &h->pic, h->p_handle, info->csp, info->width, info->height ) )
    {
        x264_cli_log( "x264", X264_LOG_ERROR, "malloc failed\n" );
        goto fail;
    }

    h->next_args = calloc( 1, sizeof(thread_input_arg_t) );
    if( !h->next_args )
        goto fail;
    h->next_args->h = h;

    if( x264_threadpool_init( &h->pool, 1 ) )
        goto fail;

    *p_handle = h;
    return 0;

fail:
    if( h->input.picture_clean )
        h->input.picture_clean( &h->pic, h->p_handle );
    free( h->next_args );
    free( h );
    return -1;
}

static void *read_frame_thread_int( void *arg )
{
    thread_input_arg_t *i = arg;
    if( !i || !i->h || !i->pic || !i->h->p_handle || !i->h->input.read_frame ||
        i->i_frame < 0 || i->i_frame == INT_MAX ||
        (i->h->frame_total && i->i_frame >= i->h->frame_total) )
    {
        if( i )
            i->status = -1;
        return NULL;
    }
    i->status = i->h->input.read_frame( i->pic, i->h->p_handle, i->i_frame );
    return NULL;
}

static int read_frame( cli_pic_t *p_pic, hnd_t handle, int i_frame )
{
    thread_hnd_t *h = handle;
    int ret = 0;

    if( !p_pic || !h || !h->p_handle || !h->pool || !h->next_args || !h->input.read_frame ||
        i_frame < 0 || i_frame == INT_MAX || (h->frame_total && i_frame >= h->frame_total) )
        return -1;

    if( h->next_frame >= 0 )
    {
        x264_threadpool_wait( h->pool, h->next_args );
        ret |= h->next_args->status;
        if( ret )
        {
            h->next_frame = -1;
            return ret;
        }
    }

    if( h->next_frame == i_frame )
        XCHG( cli_pic_t, *p_pic, h->pic );
    else
    {
        if( h->next_frame >= 0 )
        {
            ret = thread_input.release_frame( &h->pic, handle );
            if( ret )
            {
                h->next_frame = -1;
                return ret;
            }
        }
        ret = h->input.read_frame( p_pic, h->p_handle, i_frame );
    }

    if( ret )
    {
        h->next_frame = -1;
        return ret;
    }

    if( i_frame < INT_MAX && (!h->frame_total || i_frame+1 < h->frame_total) )
    {
        h->next_frame =
        h->next_args->i_frame = i_frame+1;
        h->next_args->pic = &h->pic;
        h->next_args->status = 0;
        x264_threadpool_run( h->pool, read_frame_thread_int, h->next_args );
    }
    else
        h->next_frame = -1;

    return ret;
}

static int release_frame( cli_pic_t *pic, hnd_t handle )
{
    thread_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle )
        return -1;
    if( h->input.release_frame )
        return h->input.release_frame( pic, h->p_handle );
    return 0;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    thread_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle || !h->input.picture_alloc )
        return -1;
    return h->input.picture_alloc( pic, h->p_handle, csp, width, height );
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    thread_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle || !h->input.picture_clean )
        return;
    h->input.picture_clean( pic, h->p_handle );
}

static int close_file( hnd_t handle )
{
    thread_hnd_t *h = handle;
    int ret = 0;
    if( !h )
        return 0;
    if( h->pool && h->next_frame >= 0 )
    {
        x264_threadpool_wait( h->pool, h->next_args );
        if( !h->next_args->status && h->p_handle && h->input.release_frame &&
            h->input.release_frame( &h->pic, h->p_handle ) )
            ret = -1;
        h->next_frame = -1;
    }
    if( h->pool )
        x264_threadpool_delete( h->pool );
    if( h->p_handle && h->input.picture_clean )
        h->input.picture_clean( &h->pic, h->p_handle );
    if( h->p_handle && h->input.close_file )
    {
        int close_ret = h->input.close_file( h->p_handle );
        if( !ret )
            ret = close_ret;
    }
    free( h->next_args );
    free( h );
    return ret;
}

const cli_input_t thread_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
