/*****************************************************************************
 * ffms.c: ffmpegsource input
 *****************************************************************************
 * Copyright (C) 2009-2025 x264 project
 *
 * Authors: Mike Gurlitz <mike.gurlitz@gmail.com>
 *          Steven Walters <kemuri9@gmail.com>
 *          Henrik Gramner <henrik@gramner.com>
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
#include <ffms.h>

#undef DECLARE_ALIGNED
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <limits.h>
#include <math.h>

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "ffms", __VA_ARGS__ )
#define FAIL_IF_ERROR_CLEANUP( cond, ... )\
do\
{\
    if( cond )\
    {\
        x264_cli_log( "ffms", X264_LOG_ERROR, __VA_ARGS__ );\
        goto fail;\
    }\
} while( 0 )

#define PROGRESS_LENGTH 36

static inline int invalid_dimensions( int width, int height )
{
    return width <= 0 || height <= 0 || width > MAX_RESOLUTION || height > MAX_RESOLUTION;
}

static int required_pixel_planes( int csp )
{
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get( csp );
    if( !pix_desc )
        return -1;
    int planes = 0;
    for( int i = 0; i < pix_desc->nb_components; i++ )
    {
        int plane = pix_desc->comp[i].plane + 1;
        planes = X264_MAX( planes, plane );
    }
    return planes > 0 && planes <= 4 ? planes : -1;
}

static int decoded_video_planes_are_invalid( int pix_fmt, int width, const uint8_t * const data[4], const int linesize[4], int planes )
{
    if( planes <= 0 || planes > 4 )
        return 1;

    for( int i = 0; i < planes; i++ )
    {
        int stride = linesize[i];
        int row_size = av_image_get_linesize( pix_fmt, width, i );
        if( stride == INT_MIN )
            return 1;
        stride = stride < 0 ? -stride : stride;
        if( row_size <= 0 || stride < row_size || !data[i] )
            return 1;
    }
    return 0;
}

static inline int64_t reduce_pts_floor( int64_t pts, int shift )
{
    if( shift <= 0 )
        return pts;
    if( shift >= 63 )
        return pts < 0 ? -1 : 0;

    int64_t divisor = (int64_t)(UINT64_C(1) << shift);
    if( pts >= 0 )
        return pts / divisor;
    return -1 - ( ( -1 - pts ) / divisor );
}

#if HAVE_AUDIO
#include "audio/audio.h"
#endif

typedef struct
{
    FFMS_VideoSource *video_source;
    FFMS_Track *track;
    int reduce_pts;
    int vfr_input;
    int num_frames;
    int64_t time;
#if HAVE_AUDIO
    char *filename;
    int has_audio;
#endif
} ffms_hnd_t;

static int FFMS_CC update_progress( int64_t current, int64_t total, void *private )
{
    int64_t *update_time = private;
    int64_t oldtime = *update_time;
    int64_t newtime = x264_mdate();
    if( oldtime && newtime - oldtime < UPDATE_INTERVAL )
        return 0;
    *update_time = newtime;

    char buf[PROGRESS_LENGTH+5+1];
    double percent = total ? 100.0 * (double)current / (double)total : 0.0;
    snprintf( buf, sizeof(buf), "ffms [info]: indexing input file [%.1f%%]", percent );
    fprintf( stderr, "%-*s\r", PROGRESS_LENGTH, buf+5 );
    x264_cli_set_console_title( buf );
    fflush( stderr );
    return 0;
}

/* handle the deprecated jpeg pixel formats */
static int handle_jpeg( int csp, int *fullrange )
{
    switch( csp )
    {
        case AV_PIX_FMT_YUVJ420P: *fullrange = 1; return AV_PIX_FMT_YUV420P;
        case AV_PIX_FMT_YUVJ422P: *fullrange = 1; return AV_PIX_FMT_YUV422P;
        case AV_PIX_FMT_YUVJ444P: *fullrange = 1; return AV_PIX_FMT_YUV444P;
        default:                               return csp;
    }
}

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    if( !psz_filename || !p_handle || !info || !opt )
        return -1;
    *p_handle = NULL;

    ffms_hnd_t *h = calloc( 1, sizeof(ffms_hnd_t) );
    if( !h )
        return -1;
    video_info_t updated_info = *info;

    FFMS_Init( 0, 1 );
    FFMS_ErrorInfo e;
    e.BufferSize = 0;
    int seekmode = opt->seek ? FFMS_SEEK_NORMAL : FFMS_SEEK_LINEAR_NO_RW;

    FFMS_Index *idx = NULL;
    FFMS_Indexer *idxer = NULL;
    if( opt->index_file )
    {
        x264_struct_stat index_s, input_s;
        if( !x264_stat( opt->index_file, &index_s ) && !x264_stat( psz_filename, &input_s ) && input_s.st_mtime < index_s.st_mtime )
        {
            idx = FFMS_ReadIndex( opt->index_file, &e );
            if( idx && FFMS_IndexBelongsToFile( idx, psz_filename, &e ) )
            {
                FFMS_DestroyIndex( idx );
                idx = NULL;
            }
        }
    }
    if( !idx )
    {
        FFMS_Indexer *indexer = FFMS_CreateIndexer( psz_filename, &e );
        FAIL_IF_ERROR_CLEANUP( !indexer, "could not create indexer\n" );

        if( opt->progress )
            FFMS_SetProgressCallback( indexer, update_progress, &h->time );

        idx = FFMS_DoIndexing2( indexer, FFMS_IEH_ABORT, &e );
        fprintf( stderr, "%*c", PROGRESS_LENGTH+1, '\r' );
        FAIL_IF_ERROR_CLEANUP( !idx, "could not create index\n" );

        if( opt->index_file && FFMS_WriteIndex( opt->index_file, idx, &e ) )
            x264_cli_log( "ffms", X264_LOG_WARNING, "could not write index file\n" );
    }

    int trackno = FFMS_GetFirstTrackOfType( idx, FFMS_TYPE_VIDEO, &e );
    if( trackno >= 0 )
	{
#if HAVE_AUDIO
        h->filename  = strdup( psz_filename );
        FAIL_IF_ERROR_CLEANUP( !h->filename, "malloc failed\n" );
        h->has_audio = !!( FFMS_GetFirstTrackOfType( idx, FFMS_TYPE_AUDIO, &e ) >= 0 );
#endif
        h->video_source = FFMS_CreateVideoSource( psz_filename, trackno, idx, opt->demuxer_threads, seekmode, &e );
	}
    FFMS_DestroyIndex( idx );
    idx = NULL;

    FAIL_IF_ERROR_CLEANUP( trackno < 0, "could not find video track\n" );
    FAIL_IF_ERROR_CLEANUP( !h->video_source, "could not create video source\n" );

    const FFMS_VideoProperties *videop = FFMS_GetVideoProperties( h->video_source );
    FAIL_IF_ERROR_CLEANUP( !videop, "could not get video properties\n" );
    FAIL_IF_ERROR_CLEANUP( videop->NumFrames < 0 || videop->NumFrames > INT_MAX, "invalid frame count\n" );
    FAIL_IF_ERROR_CLEANUP( videop->SARDen < 0 || (uint64_t)videop->SARDen > UINT32_MAX ||
                           videop->SARNum < 0 || (uint64_t)videop->SARNum > UINT32_MAX,
                           "invalid sample aspect ratio\n" );
    FAIL_IF_ERROR_CLEANUP( videop->FPSDenominator < 0 || (uint64_t)videop->FPSDenominator > UINT32_MAX ||
                           videop->FPSNumerator < 0 || (uint64_t)videop->FPSNumerator > UINT32_MAX,
                           "invalid framerate\n" );
    updated_info.num_frames   = h->num_frames = (int)videop->NumFrames;
    updated_info.sar_height   = (uint32_t)videop->SARDen;
    updated_info.sar_width    = (uint32_t)videop->SARNum;
    updated_info.fps_den      = (uint32_t)videop->FPSDenominator;
    updated_info.fps_num      = (uint32_t)videop->FPSNumerator;
    h->vfr_input              = updated_info.vfr;
    /* ffms is thread unsafe as it uses a single frame buffer for all frame requests */
    updated_info.thread_safe  = 0;
	
    if( !opt->b_accurate_fps )
        x264_ntsc_fps( &updated_info.fps_num, &updated_info.fps_den );

    const FFMS_Frame *frame = FFMS_GetFrame( h->video_source, 0, &e );
    FAIL_IF_ERROR_CLEANUP( !frame, "could not read frame 0\n" );
    FAIL_IF_ERROR_CLEANUP( invalid_dimensions( frame->EncodedWidth, frame->EncodedHeight ),
                           "invalid video dimensions\n" );
	
    /* -1 = 'unset' (internal) , 2 from lavf|ffms = 'unset' */
    if( frame->ColorSpace >= 0 && frame->ColorSpace <= 8 && frame->ColorSpace != 2 )
        updated_info.colormatrix = frame->ColorSpace;
    else
        updated_info.colormatrix = -1;

    updated_info.fullrange  = 0;
    updated_info.width      = frame->EncodedWidth;
    updated_info.height     = frame->EncodedHeight;
    updated_info.csp        = handle_jpeg( frame->EncodedPixelFormat, &updated_info.fullrange ) | X264_CSP_OTHER;
    updated_info.interlaced = frame->InterlacedFrame;
    updated_info.tff        = frame->TopFieldFirst;
    updated_info.fullrange |= frame->ColorRange == FFMS_CR_JPEG;

    /* ffms timestamps are in milliseconds. ffms also uses int64_ts for timebase,
     * so we need to reduce large timebases to prevent overflow */
    h->track = FFMS_GetTrackFromVideo( h->video_source );
    FAIL_IF_ERROR_CLEANUP( !h->track, "could not get video track\n" );
    const FFMS_TrackTimeBase *timebase = FFMS_GetTimeBase( h->track );
    FAIL_IF_ERROR_CLEANUP( !timebase || timebase->Num <= 0 || timebase->Den <= 0 || timebase->Den > INT64_MAX / 1000,
                           "invalid timebase\n" );
    if( h->vfr_input )
    {
        int64_t timebase_num = timebase->Num;
        int64_t timebase_den = timebase->Den * 1000;
        h->reduce_pts = 0;

        while( timebase_num > UINT32_MAX || timebase_den > INT32_MAX )
        {
            timebase_num >>= 1;
            timebase_den >>= 1;
            h->reduce_pts++;
        }
        FAIL_IF_ERROR_CLEANUP( timebase_num <= 0 || timebase_den <= 0, "invalid reduced timebase\n" );
        updated_info.timebase_num = (uint32_t)timebase_num;
        updated_info.timebase_den = (uint32_t)timebase_den;
    }
	
    /* show video info */
    idxer                  = FFMS_CreateIndexer( psz_filename, &e );
    const char *format     = idxer ? FFMS_GetFormatNameI( idxer ) : "unknown";
    const char *codec      = idxer ? FFMS_GetCodecNameI( idxer, trackno ) : "unknown";
    if( !format )
        format = "unknown";
    if( !codec )
        codec = "unknown";
    double duration        = videop->FPSNumerator > 0 && videop->FPSDenominator > 0 ?
                             (double)videop->NumFrames * videop->FPSDenominator / videop->FPSNumerator : 0;
    int duration_log       = isfinite( duration ) && duration > 0.0
                           ? duration > INT_MAX ? INT_MAX : (int)duration
                           : 0;
    const AVPixFmtDescriptor *pix_desc = av_pix_fmt_desc_get(frame->EncodedPixelFormat);
    x264_cli_log( "ffms", X264_LOG_INFO,
                  "\n Format    : %s"
                  "\n Codec     : %s"
                  "\n PixFmt    : %s"
                  "\n Framerate : %d/%d"
                  "\n Timebase  : %"PRIu64"/%"PRIu64
                  "\n Duration  : %d:%02d:%02d\n",
                  format,
                  codec,
                  pix_desc ? pix_desc->name : "unknown",
                  videop->FPSNumerator, videop->FPSDenominator,
                  (uint64_t)timebase->Num, (uint64_t)timebase->Den * 1000,
                  duration_log / 60 / 60, duration_log / 60 % 60, duration_log - duration_log / 60 * 60 );
    if( codec && !strcmp( codec,"rawvideo" ) )
        x264_cli_log( "ffms", X264_LOG_WARNING, "recommend using --demuxer lavf with rawvideo" );
    if( idxer )
    {
        FFMS_CancelIndexing( idxer );
        idxer = NULL;
    }

    *info = updated_info;
    *p_handle = h;
    return 0;

fail:
    if( idxer )
        FFMS_CancelIndexing( idxer );
    if( idx )
        FFMS_DestroyIndex( idx );
    if( h->video_source )
        FFMS_DestroyVideoSource( h->video_source );
#if HAVE_AUDIO
    free( h->filename );
#endif
    free( h );
    return -1;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    if( !pic )
        return -1;
    if( x264_cli_pic_alloc( pic, X264_CSP_NONE, width, height ) )
        return -1;
    pic->img.csp = csp;
    pic->img.planes = 4;
    return 0;
}

static int read_frame( cli_pic_t *pic, hnd_t handle, int i_frame )
{
    ffms_hnd_t *h = handle;
    if( !pic || !h || i_frame < 0 || i_frame >= h->num_frames ||
        !h->video_source || (h->vfr_input && !h->track) )
        return -1;
    FFMS_ErrorInfo e;
    e.BufferSize = 0;
    const FFMS_Frame *frame = FFMS_GetFrame( h->video_source, i_frame, &e );
    FAIL_IF_ERROR( !frame, "could not read frame %d \n", i_frame );
    FAIL_IF_ERROR( invalid_dimensions( frame->EncodedWidth, frame->EncodedHeight ),
                   "invalid video dimensions\n" );

    int is_fullrange = 0;
    int pix_fmt = handle_jpeg( frame->EncodedPixelFormat, &is_fullrange );
    int csp = pix_fmt | X264_CSP_OTHER;
    int64_t pts = 0;
    int64_t duration = 0;

    if( h->vfr_input )
    {
        const FFMS_FrameInfo *info = FFMS_GetFrameInfo( h->track, i_frame );
        FAIL_IF_ERROR( !info || info->PTS == AV_NOPTS_VALUE, "invalid timestamp. "
                       "Use --force-cfr and specify a framerate with --fps\n" );

        pts = reduce_pts_floor( info->PTS, h->reduce_pts );
    }
    memcpy( pic->img.stride, frame->Linesize, sizeof(pic->img.stride) );
    memcpy( pic->img.plane, frame->Data, sizeof(pic->img.plane) );
    int planes = required_pixel_planes( pix_fmt );
    FAIL_IF_ERROR( planes < 0, "invalid pixel format\n" );
    FAIL_IF_ERROR( decoded_video_planes_are_invalid( pix_fmt, frame->EncodedWidth, frame->Data,
                                                     frame->Linesize, planes ),
                   "invalid frame plane data\n" );
    pic->img.width   = frame->EncodedWidth;
    pic->img.height  = frame->EncodedHeight;
    pic->img.csp     = csp;
    if( h->vfr_input )
    {
        pic->pts = pts;
        pic->duration = duration;
    }
    return 0;
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    if( !pic )
        return;
    memset( pic, 0, sizeof(cli_pic_t) );
}

static int close_file( hnd_t handle )
{
    ffms_hnd_t *h = handle;
    if( !h )
        return 0;
    FFMS_DestroyVideoSource( h->video_source );
#if HAVE_AUDIO
    free( h->filename );
#endif
    free( h );
    return 0;
}

#if HAVE_AUDIO
static hnd_t open_audio( hnd_t handle, int track )
{
    ffms_hnd_t *h = handle;
    if( !h || !h->filename )
        return NULL;
    if( !x264_is_regular_file_path( h->filename ) )
    {
        x264_cli_log( "ffms", X264_LOG_WARNING, "reading audio from non-regular files is not implemented yet.\n" );
        return 0;
    }
    if( !h->has_audio )
        return 0;
    return x264_audio_open_from_file( NULL, h->filename, track );
}

const cli_input_t ffms_input = { open_file, picture_alloc, read_frame, NULL, picture_clean, close_file, open_audio };
#else
const cli_input_t ffms_input = { open_file, picture_alloc, read_frame, NULL, picture_clean, close_file, NULL };
#endif
