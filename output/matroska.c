/*****************************************************************************
 * matroska.c: matroska muxer
 *****************************************************************************
 * Copyright (C) 2005-2025 x264 project
 *
 * Authors: Mike Matsnev <mike@haali.su>
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

#include "output.h"
#include "matroska_ebml.h"
#if HAVE_AUDIO
#include "audio/encoders.h"
#endif

#if HAVE_AUDIO
typedef struct
{
    audio_info_t *info;
    hnd_t encoder;
    int64_t lastdts;
} mkv_audio_hnd_t;
#endif

typedef struct
{
    mk_writer *w;
    mk_track_t tracks[MK_MAX_TRACKS];
    uint32_t i_track_count;
    uint32_t i_video_track;
    char b_writing_frame;
    uint32_t i_timebase_num;
    uint32_t i_timebase_den;
#if HAVE_AUDIO
    mkv_audio_hnd_t *a_mkv;
    uint32_t i_audio_track;
#endif
} mkv_hnd_t;

#define MKV_NANOSECONDS 1000000000LL
#define MKV_TIMECODE_SCALE 50000LL

static int mkv_timebase_to_ns( int64_t *dst, int64_t timestamp, uint32_t timebase_num, uint32_t timebase_den )
{
    if( timestamp < 0 || !timebase_num || !timebase_den )
        return -1;
    long double value = (long double)timestamp * (long double)MKV_NANOSECONDS *
                        (long double)timebase_num / (long double)timebase_den + 0.5L;
    if( value != value || value < 0.0L || value > (long double)INT64_MAX )
        return -1;
    *dst = (int64_t)value;
    return 0;
}

static int mkv_default_duration_from_fps( int64_t *dst, uint32_t fps_num, uint32_t fps_den )
{
    if( !fps_num )
        return -1;
    uint64_t duration = (uint64_t)fps_den * (uint64_t)MKV_NANOSECONDS / fps_num;
    if( !duration || duration > INT64_MAX )
        return -1;
    *dst = (int64_t)duration;
    return 0;
}

#if HAVE_AUDIO
static int audio_init( hnd_t handle, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !audio_enc )
        return -1;
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    hnd_t henc;

    if( !strcmp( audio_enc, "copy" ) )
        henc = x264_audio_copy_open( filters );
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, (char*[]){ "ac3", "aac", "vorbis", "mp3", "raw", NULL }, &used_enc );
        FAIL_IF_ERR( !encoder, "mkv", "unable to select audio encoder\n" );

        int audio_params_len = snprintf( audio_params, sizeof(audio_params), "%s,codec=%s",
                                         audio_parameters ? audio_parameters : "", used_enc );
        FAIL_IF_ERR( audio_params_len < 0, "mkv", "audio encoder parameters are too long\n" );
        size_t audio_params_size = (size_t)audio_params_len;
        FAIL_IF_ERR( audio_params_size >= sizeof(audio_params), "mkv", "audio encoder parameters are too long\n" );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }
    FAIL_IF_ERR( !henc, "mkv", "error opening audio encoder\n" );

    mkv_hnd_t *p_mkv = handle;
    mkv_audio_hnd_t *a_mkv = calloc( 1, sizeof( mkv_audio_hnd_t ) );
    if( !a_mkv )
    {
        x264_cli_log( "mkv", X264_LOG_ERROR, "malloc failed!\n" );
        goto error;
    }

    audio_info_t *info = x264_audio_encoder_info( henc );
    if( !info )
    {
        x264_cli_log( "mkv", X264_LOG_ERROR, "invalid audio codec information\n" );
        goto error;
    }

    a_mkv->lastdts = INVALID_DTS;
    a_mkv->encoder = henc;
    a_mkv->info    = info;
    p_mkv->a_mkv   = a_mkv;

    return 1;

error:
    x264_audio_encoder_close( henc );
    free( a_mkv );

    return -1;
}

static void mkv_close_audio( mkv_hnd_t *p_mkv )
{
    if( !p_mkv || !p_mkv->a_mkv )
        return;
    x264_audio_encoder_close( p_mkv->a_mkv->encoder );
    free( p_mkv->a_mkv );
    p_mkv->a_mkv = NULL;
}
#endif

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    if( !psz_filename || !p_handle )
        return -1;
    *p_handle = NULL;
    mkv_hnd_t *p_mkv = calloc( 1, sizeof(mkv_hnd_t) );
    if( !p_mkv )
        return -1;

    p_mkv->w = mk_create_writer( psz_filename );
    if( !p_mkv->w )
    {
        free( p_mkv );
        return -1;
    }

#if HAVE_AUDIO
    if( audio_init( p_mkv, audio_filters, audio_enc, audio_params ) < 0 )
    {
        int64_t last_delta[MK_MAX_TRACKS] = { 0 };
        x264_cli_log( "mkv", X264_LOG_ERROR, "unable to init audio output\n" );
        mk_close( p_mkv->w, last_delta );
        free( p_mkv );
        return -1;
    }
#endif

    *p_handle = p_mkv;

    return 0;
}


#define STEREO_COUNT 7
static const uint8_t stereo_modes[] = {5,9,7,1,3,13,0};
static const uint8_t stereo_w_div[] = {1,2,1,2,1,1,1};
static const uint8_t stereo_h_div[] = {1,1,2,1,2,1,1};
X264_STATIC_ASSERT( ARRAY_ELEMS(stereo_modes) == STEREO_COUNT, "Matroska stereo mode table size must match frame packing domain" );
X264_STATIC_ASSERT( ARRAY_ELEMS(stereo_w_div) == STEREO_COUNT, "Matroska stereo width divisor table size must match frame packing domain" );
X264_STATIC_ASSERT( ARRAY_ELEMS(stereo_h_div) == STEREO_COUNT, "Matroska stereo height divisor table size must match frame packing domain" );

static int set_video_track( mkv_hnd_t *p_mkv, x264_param_t *p_param )
{
    mk_track_t vtrack = {0};
    mk_video_info_t *v = &vtrack.info.v;
    int64_t dw, dh;

    vtrack.type = MK_TRACK_VIDEO;
    vtrack.lacing = MK_LACING_NONE;
    vtrack.codec_id = "V_MPEG4/ISO/AVC";
    vtrack.id = 1;

    if( !p_param->i_timebase_num || !p_param->i_timebase_den )
        return -1;

    if( !p_param->b_vfr_input && ( !p_param->i_fps_num || !p_param->i_fps_den ) )
        return -1;

    if( p_param->i_fps_num > 0 && !p_param->b_vfr_input )
    {
        if( mkv_default_duration_from_fps( &vtrack.default_frame_duration,
                                           p_param->i_fps_num, p_param->i_fps_den ) )
            return -1;
    }
    else
        vtrack.default_frame_duration = 0;

    if( p_param->i_width <= 0 || p_param->i_height <= 0 )
        return -1;
    dw = p_param->i_width;
    dh = p_param->i_height;
    v->width = (unsigned)p_param->i_width;
    v->height = (unsigned)p_param->i_height;
    v->display_size_units = DS_PIXELS;
    v->stereo_mode = -1;
    if( p_param->vui.i_sar_width < 0 || p_param->vui.i_sar_height < 0 )
        return -1;
    if( p_param->i_frame_packing >= 0 && p_param->i_frame_packing < STEREO_COUNT )
    {
        v->stereo_mode = stereo_modes[p_param->i_frame_packing];
        dw /= stereo_w_div[p_param->i_frame_packing];
        dh /= stereo_h_div[p_param->i_frame_packing];
    }
    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height
        && p_param->vui.i_sar_width != p_param->vui.i_sar_height )
    {
        if( p_param->vui.i_sar_width > p_param->vui.i_sar_height )
        {
            uint64_t sar_num = (uint64_t)p_param->vui.i_sar_width;
            uint64_t sar_den = (uint64_t)p_param->vui.i_sar_height;
            if( (uint64_t)dw > UINT64_MAX / sar_num )
                return -1;
            dw = (int64_t)( (uint64_t)dw * sar_num / sar_den );
        }
        else
        {
            uint64_t sar_num = (uint64_t)p_param->vui.i_sar_height;
            uint64_t sar_den = (uint64_t)p_param->vui.i_sar_width;
            if( (uint64_t)dh > UINT64_MAX / sar_num )
                return -1;
            dh = (int64_t)( (uint64_t)dh * sar_num / sar_den );
        }
    }
    if( dw <= 0 || dh <= 0 || dw > UINT_MAX || dh > UINT_MAX )
        return -1;
    unsigned display_width = (unsigned)dw;
    unsigned display_height = (unsigned)dh;
    v->display_width = display_width;
    v->display_height = display_height;

    p_mkv->tracks[1] = vtrack;
    p_mkv->i_video_track = 1;
    p_mkv->i_track_count = 1;
    p_mkv->i_timebase_num = p_param->i_timebase_num;
    p_mkv->i_timebase_den = p_param->i_timebase_den;

    return 0;
}

#if HAVE_AUDIO
static int codec_private_required( const char *codec )
{
    if( !strcmp( codec, MK_AUDIO_TAG_TTA  ) ||
        !strcmp( codec, MK_AUDIO_TAG_VORBIS ) ||
        !strcmp( codec, MK_AUDIO_TAG_FLAC ) ||
        !strcmp( codec, MK_AUDIO_TAG_AAC ) )
        return 1;

    return 0;
}

static int get_pcm_audio_framelen( int *dst, audio_info_t *info, x264_param_t *p_param )
{
    double framelen;
    if( info->samplerate <= 0 )
        return -1;
    if( !p_param->b_vfr_input )
    {
        if( p_param->i_fps_num <= 0 )
            return -1;
        framelen = (double)info->samplerate * p_param->i_fps_den / p_param->i_fps_num + 0.5;
    }
    else
    {
        if( p_param->i_timebase_den <= 0 )
            return -1;
        framelen = (double)info->samplerate * p_param->i_timebase_num / p_param->i_timebase_den + 0.5;
    }
    if( framelen != framelen || framelen <= 0.0 || framelen > INT_MAX )
        return -1;
    int frame_length = (int)framelen;
    *dst = frame_length;
    return 0;
}

static int set_audio_track( mkv_hnd_t *p_mkv, x264_param_t *p_param )
{
    mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv;
    audio_info_t *info = a_mkv->info;
    if( p_mkv->i_track_count >= MK_MAX_TRACKS - 1 )
        return -1;
    uint32_t audio_track_id = p_mkv->i_track_count + 1;
    mk_track_t atrack = {0};
    mk_audio_info_t *a = &atrack.info.a;
    int framelen = info->framelen;

    atrack.id = audio_track_id;
    atrack.type = MK_TRACK_AUDIO;
    atrack.lacing = MK_LACING_NONE;

    if( !strcmp( info->codec_name, "aac" ) )
        atrack.codec_id = MK_AUDIO_TAG_AAC;
    else if( !strcmp( info->codec_name, "ac3" ) )
        atrack.codec_id = MK_AUDIO_TAG_AC3;
    else if( !strcmp( info->codec_name, "eac3" ) )
        atrack.codec_id = MK_AUDIO_TAG_EAC3;
    else if( !strcmp( info->codec_name, "dca" ) )
        atrack.codec_id = MK_AUDIO_TAG_DTS;
    else if( !strcmp( info->codec_name, "vorbis" ) )
        atrack.codec_id = MK_AUDIO_TAG_VORBIS;
    else if( !strcmp( info->codec_name, "mp3" ) )
        atrack.codec_id = MK_AUDIO_TAG_MP3;
    else if( !strcmp( info->codec_name, "mp2" ) )
        atrack.codec_id = MK_AUDIO_TAG_MP2;
    else if( !strcmp( info->codec_name, "mp1" ) )
        atrack.codec_id = MK_AUDIO_TAG_MP1;
    else if( !strcmp( info->codec_name, "mlp" ) )
        atrack.codec_id = MK_AUDIO_TAG_MLP;
    else if( !strcmp( info->codec_name, "truehd" ) )
        atrack.codec_id = MK_AUDIO_TAG_TRUEHD;
    else if( !strcmp( info->codec_name, "tta" ) )
        atrack.codec_id = MK_AUDIO_TAG_TTA;
    else if( !strcmp( info->codec_name, "raw" ) )
        atrack.codec_id = MK_AUDIO_TAG_PCM_LE;
    else
    {
        x264_cli_log( "mkv", X264_LOG_ERROR, "unsupported audio codec\n" );
        return -1;
    }

    if( info->samplerate <= 0 || info->channels <= 0 || info->chansize < 0 ||
        info->framelen < 0 || info->extradata_size < 0 ||
        info->timebase.num <= 0 || info->timebase.den <= 0 )
        return -1;
    a->samplerate            = info->samplerate;
    a->channels              = info->channels;

    if( !strcmp( atrack.codec_id, MK_AUDIO_TAG_AAC ) )
    {
        audio_aac_info_t *aacinfo = info->opaque;
        if( aacinfo && aacinfo->has_sbr )
        {
            a->output_samplerate = info->samplerate;
            a->samplerate       /= 2;
        }
    }

    if( !strcmp( atrack.codec_id, MK_AUDIO_TAG_PCM_LE ) )
    {
        if( info->chansize <= 0 || info->chansize > INT_MAX / 8 )
            return -1;
        int audio_bit_depth = info->chansize * 8;
        a->bit_depth         = audio_bit_depth;

        // this is slightly inaccurate for some fps and samplerate conbinations
        if( get_pcm_audio_framelen( &framelen, info, p_param ) )
            return -1;
    }

    atrack.default_frame_duration = x264_from_timebase( framelen, info->timebase, MKV_NANOSECONDS );
    if( framelen > 0 &&
        (atrack.default_frame_duration <= 0 || atrack.default_frame_duration == INT64_MAX) )
        return -1;

    if( codec_private_required( atrack.codec_id ) )
    {
        if( info->extradata_size <= 0 || !info->extradata )
        {
            x264_cli_log( "mkv", X264_LOG_ERROR, "no extradata found!\n" );
            return -1;
        }
        size_t codec_private_size = (size_t)info->extradata_size;
        if( codec_private_size > UINT_MAX )
            return -1;
        unsigned codec_private_size_u = (unsigned)codec_private_size;
        atrack.codec_private_size = codec_private_size_u;
        atrack.codec_private = malloc( codec_private_size );
        if( !atrack.codec_private )
            return -1;
        memcpy( atrack.codec_private, info->extradata, codec_private_size );
    }

    p_mkv->tracks[audio_track_id] = atrack;
    p_mkv->i_audio_track = audio_track_id;
    p_mkv->i_track_count = audio_track_id;
    info->framelen = framelen;

    return 0;
}
#endif

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    mkv_hnd_t   *p_mkv = handle;
    if( !p_mkv || !p_mkv->w || !p_param )
        return -1;

    mkv_hnd_t staged = *p_mkv;

    FAIL_IF_ERR( set_video_track( &staged, p_param ), "mkv", "failed to create video track\n" );

#if HAVE_AUDIO
    FAIL_IF_ERR( staged.a_mkv && set_audio_track( &staged, p_param ), "mkv", "failed to create audio track\n" );
#endif

    memcpy( p_mkv->tracks, staged.tracks, sizeof(p_mkv->tracks) );
    p_mkv->i_track_count = staged.i_track_count;
    p_mkv->i_video_track = staged.i_video_track;
    p_mkv->i_timebase_num = staged.i_timebase_num;
    p_mkv->i_timebase_den = staged.i_timebase_den;
#if HAVE_AUDIO
    p_mkv->i_audio_track = staged.i_audio_track;
#endif

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    mkv_hnd_t *p_mkv = handle;
    if( !p_mkv || !p_mkv->w || !p_nal ||
        !p_mkv->i_video_track || p_mkv->i_video_track >= MK_MAX_TRACKS )
        return -1;
    mk_track_t *vtrack = &p_mkv->tracks[p_mkv->i_video_track];

    if( p_nal[0].i_payload < 8 || p_nal[1].i_payload < 5 )
        return -1;

    int sps_size = p_nal[0].i_payload - 4;
    int pps_size = p_nal[1].i_payload - 4;
    int sei_size = p_nal[2].i_payload;

    if( sei_size < 0 || sps_size > UINT16_MAX || pps_size > UINT16_MAX ||
        sei_size > INT_MAX - sps_size || sei_size + sps_size > INT_MAX - pps_size ||
        !p_nal[0].p_payload || !p_nal[1].p_payload || (sei_size && !p_nal[2].p_payload) )
        return -1;
    unsigned sps_size_u = (unsigned)sps_size;
    unsigned pps_size_u = (unsigned)pps_size;
    unsigned sei_size_u = (unsigned)sei_size;

    uint8_t *sps = p_nal[0].p_payload + 4;
    uint8_t *pps = p_nal[1].p_payload + 4;
    uint8_t *sei = p_nal[2].p_payload;

    int ret;
    unsigned codec_private_size = 5 + 1 + 2 + sps_size_u + 1 + 2 + pps_size_u;
    uint8_t *codec_private = malloc( codec_private_size );

    if( !codec_private )
        return -1;
    uint8_t *avcC = codec_private;

    avcC[0] = 1;
    avcC[1] = sps[1];
    avcC[2] = sps[2];
    avcC[3] = sps[3];
    avcC[4] = 0xff; // nalu size length is four bytes
    avcC[5] = 0xe1; // one sps

    avcC[6] = (uint8_t)(sps_size >> 8);
    avcC[7] = (uint8_t)sps_size;

    memcpy( avcC+8, sps, (size_t)sps_size );

    avcC[8+sps_size] = 1; // one pps
    avcC[9+sps_size] = (uint8_t)(pps_size >> 8);
    avcC[10+sps_size] = (uint8_t)pps_size;

    memcpy( avcC+11+sps_size, pps, (size_t)pps_size );

    mk_track_t header_tracks[MK_MAX_TRACKS];
    memcpy( header_tracks, p_mkv->tracks, sizeof(header_tracks) );
    header_tracks[p_mkv->i_video_track].codec_private_size = codec_private_size;
    header_tracks[p_mkv->i_video_track].codec_private = codec_private;
    int track_count = (int)p_mkv->i_track_count;

    ret = mk_write_header( p_mkv->w, "x264 "X264_COREVER, MKV_TIMECODE_SCALE,
                           header_tracks, track_count );

    if( ret < 0 )
    {
        free( codec_private );
        return ret;
    }

    // SEI

    if( mk_start_frame( p_mkv->w ) < 0 )
    {
        free( codec_private );
        return -1;
    }

    if( mk_add_frame_data( p_mkv->w, sei, sei_size_u ) < 0 )
    {
        free( codec_private );
        return -1;
    }

    vtrack->codec_private_size = codec_private_size;
    vtrack->codec_private = codec_private;
    p_mkv->b_writing_frame = 1;

    return sei_size + sps_size + pps_size;
}

#if HAVE_AUDIO
static int write_audio( mkv_hnd_t *p_mkv, int64_t video_dts, int finish )
{
    if( !p_mkv || !p_mkv->a_mkv || !p_mkv->w )
        return -1;
    mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv;
    if( !a_mkv->encoder || !a_mkv->info || a_mkv->info->samplerate <= 0 )
        return -1;

    if( a_mkv->lastdts == INVALID_DTS )
    {
        if( video_dts > 0 )
        {
            uint64_t skip_ns = (uint64_t)video_dts;
            uint64_t samplerate = (uint64_t)a_mkv->info->samplerate;
            uint64_t skip_samples = skip_ns > UINT64_MAX / samplerate ? UINT64_MAX : skip_ns * samplerate / MKV_NANOSECONDS;
            x264_audio_encoder_skip_samples( a_mkv->encoder, skip_samples );
        }
        a_mkv->lastdts = video_dts; // first frame (nonzero if --seek is used)
    }

    audio_packet_t *frame;
    while( a_mkv->lastdts <= video_dts || video_dts < 0 )
    {
        if( finish )
            frame = x264_audio_encoder_finish( a_mkv->encoder );
        else if( !(frame = x264_audio_encode_frame( a_mkv->encoder )) )
        {
            if( x264_audio_encoder_failed( a_mkv->encoder ) )
                return -1;
            finish = 1;
            continue;
        }

        if( !frame )
        {
            if( x264_audio_encoder_failed( a_mkv->encoder ) )
                return -1;
            break;
        }

        if( frame->dts < 0 || frame->size < 0 || (frame->size && !frame->data) )
        {
            x264_audio_free_frame( a_mkv->encoder, frame );
            return -1;
        }
        unsigned frame_size = (unsigned)frame->size;
        int64_t audio_dts = x264_from_timebase( frame->dts, frame->info.timebase, MKV_NANOSECONDS );
        if( audio_dts < 0 || audio_dts == INT64_MAX )
        {
            x264_audio_free_frame( a_mkv->encoder, frame );
            return -1;
        }

        if( mk_start_frame( p_mkv->w ) < 0 )
        {
            x264_audio_free_frame( a_mkv->encoder, frame );
            return -1;
        }

        if( mk_add_frame_data( p_mkv->w, frame->data, frame_size ) < 0 )
        {
            x264_audio_free_frame( a_mkv->encoder, frame );
            return -1;
        }

        if( mk_set_frame_flags( p_mkv->w, audio_dts, 1, 0, p_mkv->i_audio_track ) < 0 )
        {
            x264_audio_free_frame( a_mkv->encoder, frame );
            return -1;
        }

        if( mk_end_frame( p_mkv->w, p_mkv->i_audio_track ) < 0 )
        {
            x264_audio_free_frame( a_mkv->encoder, frame );
            return -1;
        }

        x264_audio_free_frame( a_mkv->encoder, frame );

        a_mkv->lastdts = audio_dts;
    }

    return 0;
}
#endif

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    mkv_hnd_t *p_mkv = handle;
    int skip = 0;
    int64_t i_stamp;

    if( !p_mkv || !p_mkv->w || !p_picture ||
        !p_mkv->i_video_track || p_mkv->i_video_track >= MK_MAX_TRACKS ||
        i_size < 0 || (i_size && !p_nalu) ||
        mkv_timebase_to_ns( &i_stamp, p_picture->i_pts, p_mkv->i_timebase_num, p_mkv->i_timebase_den ) )
        return -1;
    unsigned nalu_size = (unsigned)i_size;

    if( p_mkv->b_writing_frame )
    {
        if( mk_add_frame_data( p_mkv->w, p_nalu, nalu_size ) < 0 ||
            mk_set_frame_flags( p_mkv->w, i_stamp, p_picture->b_keyframe, p_picture->i_type == X264_TYPE_B, p_mkv->i_video_track ) < 0 ||
            mk_end_frame( p_mkv->w, p_mkv->i_video_track ) < 0 )
            return -1;
        p_mkv->b_writing_frame = 0;
        skip = 1;
    }

#if HAVE_AUDIO
    FAIL_IF_ERR( p_mkv->a_mkv && write_audio( p_mkv, i_stamp, 0 ) < 0, "mkv", "error writing audio\n" );
#endif

    if( !skip )
    {
        if( mk_start_frame( p_mkv->w ) < 0 ||
            mk_add_frame_data( p_mkv->w, p_nalu, nalu_size ) < 0 ||
            mk_set_frame_flags( p_mkv->w, i_stamp, p_picture->b_keyframe, p_picture->i_type == X264_TYPE_B, p_mkv->i_video_track ) < 0 ||
            mk_end_frame( p_mkv->w, p_mkv->i_video_track ) < 0 )
                return -1;
    }

    return i_size;
}

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    mkv_hnd_t *p_mkv = handle;
    int ret = 0;
    int64_t i_last_delta[MK_MAX_TRACKS] = { 0 };

    if( !p_mkv || !p_mkv->w )
        return 0;

    int valid_video_track = p_mkv->i_video_track && p_mkv->i_video_track < MK_MAX_TRACKS;
    if( valid_video_track && largest_pts >= second_largest_pts && second_largest_pts >= 0 )
    {
        if( mkv_timebase_to_ns( &i_last_delta[p_mkv->i_video_track], largest_pts - second_largest_pts,
                                p_mkv->i_timebase_num, p_mkv->i_timebase_den ) )
            ret = -1;
    }
    else if( !valid_video_track )
        ret = -1;

#if HAVE_AUDIO
    if( p_mkv->a_mkv )
    {
        mkv_audio_hnd_t *a_mkv = p_mkv->a_mkv;
        if( !a_mkv->info || !p_mkv->i_audio_track || p_mkv->i_audio_track >= MK_MAX_TRACKS )
            ret = -1;
        else if( write_audio( p_mkv, -1, 1 ) < 0 )
        {
            x264_cli_log( "mkv", X264_LOG_ERROR, "error flushing audio\n" );
            ret = -1;
        }
        else
        {
            i_last_delta[p_mkv->i_audio_track] = x264_from_timebase( a_mkv->info->last_delta, a_mkv->info->timebase, MKV_NANOSECONDS );
            if( i_last_delta[p_mkv->i_audio_track] < 0 || i_last_delta[p_mkv->i_audio_track] == INT64_MAX )
                ret = -1;
        }
        mkv_close_audio( p_mkv );
    }
#endif

    if( mk_close( p_mkv->w, i_last_delta ) < 0 )
        ret = -1;

#if HAVE_AUDIO
    mkv_close_audio( p_mkv );
#endif

    uint32_t i;
    for( i=1; i<=p_mkv->i_track_count && i<(uint32_t)MK_MAX_TRACKS; i++ )
    {
        if( p_mkv->tracks[i].codec_private_size )
            free( p_mkv->tracks[i].codec_private );
    }

    free( p_mkv );

    return ret;
}

const cli_output_t mkv_output = { open_file, set_param, write_headers, write_frame, close_file };

#undef MKV_TIMECODE_SCALE
#undef MKV_NANOSECONDS
