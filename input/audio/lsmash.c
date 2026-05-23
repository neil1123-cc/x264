#include <stdio.h>
#include <inttypes.h>
#include <limits.h>

#include <lsmash.h>
#if defined(__has_include)
#if __has_include(<lsmash_importer.h>)
#include <lsmash_importer.h>
#define X264_HAVE_LSMASH_IMPORTER_H 1
#endif
#endif

#ifndef X264_HAVE_LSMASH_IMPORTER_H
/* Some liblsmash packages export importer symbols without installing lsmash_importer.h. */
typedef struct importer_tag importer_t;
importer_t *lsmash_importer_open( lsmash_root_t *root, const char *identifier, const char *format );
void lsmash_importer_close( importer_t *importer );
int lsmash_importer_get_access_unit( importer_t *importer, uint32_t track_number, lsmash_sample_t **p_sample );
uint32_t lsmash_importer_get_last_delta( importer_t *importer, uint32_t track_number );
int lsmash_importer_construct_timeline( importer_t *importer, uint32_t track_number );
uint32_t lsmash_importer_get_track_count( importer_t *importer );
lsmash_summary_t *lsmash_duplicate_summary( importer_t *importer, uint32_t track_number );
#endif

#include "audio/encoders.h"
#include "filters/audio/internal.h"

typedef struct lsmash_source_t
{
    AUDIO_FILTER_COMMON

    importer_t* importer;
    lsmash_audio_summary_t* summary;
    lsmash_root_t* root;
    uint32_t track;
    lsmash_sample_t *pending_sample;

    int copy_error;
} lsmash_source_t;

enum
{
    /* Public lsmash_importer.h hides importer_status unless building importer internals. */
    LSMASH_IMPORTER_STATUS_CHANGE = 1,
    LSMASH_IMPORTER_STATUS_EOF    = 2,
};

const audio_filter_t audio_filter_lsmash;

static int lsmash_parse_audio_track( const char *trackstr, int *track )
{
    int parsed_track;

    if( !trackstr || !strcmp( trackstr, "any" ) )
    {
        *track = TRACK_ANY;
        return 0;
    }

    if( x264_otoi_checked( trackstr, &parsed_track ) || parsed_track < 0 )
        return -1;

    *track = parsed_track;
    return 0;
}

static void lsmash_free_extradata( audio_info_t *info )
{
    if( !info || !info->extradata )
        return;
    if( info->extradata_type == EXTRADATA_TYPE_LSMASH && info->extradata_size > 0 &&
        (size_t)info->extradata_size % sizeof(lsmash_codec_specific_t *) == 0 )
    {
        lsmash_codec_specific_t **extradata = (lsmash_codec_specific_t **)info->extradata;
        size_t num_extensions = (size_t)info->extradata_size / sizeof(lsmash_codec_specific_t *);
        for( size_t i = 0; i < num_extensions; i++ )
            if( extradata[i] )
                lsmash_destroy_codec_specific_data( extradata[i] );
    }
    free( info->extradata );
    info->extradata = NULL;
    info->extradata_size = 0;
}

static int lsmash_init( hnd_t *handle, const char *opt_str )
{
    if( !handle || *handle || !opt_str ) // This must be the first filter
        return -1;
    static const char * const optlist[] = { "filename", "track", NULL };
    char **opts = x264_split_options( opt_str, optlist );

    if( !opts )
        return -1;

    char *filename = x264_get_option( "filename", opts );

    if( !filename )
    {
        x264_cli_log( "lsmash", X264_LOG_ERROR, "no filename given.\n" );
        goto fail2;
    }

    int track;
    if( lsmash_parse_audio_track( x264_get_option( "track", opts ), &track ) )
    {
        x264_cli_log( "lsmash", X264_LOG_ERROR, "no valid track requested ('any', 0 or a positive integer)\n" );
        goto fail2;
    }
    if( !strcmp( filename, "-" ) )
    {
        x264_cli_log( "lsmash", X264_LOG_ERROR, "pipe input is not supported.\n" );
        goto fail2;
    }

    INIT_FILTER_STRUCT( audio_filter_lsmash, lsmash_source_t );
	
	h->root = lsmash_create_root();
	
	if( !h->root )
        goto error;
	
    h->importer = lsmash_importer_open( h->root, filename, "auto" );
    if( !h->importer )
        goto error;

    if( track >= 0 )
    {
        h->track = (uint32_t)track + 1;
        lsmash_summary_t *summary = lsmash_duplicate_summary( h->importer, h->track );
        if( summary && summary->summary_type == LSMASH_SUMMARY_TYPE_AUDIO )
            h->summary = (lsmash_audio_summary_t *)summary;
        else
        {
            if( summary )
                lsmash_cleanup_summary( summary );
            AF_LOG_ERR( h, "requested track %d is unavailable or is not an audio track\n", track );
            goto error;
        }
    }
    else
    {
        uint32_t track_count = lsmash_importer_get_track_count( h->importer );
        for( uint32_t track_number = 1; track_number <= track_count; track_number++ )
        {
            lsmash_summary_t *summary = lsmash_duplicate_summary( h->importer, track_number );
            if( !summary )
                continue;
            if( summary->summary_type == LSMASH_SUMMARY_TYPE_AUDIO )
            {
                h->track = track_number;
                h->summary = (lsmash_audio_summary_t *)summary;
                break;
            }
            lsmash_cleanup_summary( summary );
        }
        if( !h->summary )
        {
            AF_LOG_ERR( h, "could not find any audio track\n" );
            goto error;
        }
    }

    if( lsmash_importer_construct_timeline( h->importer, h->track ) < 0 )
    {
        AF_LOG_ERR( h, "failed to construct importer timeline for audio track.\n" );
        goto error;
    }

    memset( &h->info, 0, sizeof(audio_info_t) );

    if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_MP4A_AUDIO ) )
    {
        /* Investigate what CODEC is used. */
        lsmash_mp4sys_object_type_indication objectTypeIndication = lsmash_mp4sys_get_object_type_indication( (lsmash_summary_t *)h->summary );
        switch( objectTypeIndication )
        {
            case MP4SYS_OBJECT_TYPE_Audio_ISO_14496_3:
                if( h->summary->aot == MP4A_AUDIO_OBJECT_TYPE_ALS )
                    h->info.codec_name = "als";
                else
                    h->info.codec_name = "aac";
                break;
            case MP4SYS_OBJECT_TYPE_Audio_ISO_11172_3:
            case MP4SYS_OBJECT_TYPE_Audio_ISO_13818_3:
                if( h->summary->aot == MP4A_AUDIO_OBJECT_TYPE_Layer_3 )
                    h->info.codec_name = "mp3";
                else if( h->summary->aot == MP4A_AUDIO_OBJECT_TYPE_Layer_2 )
                    h->info.codec_name = "mp2";
                else
                    h->info.codec_name = "mp1";
                break;
            default :
                AF_LOG_ERR( h, "unknown audio stream type.\n" );
                goto error;
                break;
        }
    }
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_SAMR_AUDIO ) )
        h->info.codec_name = "amrnb";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_SAWB_AUDIO ) )
        h->info.codec_name = "amrwb";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_AC_3_AUDIO ) )
        h->info.codec_name = "ac3";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_EC_3_AUDIO ) )
        h->info.codec_name = "eac3";
    else if( lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSC_AUDIO )
          || lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSE_AUDIO )
          || lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSH_AUDIO )
          || lsmash_check_codec_type_identical( h->summary->sample_type, ISOM_CODEC_TYPE_DTSL_AUDIO ) )
    {
        h->info.codec_name = "dca";
        int64_t dts_info_alloc_size = sizeof( audio_dts_info_t );
        audio_dts_info_t *dts_info = malloc( dts_info_alloc_size );
        if( !dts_info )
            goto error;
        dts_info->coding_name = h->summary->sample_type;
        h->info.opaque = dts_info;
    }
    else
    {
        AF_LOG_ERR( h, "unknown audio stream type.\n" );
        goto error;
    }

    uint32_t chansize = h->summary->sample_size >> 3;
    if( !h->summary->frequency || !h->summary->channels || !h->summary->samples_in_frame ||
        h->summary->frequency > (uint32_t)INT_MAX ||
        h->summary->channels > (uint32_t)INT_MAX ||
        h->summary->samples_in_frame > (uint32_t)INT_MAX ||
        h->summary->sample_size > (uint32_t)INT_MAX )
    {
        AF_LOG_ERR( h, "audio stream parameters are too large.\n" );
        goto error;
    }
    uint64_t samplesize64 = (uint64_t)chansize * h->summary->channels;
    if( chansize > (uint32_t)(INT_MAX / 8) || samplesize64 > (uint64_t)INT_MAX )
    {
        AF_LOG_ERR( h, "audio stream parameters are too large.\n" );
        goto error;
    }
    int channel_size = (int)chansize;
    int samplesize = (int)samplesize64;
    int samplerate = (int)h->summary->frequency;
    int channels = (int)h->summary->channels;
    int frame_length = (int)h->summary->samples_in_frame;
    int sample_size = (int)h->summary->sample_size;
    if( h->summary->samples_in_frame && (size_t)samplesize > SIZE_MAX / h->summary->samples_in_frame )
    {
        AF_LOG_ERR( h, "audio stream parameters are too large.\n" );
        goto error;
    }
    size_t framesize = (size_t)h->summary->samples_in_frame * (size_t)samplesize;

    h->info.samplerate     = samplerate;
    h->info.channels       = channels;
    h->info.framelen       = frame_length;
    h->info.chansize       = channel_size;
    h->info.samplesize     = samplesize;
    h->info.framesize      = framesize;
    h->info.depth          = sample_size;
    h->info.timebase       = (timebase_t){ 1, h->summary->frequency };
    h->info.last_delta     = h->summary->samples_in_frame;
    h->info.priming        = 0;     /* No one can detect this. */

    uint32_t num_extensions = lsmash_count_codec_specific_data( (lsmash_summary_t *)h->summary );
    if( num_extensions )
    {
        if( num_extensions > (uint32_t)((size_t)INT_MAX / sizeof(lsmash_codec_specific_t *)) )
        {
            AF_LOG_ERR( h, "too much audio extradata.\n" );
            goto error;
        }
        size_t extradata_size = (size_t)num_extensions * sizeof(lsmash_codec_specific_t *);
        int extradata_size_int = (int)extradata_size;
        h->info.extradata_type = EXTRADATA_TYPE_LSMASH;
        int64_t extradata_alloc_size = (int64_t)num_extensions * sizeof(lsmash_codec_specific_t *);
        h->info.extradata = calloc( 1, extradata_alloc_size );
        if( !h->info.extradata )
        {
            AF_LOG_ERR( h, "malloc failed!\n" );
            goto error;
        }
        h->info.extradata_size = extradata_size_int;
        for( uint32_t i = 0; i < num_extensions; i++ )
        {
            lsmash_codec_specific_t **extradata = (lsmash_codec_specific_t **)h->info.extradata;
            lsmash_codec_specific_t *src = lsmash_get_codec_specific_data( (lsmash_summary_t *)h->summary, i + 1 );
            if( !src )
            {
                AF_LOG_ERR( h, "invalid audio extradata.\n" );
                goto error;
            }
            lsmash_codec_specific_t *dst = lsmash_convert_codec_specific_format( src, LSMASH_CODEC_SPECIFIC_FORMAT_UNSTRUCTURED );
            if( !dst )
            {
                AF_LOG_ERR( h, "invalid audio extradata.\n" );
                goto error;
            }
            extradata[i] = dst;
        }
    }

    free( opts );

    x264_cli_log( "lsmash", X264_LOG_INFO, "opened L-SMASH importer for %s audio stream copy.\n", h->info.codec_name );

    return 0;

error:
    AF_LOG_ERR( h, "error opening the L-SMASH importer.\n" );
fail:
    if( h )
    {
        lsmash_free_extradata( &h->info );
        free( h->info.opaque );
        h->info.opaque = NULL;
        if( h->summary )
        {
            lsmash_cleanup_summary( (lsmash_summary_t *)h->summary );
            h->summary = NULL;
        }
        if( h->importer )
        {
            lsmash_importer_close( h->importer );
            h->importer = NULL;
        }
        if ( h->root )
        {
            lsmash_destroy_root( h->root );
            h->root = NULL;
        }
        free( h );
    }
    *handle = NULL;
fail2:
    free( opts );
    return -1;
}

static void free_packet( hnd_t handle, audio_packet_t *pkt )
{
    if( !pkt )
        return;
    pkt->owner = NULL;
    x264_af_free_packet( pkt );
}

static void lsmash_close( hnd_t handle )
{
    if( !handle )
        return;
    lsmash_source_t *h = handle;

    if( h->pending_sample )
        lsmash_delete_sample( h->pending_sample );
    if( h->summary )
        lsmash_cleanup_summary( (lsmash_summary_t *)h->summary );
    if( h->importer )
        lsmash_importer_close( h->importer );
    lsmash_free_extradata( &h->info );
    if( h->info.opaque )
		free( h->info.opaque );
	if ( h->root )
	    lsmash_destroy_root( h->root );
    if( h )
        free( h );
}

static lsmash_sample_t *lsmash_fetch_access_unit( lsmash_source_t *h )
{
    if( !h || !h->importer || !h->track )
        return NULL;

    lsmash_sample_t *sample = NULL;
    int ret = lsmash_importer_get_access_unit( h->importer, h->track, &sample );

    if( ret < 0 )
    {
        AF_LOG_ERR( h, "failed to retrieve access unit from importer.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( ret == LSMASH_IMPORTER_STATUS_CHANGE )
    {
        AF_LOG_ERR( h, "stream property changes are not supported in L-SMASH copy mode.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( ret == LSMASH_IMPORTER_STATUS_EOF )
    {
        h->info.last_delta = lsmash_importer_get_last_delta( h->importer, h->track );
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( ret > 0 )
    {
        AF_LOG_ERR( h, "importer returned an unsupported access unit status.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( !sample )
    {
        AF_LOG_ERR( h, "importer returned no access unit.\n" );
        h->copy_error = 1;
        return NULL;
    }
    if( !sample->length )
    {
        h->info.last_delta = lsmash_importer_get_last_delta( h->importer, h->track );
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( !sample->data )
    {
        AF_LOG_ERR( h, "importer returned a non-empty access unit without payload data.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( sample->length > (uint32_t)INT_MAX )
    {
        AF_LOG_ERR( h, "audio access unit payload is too large.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        return NULL;
    }
    if( sample->dts > (uint64_t)INT64_MAX )
    {
        AF_LOG_ERR( h, "audio timestamp state overflowed.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        return NULL;
    }

    return sample;
}

static audio_packet_t *get_next_au( hnd_t handle )
{
    lsmash_source_t *h = handle;
    if( !h || !h->summary || !h->importer ||
        !h->track || h->info.channels <= 0 || h->info.framelen <= 0 )
        return NULL;
    if( h->copy_error )
        return NULL;
    int64_t packet_alloc_size = sizeof( audio_packet_t );
    audio_packet_t *out = calloc( 1, packet_alloc_size );

    if( !out )
    {
        AF_LOG_ERR( h, "malloc failed while allocating output packet.\n" );
        h->copy_error = 1;
        return NULL;
    }
    unsigned packet_channels = (unsigned)h->info.channels;
    out->info        = h->info;
    out->channels    = packet_channels;
    lsmash_sample_t *sample = h->pending_sample;
    h->pending_sample = NULL;
    if( !sample )
        sample = lsmash_fetch_access_unit( h );
    if( !sample )
    {
        x264_af_free_packet( out );
        return NULL;
    }
    int sample_size = (int)sample->length;
    out->size = sample_size;
    out->data = sample->data;
    lsmash_sample_t *next = lsmash_fetch_access_unit( h );
    uint32_t packet_samplecount = 0;
    if( next )
    {
        if( next->dts <= sample->dts || next->dts - sample->dts > UINT32_MAX )
        {
            AF_LOG_ERR( h, "invalid audio sample timeline.\n" );
            h->copy_error = 1;
            lsmash_delete_sample( next );
            lsmash_delete_sample( sample );
            x264_af_free_packet( out );
            return NULL;
        }
        packet_samplecount = (uint32_t)(next->dts - sample->dts);
        h->pending_sample = next;
    }
    else
    {
        if( h->copy_error )
        {
            lsmash_delete_sample( sample );
            x264_af_free_packet( out );
            return NULL;
        }
        packet_samplecount = h->info.last_delta;
    }
    if( !packet_samplecount )
    {
        AF_LOG_ERR( h, "audio sample duration is invalid.\n" );
        h->copy_error = 1;
        lsmash_delete_sample( sample );
        x264_af_free_packet( out );
        return NULL;
    }
    out->samplecount = packet_samplecount;
    out->info.last_delta = packet_samplecount;
    h->info.last_delta = packet_samplecount;
    out->dts = (int64_t)sample->dts;
    /* Keep the AU payload but free the L-SMASH sample wrapper. */
    sample->data = NULL;
    lsmash_delete_sample( sample );

    return out;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    lsmash_source_t *h = handle;
    if( !h )
        return;

    if( h->info.framelen <= 0 )
        return;
    uint64_t framelen = (uint64_t)h->info.framelen;
    if( samplecount < framelen )
        return; // Nothing to do due to low accuracy
    uint64_t skip_limit = samplecount - framelen;
    uint64_t samples_skipped = 0;
    while( samples_skipped <= skip_limit )
    {
        audio_packet_t *pkt = get_next_au( h );
        if( !pkt )
            break;
        if( !pkt->samplecount || pkt->samplecount > UINT64_MAX - samples_skipped )
        {
            free_packet( h, pkt );
            break;
        }
        samples_skipped += pkt->samplecount;
        free_packet( h, pkt );
    }
}

static audio_info_t *get_info( hnd_t handle )
{
    audio_hnd_t *h = handle;
    if( !h )
        return NULL;
    return &h->info;
}

static hnd_t copy_init( hnd_t filter_chain, const char *opts )
{
    if( !filter_chain )
        return NULL;
    audio_hnd_t *chain = filter_chain;
    if( chain->self == &audio_filter_lsmash )
    {
        lsmash_source_t *h = filter_chain;
        return h->summary && h->importer ? chain : NULL;
    }
    fprintf( stderr, "lsmash [error]: attempted to enter copy mode with a non-empty filter chain!\n" ); // as far as CLI users see, lavf isn't a filter
    return NULL;
}

static audio_packet_t *copy_finish( hnd_t handle )
{
    return get_next_au( handle );
}

static int copy_is_failed( hnd_t handle )
{
    lsmash_source_t *h = handle;
    return h && h->copy_error;
}

static void copy_close( hnd_t handle )
{
    // do nothing or a double-free will happen when the filter chain is freed
}

const audio_filter_t audio_filter_lsmash =
{
    .name        = "lsmash",
    .description = "Demuxes raw audio streams using L-SMASH importer",
    .help        = "Arguments: filename[:track]",
    .init        = lsmash_init,
    .close       = lsmash_close
};

const audio_encoder_t audio_copy_lsmash =
{
    .init            = copy_init,
    .get_next_packet = get_next_au,
    .get_info        = get_info,
    .skip_samples    = skip_samples,
    .finish          = copy_finish,
    .free_packet     = free_packet,
    .close           = copy_close,
    .is_failed       = copy_is_failed
};
