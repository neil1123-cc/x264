/*****************************************************************************
 * flv.c: flv muxer
 *****************************************************************************
 * Copyright (C) 2009-2025 x264 project
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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
#include "flv_bytestream.h"
#if HAVE_AUDIO
#include "audio/encoders.h"
#endif
#include <float.h>
#if HAVE_AUDIO && HAVE_LSMASH
#include <lsmash.h>
#endif

#define CHECK(x)\
do {\
    if( (x) < 0 )\
        return -1;\
} while( 0 )

#if HAVE_AUDIO
typedef struct
{
    audio_info_t *info;
    hnd_t encoder;
    int header;
    int codecid;
    int stereo;
    int64_t lastdts;
} flv_audio_hnd_t;
#endif

typedef struct
{
    flv_buffer *c;

    uint8_t *sei;
    int sei_len;

    int64_t i_fps_num;
    int64_t i_fps_den;
    int64_t i_framenum;

    uint64_t i_framerate_pos;
    uint64_t i_duration_pos;
    uint64_t i_filesize_pos;
    uint64_t i_bitrate_pos;

    uint8_t b_write_length;
    int64_t i_prev_dts;
    int64_t i_prev_cts;
    int64_t i_delay_time;
    int64_t i_init_delta;
    int i_delay_frames;

    double d_timebase;
    int b_vfr_input;
    int b_dts_compress;

    unsigned start;

#if HAVE_AUDIO
    flv_audio_hnd_t *a_flv;
#endif
	int x264_bit_depth;
} flv_hnd_t;

static int flv_timebase_to_ms( int64_t *dst, int64_t timestamp, double timebase )
{
    long double value = (long double)timestamp * (long double)timebase * 1000.0L + 0.5L;
    if( value != value || value < 0.0L || value > (long double)INT64_MAX )
        return -1;
    *dst = (int64_t)value;
    return 0;
}

static int flv_add_i64( int64_t *dst, int64_t a, int64_t b )
{
    if( (b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b) )
        return -1;
    *dst = a + b;
    return 0;
}

static int flv_write_timestamp( flv_buffer *c, int64_t timestamp )
{
    if( timestamp < 0 || timestamp > UINT32_MAX )
        return -1;
    uint32_t timestamp32 = (uint32_t)timestamp;
    flv_put_be24( c, timestamp32 );
    flv_put_byte( c, timestamp32 >> 24 );
    return 0;
}

#if HAVE_AUDIO
static int flv_get_raw_audio_framelen( int *dst, audio_info_t *info, x264_param_t *p_param )
{
    double framelen;
    if( info->samplerate <= 0 )
        return -1;
    if( !p_param->b_vfr_input )
    {
        if( !p_param->i_fps_num || !p_param->i_fps_den )
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
    *dst = (int)framelen;
    return 0;
}

static int audio_init( hnd_t handle, hnd_t filters, char *audio_enc, char *audio_parameters )
{
    if( !audio_enc )
        return -1;
    if( !strcmp( audio_enc, "none" ) || !filters )
        return 0;

    // TODO: support adpcm_swf, pcm and aac

    hnd_t henc;

    if( !strcmp( audio_enc, "copy" ) )
        henc = x264_audio_copy_open( filters );
    else
    {
        char audio_params[MAX_ARGS];
        const char *used_enc;
        const audio_encoder_t *encoder = x264_select_audio_encoder( audio_enc, (char*[]){ "mp3", "aac", "raw", NULL }, &used_enc );
        FAIL_IF_ERR( !encoder, "flv", "unable to select audio encoder\n" );

        int audio_params_len = snprintf( audio_params, sizeof(audio_params), "%s,codec=%s",
                                         audio_parameters ? audio_parameters : "", used_enc );
        FAIL_IF_ERR( audio_params_len < 0 || (size_t)audio_params_len >= sizeof(audio_params), "flv", "audio encoder parameters are too long\n" );
        henc = x264_audio_encoder_open( encoder, filters, audio_params );
    }
    FAIL_IF_ERR( !henc, "flv", "error opening audio encoder\n" );
    flv_hnd_t *p_flv = handle;
    flv_audio_hnd_t *a_flv = calloc( 1, sizeof( flv_audio_hnd_t ) );
    if( !a_flv )
        goto error;
    a_flv->lastdts = INVALID_DTS;
    audio_info_t *info = x264_audio_encoder_info( henc );
    if( !info || info->samplerate <= 0 || info->channels <= 0 || info->chansize < 0 ||
        info->framelen < 0 || info->samplesize < 0 || info->extradata_size < 0 ||
        info->timebase.num <= 0 || info->timebase.den <= 0 )
        goto error;

    int header = 0;
    if ( !strcmp( info->codec_name, "raw" ) )
        a_flv->codecid = FLV_CODECID_RAW;
    else if( !strcmp( info->codec_name, "mp3" ) )
        a_flv->codecid = FLV_CODECID_MP3;
    else if( !strcmp( info->codec_name, "aac" ) )
        a_flv->codecid = FLV_CODECID_AAC;
    else
    {
        x264_cli_log( "flv", X264_LOG_ERROR, "unsupported audio codec\n" );
        goto error;
    }

    header |= a_flv->codecid;
    a_flv->stereo = info->channels == 2;

    if( a_flv->codecid == FLV_CODECID_AAC )
        header |= FLV_SAMPLERATE_44100HZ | FLV_SAMPLESSIZE_16BIT | FLV_STEREO;
    else
    {
        switch( info->samplerate )
        {
            case 5512:
            case 8000:
                header |= FLV_SAMPLERATE_SPECIAL;
                break;
            case 11025:
                header |= FLV_SAMPLERATE_11025HZ;
                break;
            case 22050:
                header |= FLV_SAMPLERATE_22050HZ;
                break;
            case 44100:
                header |= FLV_SAMPLERATE_44100HZ;
                break;
            default:
                x264_cli_log( "flv", X264_LOG_ERROR, "unsupported %dhz sample rate\n", info->samplerate );
                goto error;
        }

        switch( info->chansize )
        {
            case 1:
                header |= FLV_SAMPLESSIZE_8BIT;
                break;
            case 2:
                header |= FLV_SAMPLESSIZE_16BIT;
                break;
            default:
                if( info->chansize > INT_MAX / 8 )
                    goto error;
                x264_cli_log( "flv", X264_LOG_ERROR, "%d-bit audio not supported\n", info->chansize * 8 );
                goto error;
        }

        switch( info->channels )
        {
            case 1:
                header |= FLV_MONO;
                break;
            case 2:
                header |= FLV_STEREO;
                break;
            default:
                x264_cli_log( "flv", X264_LOG_ERROR, "%d-channel audio not supported\n", info->channels );
                goto error;
        }
    }

    a_flv->header   = header;
    a_flv->encoder  = henc;
    a_flv->info     = info;
    p_flv->a_flv    = a_flv;

    return 1;

error:
    x264_audio_encoder_close( henc );
    free( a_flv );

    return -1;
}

static void flv_close_audio( flv_hnd_t *p_flv )
{
    if( !p_flv || !p_flv->a_flv )
        return;
    x264_audio_encoder_close( p_flv->a_flv->encoder );
    free( p_flv->a_flv );
    p_flv->a_flv = NULL;
}
#endif

static int write_header( flv_buffer *c, int audio )
{
    flv_put_tag( c, "FLV" );                // Signature
    flv_put_byte( c, 1 );                   // Version
    flv_put_byte( c, 1 | (audio ? 4 : 0) ); // Video + Audio (if requested)
    flv_put_be32( c, 9 );                   // DataOffset
    flv_put_be32( c, 0 );                   // PreviousTagSize 0

    return flv_flush_data( c );
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    if( !psz_filename || !p_handle || !opt )
        return -1;
    *p_handle = NULL;

    flv_hnd_t *p_flv = calloc( 1, sizeof(flv_hnd_t) );
	int ret = -1;
    if( p_flv )
    {
		p_flv->x264_bit_depth=opt->x264_bit_depth;
        flv_buffer *c = flv_create_writer( psz_filename );
        if( c )
        {
            ret=0;
#if HAVE_AUDIO
            ret = audio_init( p_flv, audio_filters, audio_enc, audio_params );
            if( ret < 0 )
                x264_cli_log( "flv", X264_LOG_ERROR, "unable to init audio output\n" );
            else if( !write_header( c,ret ) )
#else
            if((ret==0) && !write_header( c,ret ))
#endif								
            {
                p_flv->c = c;
                p_flv->b_dts_compress = opt->use_dts_compress;
                *p_handle = p_flv;
#if HAVE_AUDIO
				return ret;
#else
				return 0;
#endif				
            }
#if HAVE_AUDIO
            if (ret>=0) ret=-1;
#else
            if (ret==0) ret=-1;		
#endif
#if HAVE_AUDIO
            flv_close_audio( p_flv );
#endif
            if( c->fp == stdout ? fflush( c->fp ) : fclose( c->fp ) )
                x264_cli_log( "flv", X264_LOG_ERROR, "failed to close output file `%s'\n", psz_filename );
            free( c->data );
            free( c );
        }
        free( p_flv );
    }

    return ret;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    flv_hnd_t *p_flv = handle;
    if( !p_flv || !p_flv->c || !p_param )
        return -1;
    flv_buffer *c = p_flv->c;
#if HAVE_AUDIO
    flv_audio_hnd_t *raw_audio = NULL;
    int raw_audio_framelen = 0;
#endif

    if( p_param->i_timebase_num <= 0 || p_param->i_timebase_den <= 0 ||
        ( !p_param->b_vfr_input && ( !p_param->i_fps_num || !p_param->i_fps_den ) ) )
        return -1;

    flv_put_byte( c, FLV_TAG_TYPE_META ); // Tag Type "script data"

    unsigned start = c->d_cur;
    flv_put_be24( c, 0 ); // data length
    flv_put_be24( c, 0 ); // timestamp
    flv_put_be32( c, 0 ); // reserved

    flv_put_byte( c, AMF_DATA_TYPE_STRING );
    flv_put_amf_string( c, "onMetaData" );

    flv_put_byte( c, AMF_DATA_TYPE_MIXEDARRAY );
    flv_put_be32( c, 7 );

    flv_put_amf_string( c, "width" );
    flv_put_amf_double( c, p_param->i_width );

    flv_put_amf_string( c, "height" );
    flv_put_amf_double( c, p_param->i_height );

    flv_put_amf_string( c, "framerate" );

    if( !p_param->b_vfr_input )
        flv_put_amf_double( c, (double)p_param->i_fps_num / p_param->i_fps_den );
    else
    {
        p_flv->i_framerate_pos = c->d_cur + c->d_total + 1;
        flv_put_amf_double( c, 0 ); // written at end of encoding
    }

    flv_put_amf_string( c, "videocodecid" );
    flv_put_amf_double( c, FLV_CODECID_H264 );

    flv_put_amf_string( c, "duration" );
    p_flv->i_duration_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "filesize" );
    p_flv->i_filesize_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

    flv_put_amf_string( c, "videodatarate" );
    p_flv->i_bitrate_pos = c->d_cur + c->d_total + 1;
    flv_put_amf_double( c, 0 ); // written at end of encoding

#if HAVE_AUDIO
    if( p_flv->a_flv )
    {
        flv_audio_hnd_t *a_flv = p_flv->a_flv;
        flv_put_amf_string( c, "audiocodecid" );
        flv_put_amf_double( c, a_flv->codecid >> FLV_AUDIO_CODECID_OFFSET );
        flv_put_amf_string( c, "audiosamplesize" );
        flv_put_amf_double( c, a_flv->info->chansize * 8 );
        flv_put_amf_string( c, "audiosamplerate" );
        flv_put_amf_double( c, a_flv->info->samplerate );
        flv_put_amf_string( c, "stereo" );
        flv_put_amf_bool  ( c, a_flv->stereo );
        if( a_flv->codecid == FLV_CODECID_RAW )
        {
            // this is slightly inaccurate for some fps and samplerate conbinations
            if( flv_get_raw_audio_framelen( &raw_audio_framelen, a_flv->info, p_param ) )
                return -1;
            raw_audio = a_flv;
        }
    }
#endif

    flv_put_amf_string( c, "" );
    flv_put_byte( c, AMF_END_OF_OBJECT );

    unsigned length = c->d_cur - start;
    if( length < 10 || length - 10 > 0xFFFFFF || length > UINT32_MAX - 1 )
        return -1;
    flv_rewrite_amf_be24( c, length - 10, start );

    flv_put_be32( c, length + 1 ); // tag length
    if( c->error )
        return -1;

    p_flv->i_fps_num = p_param->i_fps_num;
    p_flv->i_fps_den = p_param->i_fps_den;
    p_flv->d_timebase = (double)p_param->i_timebase_num / p_param->i_timebase_den;
    p_flv->b_vfr_input = p_param->b_vfr_input;
    p_flv->i_delay_frames = p_param->i_bframe ? (p_param->i_bframe_pyramid ? 2 : 1) : 0;
#if HAVE_AUDIO
    if( raw_audio )
        raw_audio->info->framelen = raw_audio_framelen;
#endif

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    flv_hnd_t *p_flv = handle;
    if( !p_flv || !p_flv->c || !p_nal ||
        !p_nal[0].p_payload || !p_nal[1].p_payload ||
        (p_nal[2].i_payload && !p_nal[2].p_payload) )
        return -1;
#if HAVE_AUDIO
    flv_audio_hnd_t *a_flv = p_flv->a_flv;
#endif
    flv_buffer *c = p_flv->c;
#if HAVE_AUDIO && HAVE_LSMASH
    lsmash_codec_specific_t *conv = NULL;
#endif

    int sps_size = p_nal[0].i_payload;
    int pps_size = p_nal[1].i_payload;
    int sei_size = p_nal[2].i_payload;
    if( sps_size < 4 || pps_size < 4 || sei_size < 0 ||
        sps_size - 4 > UINT16_MAX || pps_size - 4 > UINT16_MAX ||
        sei_size > INT_MAX - sps_size || sei_size + sps_size > INT_MAX - pps_size )
        return -1;

    // SEI
    /* It is within the spec to write this as-is but for
     * mplayer/ffmpeg playback this is deferred until before the first frame */

    uint8_t *sei = sei_size ? malloc( (size_t)sei_size ) : NULL;
    if( sei_size && !sei )
        return -1;

    if( sei_size )
        memcpy( sei, p_nal[2].p_payload, (size_t)sei_size );

    // SPS
    uint8_t *sps = p_nal[0].p_payload + 4;

    flv_put_byte( c, FLV_TAG_TYPE_VIDEO );
    flv_put_be24( c, 0 ); // rewrite later
    flv_put_be24( c, 0 ); // timestamp
    flv_put_byte( c, 0 ); // timestamp extended
    flv_put_be24( c, 0 ); // StreamID - Always 0
    p_flv->start = c->d_cur; // needed for overwriting length

    flv_put_byte( c, FLV_FRAME_KEY | FLV_CODECID_H264 ); // FrameType and CodecID
    flv_put_byte( c, 0 ); // AVC sequence header
    flv_put_be24( c, 0 ); // composition time

    flv_put_byte( c, 1 );      // version
    flv_put_byte( c, sps[1] ); // profile
    flv_put_byte( c, sps[2] ); // profile
    flv_put_byte( c, sps[3] ); // level
    flv_put_byte( c, 0xff );   // 6 bits reserved (111111) + 2 bits nal size length - 1 (11)
    flv_put_byte( c, 0xe1 );   // 3 bits reserved (111) + 5 bits number of sps (00001)

    flv_put_be16( c, (uint16_t)(sps_size - 4) );
    if( flv_append_data( c, sps, (unsigned)(sps_size - 4) ) )
        goto fail;

    // PPS
    flv_put_byte( c, 1 ); // number of pps
    flv_put_be16( c, (uint16_t)(pps_size - 4) );
    if( flv_append_data( c, p_nal[1].p_payload + 4, (unsigned)(pps_size - 4) ) )
        goto fail;
	
    // FRExt fields
    if( sps[1] == 100 || sps[1] == 110 || sps[1] == 122 || sps[1] == 144 )
    {
        flv_put_byte( c, 0xfd );   // 6 bits reserved (111111) + 2 bits chroma format indicator (1)
        flv_put_byte( c, (uint8_t)((p_flv->x264_bit_depth - 8) | 0xf8) );   // 5 bits reserved (11111) + 3 bits bit depth of the samples in the luma arrays
        flv_put_byte( c, (uint8_t)((p_flv->x264_bit_depth - 8) | 0xf8) );   // 5 bits reserved (11111) + 3 bits bit depth of the samples in the chroma arrays
        flv_put_byte( c, 0 );      // number of spsext
    }

    // rewrite data length info
    unsigned length = c->d_cur - p_flv->start;
    if( length > 0xFFFFFF || length > UINT32_MAX - 11 )
        goto fail;
    flv_rewrite_amf_be24( c, length, p_flv->start - 10 );
    flv_put_be32( c, length + 11 ); // Last tag size

#if HAVE_AUDIO
    if( a_flv && a_flv->codecid == FLV_CODECID_AAC )
    {
        if( !a_flv->info->extradata )
        {
            x264_cli_log( "flv", X264_LOG_ERROR, "audio codec is AAC but extradata is NULL\n" );
            goto fail;
        }

        uint8_t *extradata;
        uint32_t extradata_size;
        if( a_flv->info->extradata_type == EXTRADATA_TYPE_LIBAVCODEC )
        {
            if( a_flv->info->extradata_size <= 0 )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "invalid AAC extradata size\n" );
                goto fail;
            }
            extradata = a_flv->info->extradata;
            extradata_size = (uint32_t)a_flv->info->extradata_size;
        }
#if HAVE_LSMASH
        else if( a_flv->info->extradata_type == EXTRADATA_TYPE_LSMASH )
        {
            lsmash_codec_specific_t *orig = NULL;
            if( a_flv->info->extradata_size <= 0 ||
                (size_t)a_flv->info->extradata_size % sizeof(lsmash_codec_specific_t *) )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "invalid AAC extradata extension list\n" );
                goto fail;
            }
            uint32_t num_extensions = a_flv->info->extradata_size / sizeof(lsmash_codec_specific_t *);
            for( uint32_t i = 0; i < num_extensions; i++ )
            {
                orig = ((lsmash_codec_specific_t **)a_flv->info->extradata)[i];
                if( orig && orig->type == LSMASH_CODEC_SPECIFIC_DATA_TYPE_MP4SYS_DECODER_CONFIG )
                    break;
            }
            if( !orig )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "no extradata for AAC found!\n" );
                goto fail;
            }
            if( orig->format != LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "unexpected AAC extradata format\n" );
                goto fail;
            }

            conv = lsmash_convert_codec_specific_format( orig, LSMASH_CODEC_SPECIFIC_FORMAT_STRUCTURED );
            if( !conv )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "failed to convert format of AAC specific info.\n" );
                goto fail;
            }

            lsmash_mp4sys_decoder_parameters_t *param = (lsmash_mp4sys_decoder_parameters_t *)conv->data.structured;
            if( !param || param->objectTypeIndication != MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3 )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "unexpected AAC object type\n" );
                goto fail;
            }

            int err = lsmash_get_mp4sys_decoder_specific_info( param, &extradata, &extradata_size );
            if( err )
            {
                x264_cli_log( "flv", X264_LOG_ERROR, "failed to get AAC specific info.\n" );
                goto fail;
            }
        }
#endif
        else
        {
            x264_cli_log( "flv", X264_LOG_ERROR, "unknown extradata type.\n" );
            goto fail;
        }

        if( extradata_size > 0xFFFFFF - 2 || extradata_size > UINT32_MAX - 13 )
        {
            x264_cli_log( "flv", X264_LOG_ERROR, "AAC extradata is too large\n" );
            goto fail;
        }
        flv_put_byte( c, FLV_TAG_TYPE_AUDIO );
        flv_put_be24( c, 2 + extradata_size );
        flv_put_be24( c, 0 );
        flv_put_byte( c, 0 );
        flv_put_be24( c, 0 );

        flv_put_byte( c, a_flv->header );
        flv_put_byte( c, 0 );
        int append_failed = flv_append_data( c, extradata, extradata_size );
        flv_put_be32( c, 11 + 2 + extradata_size );
#if HAVE_LSMASH
        if( conv )
        {
            lsmash_destroy_codec_specific_data( conv );
            conv = NULL;
        }
#endif
        if( append_failed )
            goto fail;
    }
#endif

    if( flv_flush_data( c ) )
        goto fail;

    p_flv->sei = sei;
    p_flv->sei_len = sei_size;
    return sei_size + sps_size + pps_size;

fail:
#if HAVE_AUDIO && HAVE_LSMASH
    if( conv )
        lsmash_destroy_codec_specific_data( conv );
#endif
    free( sei );
    return -1;
}

#if HAVE_AUDIO
static int write_audio( flv_hnd_t *p_flv, int64_t video_dts, int finish )
{
    if( !p_flv || !p_flv->a_flv || !p_flv->c )
        return -1;
    flv_audio_hnd_t *a_flv = p_flv->a_flv;
    flv_buffer *c = p_flv->c;
    if( !a_flv->encoder || !a_flv->info || a_flv->info->samplerate <= 0 )
        return -1;

    int aac = a_flv->codecid == FLV_CODECID_AAC;
    if( a_flv->lastdts == INVALID_DTS )
    {
        if( video_dts > 0 )
        {
            uint64_t skip_ms = (uint64_t)video_dts;
            uint64_t samplerate = (uint64_t)a_flv->info->samplerate;
            uint64_t skip_samples = samplerate && skip_ms > UINT64_MAX / samplerate ? UINT64_MAX : skip_ms * samplerate / 1000;
            x264_audio_encoder_skip_samples( a_flv->encoder, skip_samples );
        }
        a_flv->lastdts = video_dts; // first frame (nonzero if --seek is used)
    }
    audio_packet_t *frame;
    while( a_flv->lastdts <= video_dts || video_dts < 0 )
    {
        if( finish )
            frame = x264_audio_encoder_finish( a_flv->encoder );
        else if( !(frame = x264_audio_encode_frame( a_flv->encoder )) )
        {
            finish = 1;
            continue;
        }

        if( !frame )
            break;

        if( frame->dts < 0 || frame->size < 0 || (frame->size && !frame->data) ||
            frame->size > 0xFFFFFF - 2 || frame->size > INT_MAX - 12 )
        {
            x264_audio_free_frame( a_flv->encoder, frame );
            return -1;
        }
        int64_t audio_dts = x264_from_timebase( frame->dts, frame->info.timebase, 1000 );
        if( audio_dts < 0 || audio_dts > UINT32_MAX )
        {
            x264_audio_free_frame( a_flv->encoder, frame );
            return -1;
        }

        flv_put_byte( c, FLV_TAG_TYPE_AUDIO );
        flv_put_be24( c, (uint32_t)(1 + aac + frame->size) );
        if( flv_write_timestamp( c, audio_dts ) )
        {
            x264_audio_free_frame( a_flv->encoder, frame );
            return -1;
        }
        flv_put_be24( c, 0 );

        flv_put_byte( c, a_flv->header );
        if( aac )
            flv_put_byte( c, 1 );
        if( flv_append_data( c, frame->data, (unsigned)frame->size ) )
        {
            x264_audio_free_frame( a_flv->encoder, frame );
            return -1;
        }

        flv_put_be32( c, (uint32_t)(11 + 1 + aac + frame->size) );

        x264_audio_free_frame( a_flv->encoder, frame );

        CHECK( flv_flush_data( c ) );
        a_flv->lastdts = audio_dts;
    }
    return 0;
}
#endif

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    flv_hnd_t *p_flv = handle;
    if( !p_flv || !p_flv->c || !p_picture || i_size < 0 || (i_size && !p_nalu) ||
        p_flv->i_framenum == INT64_MAX )
        return -1;
    flv_buffer *c = p_flv->c;
    int64_t dts_ts, cts_ts;
    int64_t delay_time = p_flv->i_delay_time;
    int64_t init_delta = p_flv->i_init_delta;

    if( !p_flv->i_framenum )
    {
        if( p_picture->i_dts == INT64_MIN )
            return -1;
        delay_time = -p_picture->i_dts;
        if( !p_flv->b_dts_compress && delay_time )
        {
            int64_t initial_delay_ts, initial_delay_ms;
            if( flv_add_i64( &initial_delay_ts, p_picture->i_pts, delay_time ) ||
                flv_timebase_to_ms( &initial_delay_ms, initial_delay_ts, p_flv->d_timebase ) )
                return -1;
            x264_cli_log( "flv", X264_LOG_INFO, "initial delay %"PRId64" ms\n",
                          initial_delay_ms );
        }
    }

    int64_t dts;
    int64_t cts;
    int64_t offset;

    if( p_flv->b_dts_compress )
    {
        if( p_flv->i_framenum == 1 )
        {
            if( flv_add_i64( &dts_ts, p_picture->i_dts, delay_time ) ||
                flv_timebase_to_ms( &init_delta, dts_ts, p_flv->d_timebase ) )
                return -1;
        }
        if( p_flv->i_framenum > p_flv->i_delay_frames )
        {
            if( flv_timebase_to_ms( &dts, p_picture->i_dts, p_flv->d_timebase ) )
                return -1;
        }
        else
        {
            if( init_delta && p_flv->i_framenum > INT64_MAX / init_delta )
                return -1;
            dts = p_flv->i_framenum * init_delta / (p_flv->i_delay_frames + 1);
        }
        if( flv_timebase_to_ms( &cts, p_picture->i_pts, p_flv->d_timebase ) )
            return -1;
    }
    else
    {
        if( flv_add_i64( &dts_ts, p_picture->i_dts, delay_time ) ||
            flv_add_i64( &cts_ts, p_picture->i_pts, delay_time ) ||
            flv_timebase_to_ms( &dts, dts_ts, p_flv->d_timebase ) ||
            flv_timebase_to_ms( &cts, cts_ts, p_flv->d_timebase ) )
            return -1;
    }
    if( dts < 0 || cts < 0 )
        return -1;
    offset = cts - dts;
    if( offset < -0x800000 || offset > 0x7FFFFF )
        return -1;

    if( p_flv->i_framenum )
    {
        if( p_flv->i_prev_dts == dts )
            x264_cli_log( "flv", X264_LOG_WARNING, "duplicate DTS %"PRId64" generated by rounding\n"
                          "               decoding framerate cannot exceed 1000fps\n", dts );
        if( p_flv->i_prev_cts == cts )
            x264_cli_log( "flv", X264_LOG_WARNING, "duplicate CTS %"PRId64" generated by rounding\n"
                          "               composition framerate cannot exceed 1000fps\n", cts );
    }
    // A new frame - write packet header
    flv_put_byte( c, FLV_TAG_TYPE_VIDEO );
    flv_put_be24( c, 0 ); // calculated later
    if( flv_write_timestamp( c, dts ) )
        return -1;
    flv_put_be24( c, 0 );

    p_flv->start = c->d_cur;
    flv_put_byte( c, (p_picture->b_keyframe ? FLV_FRAME_KEY : FLV_FRAME_INTER) | FLV_CODECID_H264 );
    flv_put_byte( c, 1 ); // AVC NALU
    flv_put_be24( c, (uint32_t)offset );

    int frame_has_sei = p_flv->sei != NULL;
    if( frame_has_sei )
    {
        CHECK( flv_append_data( c, p_flv->sei, (unsigned)p_flv->sei_len ) );
    }
    CHECK( flv_append_data( c, p_nalu, (unsigned)i_size ) );

    unsigned length = c->d_cur - p_flv->start;
    if( length > 0xFFFFFF || length > UINT32_MAX - 11 )
        return -1;
    flv_rewrite_amf_be24( c, length, p_flv->start - 10 );
    flv_put_be32( c, (uint32_t)(11 + length) ); // Last tag size
    CHECK( flv_flush_data( c ) );

#if HAVE_AUDIO
    FAIL_IF_ERR( p_flv->a_flv && write_audio( p_flv, dts, 0 ) < 0, "flv", "error writing audio\n" );
#endif

    if( frame_has_sei )
    {
        free( p_flv->sei );
        p_flv->sei = NULL;
        p_flv->sei_len = 0;
    }
    p_flv->i_delay_time = delay_time;
    p_flv->i_init_delta = init_delta;
    p_flv->i_prev_dts = dts;
    p_flv->i_prev_cts = cts;
    p_flv->i_framenum++;

    return i_size;
}

static int rewrite_amf_double( FILE *fp, uint64_t position, double value )
{
    if( value != value || value < 0.0 || position > LONG_MAX )
        return -1;
    uint64_t x = endian_fix64( flv_dbl2int( value ) );
    return !fseek( fp, (long)position, SEEK_SET ) && fwrite( &x, 8, 1, fp ) == 1 ? 0 : -1;
}

#undef CHECK
#define CHECK(x)\
do {\
    if( (x) < 0 )\
        goto error;\
} while( 0 )

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    int ret = -1;
    flv_hnd_t *p_flv = handle;
    if( !p_flv || !p_flv->c )
        return 0;
    flv_buffer *c = p_flv->c;

#if HAVE_AUDIO
    if( p_flv->a_flv )
    {
        if( write_audio( p_flv, -1, 1 ) < 0 )
        {
            x264_cli_log( "flv", X264_LOG_ERROR, "error flushing audio\n" );
            goto error;
        }
        flv_close_audio( p_flv );
    }
#endif

    CHECK( flv_flush_data( c ) );

    double total_duration = 0;
    /* duration algorithm fails with one frame */
    if( p_flv->i_framenum == 1 )
        total_duration = p_flv->i_fps_num ? (double)p_flv->i_fps_den / (double)p_flv->i_fps_num : 0;
    else if( p_flv->i_framenum > 1 )
    {
        long double duration_ticks = 2.0L * (long double)largest_pts - (long double)second_largest_pts;
        long double duration = duration_ticks * (long double)p_flv->d_timebase;
        total_duration = duration > 0.0L && duration <= DBL_MAX ? (double)duration : 0;
    }

    if( x264_is_regular_file( c->fp ) && total_duration > 0 )
    {
        double framerate;
        int64_t filesize = ftell( c->fp );
        if( filesize < 0 )
            goto error;

        if( p_flv->i_framerate_pos )
        {
            framerate = (double)p_flv->i_framenum / total_duration;
            CHECK( rewrite_amf_double( c->fp, p_flv->i_framerate_pos, framerate ) );
        }

        CHECK( rewrite_amf_double( c->fp, p_flv->i_duration_pos, total_duration ) );
        CHECK( rewrite_amf_double( c->fp, p_flv->i_filesize_pos, (double)filesize ) );
        CHECK( rewrite_amf_double( c->fp, p_flv->i_bitrate_pos, (double)filesize * 8.0 / ( total_duration * 1000 ) ) );
    }
    ret = 0;

error:
    if( c->fp == stdout ? fflush( c->fp ) : fclose( c->fp ) )
        ret = -1;
    free( c->data );
    free( c );
    free( p_flv->sei );

#if HAVE_AUDIO
    flv_close_audio( p_flv );
#endif
    free( p_flv );

    return ret;
}

const cli_output_t flv_output = { open_file, set_param, write_headers, write_frame, close_file };
