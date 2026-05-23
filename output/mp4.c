/*****************************************************************************
 * mp4.c: mp4 muxer
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

#include "output.h"
#include <gpac/isomedia.h>

#define H264_NALU_LENGTH_SIZE 4

typedef struct
{
    GF_ISOFile *p_file;
    GF_AVCConfig *p_config;
    GF_ISOSample *p_sample;
    int i_track;
    uint32_t i_descidx;
    uint64_t i_time_res;
    int64_t i_time_inc;
    int64_t i_delay_time;
    int64_t i_init_delta;
    int i_numframe;
    int i_delay_frames;
    int b_dts_compress;
    int i_dts_compress_multiplier;
    uint32_t i_data_size;
} mp4_hnd_t;

static void free_avc_slot( GF_AVCConfigSlot *p_slot )
{
    if( !p_slot )
        return;

    free( p_slot->data );
    free( p_slot );
}

static GF_AVCConfigSlot *new_avc_slot( uint8_t *data, uint32_t size )
{
    GF_AVCConfigSlot *p_slot;

    if( !data || !size || size > UINT16_MAX )
        return NULL;

    p_slot = malloc( sizeof(GF_AVCConfigSlot) );
    if( !p_slot )
        return NULL;

    p_slot->size = size;
    p_slot->data = malloc( p_slot->size );
    if( !p_slot->data )
    {
        free_avc_slot( p_slot );
        return NULL;
    }

    memcpy( p_slot->data, data, size );
    return p_slot;
}

static int mp4_i64_mul_overflow( int64_t a, int64_t b, int64_t *dst )
{
    if( a < 0 || b < 0 || (b && a > INT64_MAX / b) )
        return -1;
    *dst = a * b;
    return 0;
}

static int mp4_i64_add_overflow( int64_t a, int64_t b, int64_t *dst )
{
    if( (b > 0 && a > INT64_MAX - b) ||
        (b < 0 && a < INT64_MIN - b) )
        return -1;
    *dst = a + b;
    return 0;
}

static int mp4_display_size_to_fixed( uint32_t *dst, double size )
{
    double fixed_size = size * (1u << 16);
    if( fixed_size != fixed_size || fixed_size < 0.0 || fixed_size > UINT32_MAX )
        return -1;
    *dst = (uint32_t)fixed_size;
    return 0;
}

static void recompute_bitrate_mp4( GF_ISOFile *p_file, int i_track )
{
    u32 count, di, timescale, time_wnd, rate;
    u64 offset;
    Double br;
    GF_ESD *esd;

    esd = gf_isom_get_esd( p_file, i_track, 1 );
    if( !esd )
        return;

    esd->decoderConfig->avgBitrate = 0;
    esd->decoderConfig->maxBitrate = 0;
    rate = time_wnd = 0;

    timescale = gf_isom_get_media_timescale( p_file, i_track );
    count = gf_isom_get_sample_count( p_file, i_track );
    for( u32 i = 0; i < count; i++ )
    {
        GF_ISOSample *samp = gf_isom_get_sample_info( p_file, i_track, i+1, &di, &offset );
        if( !samp )
        {
            x264_cli_log( "mp4", X264_LOG_ERROR, "failure reading back frame %u\n", i );
            break;
        }

        if( esd->decoderConfig->bufferSizeDB < samp->dataLength )
            esd->decoderConfig->bufferSizeDB = samp->dataLength;

        esd->decoderConfig->avgBitrate += samp->dataLength;
        rate += samp->dataLength;
        if( samp->DTS > time_wnd + timescale )
        {
            if( rate > esd->decoderConfig->maxBitrate )
                esd->decoderConfig->maxBitrate = rate;
            time_wnd = samp->DTS;
            rate = 0;
        }

        gf_isom_sample_del( &samp );
    }

    br = (Double)(s64)gf_isom_get_media_duration( p_file, i_track );
    br /= timescale;
    esd->decoderConfig->avgBitrate = (u32)(esd->decoderConfig->avgBitrate / br);
    /*move to bps*/
    esd->decoderConfig->avgBitrate *= 8;
    esd->decoderConfig->maxBitrate *= 8;

    gf_isom_change_mpeg4_description( p_file, i_track, 1, esd );
    gf_odf_desc_del( (GF_Descriptor*)esd );
}

static int close_file( hnd_t handle, int64_t largest_pts, int64_t second_largest_pts )
{
    mp4_hnd_t *p_mp4 = handle;

    if( !p_mp4 )
        return 0;

    if( p_mp4->p_config )
        gf_odf_avc_cfg_del( p_mp4->p_config );

    if( p_mp4->p_sample )
    {
        if( p_mp4->p_sample->data )
            free( p_mp4->p_sample->data );

        p_mp4->p_sample->dataLength = 0;
        gf_isom_sample_del( &p_mp4->p_sample );
    }

    if( p_mp4->p_file )
    {
        if( p_mp4->i_track && p_mp4->i_numframe )
        {
            int64_t pts_delta;
            int64_t mdhd_pts;
            int64_t mdhd_duration_i64;
            int64_t last_duration_i64;
            int valid_duration = largest_pts >= second_largest_pts &&
                                 second_largest_pts >= 0 &&
                                 (pts_delta = largest_pts - second_largest_pts) >= 0 &&
                                 largest_pts <= INT64_MAX - pts_delta &&
                                 !mp4_i64_mul_overflow( pts_delta, p_mp4->i_time_inc, &last_duration_i64 ) &&
                                 (mdhd_pts = largest_pts + pts_delta) >= 0 &&
                                 !mp4_i64_mul_overflow( mdhd_pts, p_mp4->i_time_inc, &mdhd_duration_i64 );
            /* The mdhd duration is defined as CTS[final] - CTS[0] + duration of last frame.
             * The mdhd duration (in seconds) should be able to be longer than the tkhd duration since the track is managed by edts.
             * So, if mdhd duration is equal to the last DTS or less, we give the last composition time delta to the last sample duration.
             * And then, the mdhd duration is updated, but it time-wise doesn't give the actual duration.
             * The tkhd duration is the actual track duration. */
            uint64_t mdhd_duration = valid_duration ? (uint64_t)mdhd_duration_i64 : 0;
            if( valid_duration && mdhd_duration != gf_isom_get_media_duration( p_mp4->p_file, p_mp4->i_track ) )
            {
                uint64_t last_dts = gf_isom_get_sample_dts( p_mp4->p_file, p_mp4->i_track, p_mp4->i_numframe );
                uint64_t last_duration = mdhd_duration > last_dts ? mdhd_duration - last_dts : (uint64_t)last_duration_i64;
                if( last_duration <= UINT32_MAX )
                    gf_isom_set_last_sample_duration( p_mp4->p_file, p_mp4->i_track, (uint32_t)last_duration );
            }

            /* Write an Edit Box if the first CTS offset is positive.
             * A media_time is given by not the mvhd timescale but rather the mdhd timescale.
             * The reason is that an Edit Box maps the presentation time-line to the media time-line.
             * Any demuxers should follow the Edit Box if it exists. */
            GF_ISOSample *sample = gf_isom_get_sample_info( p_mp4->p_file, p_mp4->i_track, 1, NULL, NULL );
            if( valid_duration && sample && sample->CTS_Offset > 0 )
            {
                uint32_t mvhd_timescale = gf_isom_get_timescale( p_mp4->p_file );
                long double tkhd_duration_ld = (long double)mdhd_duration * (long double)mvhd_timescale / p_mp4->i_time_res;
                uint64_t tkhd_duration = tkhd_duration_ld > (long double)UINT64_MAX ? UINT64_MAX : (uint64_t)tkhd_duration_ld;
#if GPAC_VERSION_MAJOR > 8
                gf_isom_append_edit( p_mp4->p_file, p_mp4->i_track, tkhd_duration, sample->CTS_Offset, GF_ISOM_EDIT_NORMAL );
#else
                gf_isom_append_edit_segment( p_mp4->p_file, p_mp4->i_track, tkhd_duration, sample->CTS_Offset, GF_ISOM_EDIT_NORMAL );
#endif
            }
            gf_isom_sample_del( &sample );

            recompute_bitrate_mp4( p_mp4->p_file, p_mp4->i_track );
        }
        gf_isom_set_pl_indication( p_mp4->p_file, GF_ISOM_PL_VISUAL, 0x15 );
        gf_isom_set_storage_mode( p_mp4->p_file, GF_ISOM_STORE_FLAT );
        gf_isom_close( p_mp4->p_file );
    }

    free( p_mp4 );

    return 0;
}

static int open_file( char *psz_filename, hnd_t *p_handle, cli_output_opt_t *opt, hnd_t audio_filters, char *audio_enc, char *audio_params )
{
    if( !psz_filename || !p_handle || !opt )
        return -1;
    *p_handle = NULL;

    FILE *fh = x264_fopen( psz_filename, "w" );
    if( !fh )
        return -1;
    int b_regular = x264_is_regular_file( fh );
    FAIL_IF_ERR( fclose( fh ), "mp4", "failed to close output probe file `%s'\n", psz_filename );
    FAIL_IF_ERR( !b_regular, "mp4", "MP4 output is incompatible with non-regular file `%s'\n", psz_filename );

    mp4_hnd_t *p_mp4 = calloc( 1, sizeof(mp4_hnd_t) );
    if( !p_mp4 )
        return -1;
	
    FAIL_IF_ERR( audio_enc && ( strcmp( audio_enc, "none" ) && strcmp( audio_enc, "auto" ) ), "mp4",
                 "audio is not yet supported on this muxer\n" );

    p_mp4->p_file = gf_isom_open( psz_filename, GF_ISOM_OPEN_WRITE, NULL );
    p_mp4->b_dts_compress = opt->use_dts_compress;
    if( !p_mp4->p_file )
    {
        close_file( p_mp4, 0, 0 );
        return -1;
    }

    if( !(p_mp4->p_sample = gf_isom_sample_new()) )
    {
        close_file( p_mp4, 0, 0 );
        return -1;
    }

    gf_isom_set_brand_info( p_mp4->p_file, GF_ISOM_BRAND_AVC1, 0 );

    *p_handle = p_mp4;

    return 0;
}

static int set_param( hnd_t handle, x264_param_t *p_param )
{
    mp4_hnd_t *p_mp4 = handle;
    uint64_t data_size;
    uint32_t data_size_u32;
    int delay_frames;
    int dts_compress_multiplier;
    uint64_t time_res;
    int64_t time_inc;
    uint32_t display_width = 0;
    uint32_t display_height = 0;

    FAIL_IF_ERR( !p_mp4 || !p_mp4->p_file || !p_mp4->p_sample || !p_param, "mp4", "invalid muxer state\n" );
    FAIL_IF_ERR( p_param->i_width <= 0 || p_param->i_height <= 0, "mp4", "invalid video dimensions\n" );
    FAIL_IF_ERR( p_param->i_timebase_num <= 0 || p_param->i_timebase_den <= 0, "mp4", "invalid timebase\n" );
    FAIL_IF_ERR( p_param->vui.i_sar_width < 0 || p_param->vui.i_sar_height < 0, "mp4", "invalid sample aspect ratio\n" );
    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height )
    {
        double sar = (double)p_param->vui.i_sar_width / p_param->vui.i_sar_height;
        double width = sar > 1.0 ? (double)p_param->i_width * sar : p_param->i_width;
        double height = sar > 1.0 ? p_param->i_height : (double)p_param->i_height / sar;
        FAIL_IF_ERR( mp4_display_size_to_fixed( &display_width, width ) ||
                     mp4_display_size_to_fixed( &display_height, height ),
                     "mp4", "display size is out of range\n" );
    }

    delay_frames = p_param->i_bframe ? (p_param->i_bframe_pyramid ? 2 : 1) : 0;
    dts_compress_multiplier = p_mp4->b_dts_compress * delay_frames + 1;

    FAIL_IF_ERR( (uint64_t)p_param->i_timebase_den > UINT64_MAX / (uint64_t)dts_compress_multiplier ||
                 (uint64_t)p_param->i_timebase_num > (uint64_t)INT64_MAX / (uint64_t)dts_compress_multiplier,
                 "mp4", "MP4 timebase exceeds maximum\n" );
    time_res = (uint64_t)p_param->i_timebase_den * dts_compress_multiplier;
    time_inc = (uint64_t)p_param->i_timebase_num * dts_compress_multiplier;
    FAIL_IF_ERR( time_res > UINT32_MAX, "mp4", "MP4 media timescale %"PRIu64" exceeds maximum\n", time_res );

    data_size = (uint64_t)p_param->i_width * p_param->i_height * 3 / 2;
    FAIL_IF_ERR( data_size > UINT32_MAX, "mp4", "MP4 sample buffer size %"PRIu64" exceeds maximum\n", data_size );
    data_size_u32 = (uint32_t)data_size;
    p_mp4->p_sample->data = malloc( data_size_u32 );
    if( !p_mp4->p_sample->data )
        return -1;
    p_mp4->i_data_size = data_size_u32;

    p_mp4->i_delay_frames = delay_frames;
    p_mp4->i_dts_compress_multiplier = dts_compress_multiplier;
    p_mp4->i_time_res = time_res;
    p_mp4->i_time_inc = time_inc;

    p_mp4->i_track = gf_isom_new_track( p_mp4->p_file, 0, GF_ISOM_MEDIA_VISUAL,
                                        p_mp4->i_time_res );
    FAIL_IF_ERR( !p_mp4->i_track, "mp4", "failed to create video track\n" );

    p_mp4->p_config = gf_odf_avc_cfg_new();
    FAIL_IF_ERR( !p_mp4->p_config, "mp4", "failed to allocate AVC config\n" );
    p_mp4->p_config->nal_unit_size = H264_NALU_LENGTH_SIZE;
    FAIL_IF_ERR( gf_isom_avc_config_new( p_mp4->p_file, p_mp4->i_track, p_mp4->p_config,
                                         NULL, NULL, &p_mp4->i_descidx ),
                 "mp4", "failed to create AVC config\n" );

    gf_isom_set_track_enabled( p_mp4->p_file, p_mp4->i_track, 1 );

    gf_isom_set_visual_info( p_mp4->p_file, p_mp4->i_track, p_mp4->i_descidx,
                             p_param->i_width, p_param->i_height );

    if( p_param->vui.i_sar_width && p_param->vui.i_sar_height )
    {
        gf_isom_set_pixel_aspect_ratio( p_mp4->p_file, p_mp4->i_track, p_mp4->i_descidx, p_param->vui.i_sar_width, p_param->vui.i_sar_height, 0 );
        gf_isom_set_track_layout_info( p_mp4->p_file, p_mp4->i_track, display_width, display_height, 0, 0, 0 );
    }

    return 0;
}

static int check_buffer( mp4_hnd_t *p_mp4, uint32_t needed_size )
{
    if( !p_mp4 || !p_mp4->p_sample )
        return -1;

    if( needed_size > p_mp4->i_data_size )
    {
        void *ptr = realloc( p_mp4->p_sample->data, needed_size );
        if( !ptr )
            return -1;
        p_mp4->p_sample->data = ptr;
        p_mp4->i_data_size = needed_size;
    }
    return 0;
}

static int append_sample_data( mp4_hnd_t *p_mp4, uint8_t *data, uint32_t size )
{
    uint32_t data_length;
    uint32_t needed_size;

    if( !p_mp4 || !p_mp4->p_sample || (size && !data) )
        return -1;

    data_length = p_mp4->p_sample->dataLength;
    if( data_length > p_mp4->i_data_size || size > UINT32_MAX - data_length )
        return -1;

    needed_size = data_length + size;
    if( check_buffer( p_mp4, needed_size ) )
        return -1;

    if( size )
        memcpy( p_mp4->p_sample->data + data_length, data, size );
    p_mp4->p_sample->dataLength = needed_size;

    return 0;
}

static int write_headers( hnd_t handle, x264_nal_t *p_nal )
{
    mp4_hnd_t *p_mp4 = handle;
    GF_AVCConfigSlot *p_slot;

    if( !p_mp4 || !p_mp4->p_file || !p_mp4->p_config || !p_mp4->p_sample || !p_nal ||
        p_nal[0].i_payload < H264_NALU_LENGTH_SIZE + 4 ||
        p_nal[1].i_payload <= H264_NALU_LENGTH_SIZE ||
        p_nal[2].i_payload < 0 ||
        !p_nal[0].p_payload || !p_nal[1].p_payload || (p_nal[2].i_payload && !p_nal[2].p_payload) )
        return -1;

    uint32_t sps_size = (uint32_t)(p_nal[0].i_payload - H264_NALU_LENGTH_SIZE);
    uint32_t pps_size = (uint32_t)(p_nal[1].i_payload - H264_NALU_LENGTH_SIZE);
    uint32_t sei_size = (uint32_t)p_nal[2].i_payload;

    if( sps_size > UINT16_MAX || pps_size > UINT16_MAX )
        return -1;
    if( sps_size > (uint32_t)INT_MAX - pps_size || sei_size > (uint32_t)INT_MAX - sps_size - pps_size )
        return -1;

    uint8_t *sps = p_nal[0].p_payload + H264_NALU_LENGTH_SIZE;
    uint8_t *pps = p_nal[1].p_payload + H264_NALU_LENGTH_SIZE;
    uint8_t *sei = p_nal[2].p_payload;

    // SPS

    p_mp4->p_config->configurationVersion = 1;
    p_mp4->p_config->AVCProfileIndication = sps[1];
    p_mp4->p_config->profile_compatibility = sps[2];
    p_mp4->p_config->AVCLevelIndication = sps[3];
    p_slot = new_avc_slot( sps, sps_size );
    if( !p_slot )
        return -1;
    if( gf_list_add( p_mp4->p_config->sequenceParameterSets, p_slot ) )
    {
        free_avc_slot( p_slot );
        return -1;
    }

    // PPS

    p_slot = new_avc_slot( pps, pps_size );
    if( !p_slot )
        return -1;
    if( gf_list_add( p_mp4->p_config->pictureParameterSets, p_slot ) )
    {
        free_avc_slot( p_slot );
        return -1;
    }
    if( gf_isom_avc_config_update( p_mp4->p_file, p_mp4->i_track, p_mp4->i_descidx, p_mp4->p_config ) )
        return -1;

    // SEI

    if( append_sample_data( p_mp4, sei, sei_size ) )
        return -1;

    return (int)(sei_size + sps_size + pps_size);
}

static int write_frame( hnd_t handle, uint8_t *p_nalu, int i_size, x264_picture_t *p_picture )
{
    mp4_hnd_t *p_mp4 = handle;
    int64_t dts;
    int64_t cts;
    int64_t delay_time;
    int64_t init_delta;

    if( !p_mp4 || !p_mp4->p_file || !p_mp4->p_sample || !p_picture ||
        i_size < 0 || (i_size && !p_nalu) || p_mp4->i_numframe == INT_MAX )
        return -1;

    delay_time = p_mp4->i_delay_time;
    init_delta = p_mp4->i_init_delta;

    if( !p_mp4->i_numframe )
    {
        if( p_picture->i_dts == INT64_MIN )
            return -1;
        delay_time = p_picture->i_dts * -1;
    }

    if( p_mp4->b_dts_compress )
    {
        if( p_mp4->i_numframe == 1 )
        {
            int64_t init_ts;
            if( mp4_i64_add_overflow( p_picture->i_dts, delay_time, &init_ts ) ||
                mp4_i64_mul_overflow( init_ts, p_mp4->i_time_inc, &init_delta ) )
                return -1;
        }
        dts = p_mp4->i_numframe > p_mp4->i_delay_frames
            ? 0
            : p_mp4->i_numframe * (init_delta / p_mp4->i_dts_compress_multiplier);
        if( p_mp4->i_numframe > p_mp4->i_delay_frames &&
            mp4_i64_mul_overflow( p_picture->i_dts, p_mp4->i_time_inc, &dts ) )
            return -1;
        if( mp4_i64_mul_overflow( p_picture->i_pts, p_mp4->i_time_inc, &cts ) )
            return -1;
    }
    else
    {
        int64_t dts_ts, cts_ts;
        if( mp4_i64_add_overflow( p_picture->i_dts, delay_time, &dts_ts ) ||
            mp4_i64_add_overflow( p_picture->i_pts, delay_time, &cts_ts ) ||
            mp4_i64_mul_overflow( dts_ts, p_mp4->i_time_inc, &dts ) ||
            mp4_i64_mul_overflow( cts_ts, p_mp4->i_time_inc, &cts ) )
            return -1;
    }
    if( cts < dts || cts - dts > INT32_MAX )
        return -1;

    if( append_sample_data( p_mp4, p_nalu, (uint32_t)i_size ) )
        return -1;

    p_mp4->p_sample->IsRAP = p_picture->b_keyframe;
    p_mp4->p_sample->DTS = dts;
    p_mp4->p_sample->CTS_Offset = (uint32_t)(cts - dts);
    if( gf_isom_add_sample( p_mp4->p_file, p_mp4->i_track, p_mp4->i_descidx, p_mp4->p_sample ) )
    {
        p_mp4->p_sample->dataLength = 0;
        return -1;
    }

    p_mp4->p_sample->dataLength = 0;
    p_mp4->i_delay_time = delay_time;
    p_mp4->i_init_delta = init_delta;
    p_mp4->i_numframe++;

    return i_size;
}

const cli_output_t mp4_output = { open_file, set_param, write_headers, write_frame, close_file };
