#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#endif

#undef DECLARE_ALIGNED
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <stdio.h>
#include <inttypes.h>

#include "audio/encoders.h"
#include "filters/audio/internal.h"

typedef struct lavf_source_t
{
    AUDIO_FILTER_COMMON
    AVFormatContext *lavf;
    AVCodecContext *ctx;
    const AVCodec *codec;
    AVBSFContext *bsfs;

    int samplefmt;
    unsigned track;
    uint8_t *buffer;
    intptr_t bufsize;
    intptr_t surplus;
    intptr_t len;
    uint64_t bytepos;

    timebase_t origtb;
    AVPacket *pkt;
    audio_packet_t *out;
    int copy;
    int eof;
    int decode_error;
    uint8_t desync_warn;
    AVFrame *decode_frame;
} lavf_source_t;

#ifndef AVCODEC_MAX_AUDIO_FRAME_SIZE
// 1 second of 192khz, 5.1 or 6 channel, 32bit audio
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 4608000
#endif

#define DEFAULT_BUFSIZE AVCODEC_MAX_AUDIO_FRAME_SIZE * 2

static int buffer_next_frame( lavf_source_t *h );
static audio_packet_t *convert_to_audio_packet( hnd_t handle, AVPacket *pkt, uint32_t last_delta );

const audio_filter_t audio_filter_lavf;

static int lavf_parse_audio_track( const char *trackstr, int *track )
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

static int init( hnd_t *handle, const char *opt_str )
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
        x264_cli_log( "lavf", X264_LOG_ERROR, "no filename given" );
        goto fail2;
    }

    int track;
    if( lavf_parse_audio_track( x264_get_option( "track", opts ), &track ) )
    {
        x264_cli_log( "lavf", X264_LOG_ERROR, "no valid track requested ('any', 0 or a positive integer)\n" );
        goto fail2;
    }

    INIT_FILTER_STRUCT( audio_filter_lavf, lavf_source_t );

    // av_register_all() removed in FFmpeg 8.x - no longer needed
    if( !strcmp( filename, "-" ) )
        filename = "pipe:";

    if( avformat_open_input( &h->lavf, filename, NULL, NULL ) )
    {
        AF_LOG_ERR( h, "could not open audio file\n" );
        goto fail;
    }

    if( avformat_find_stream_info( h->lavf, NULL ) < 0 )
    {
        AF_LOG_ERR( h, "could not find stream info\n" );
        goto fail;
    }
    if( h->lavf->nb_streams > INT_MAX )
    {
        AF_LOG_ERR( h, "too many input streams\n" );
        goto fail;
    }

    unsigned tid = UINT_MAX;
    if( track >= 0 )
    {
        unsigned requested_track = (unsigned)track;
        AVStream *stream = requested_track < h->lavf->nb_streams ? h->lavf->streams[requested_track] : NULL;
        if( stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO )
            tid = requested_track;
        else
            AF_LOG_ERR( h, "requested track %d is unavailable "
                           "or is not an audio track\n", track );
    }
    else // TRACK_ANY (pick first)
    {
        for( unsigned i = 0; i < h->lavf->nb_streams; i++ )
        {
            AVStream *stream = h->lavf->streams[i];
            if( stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO )
            {
                tid = i;
                break;
            }
        }
        if( tid == UINT_MAX )
        {
            AF_LOG_ERR( h, "could not find any audio track\n" );
        }
    }

    if( tid == UINT_MAX )
        goto fail;

    h->track = tid;

    // FFmpeg 8.x: Use codecpar instead of stream->codec
    AVStream *stream = h->lavf->streams[tid];
    if( !stream || !stream->codecpar )
        goto codecnotfound;
    AVCodecParameters *codecpar = stream->codecpar;
    h->codec = avcodec_find_decoder( codecpar->codec_id );
    if( !h->codec )
        goto codecnotfound;

    h->ctx = avcodec_alloc_context3( h->codec );
    if( !h->ctx )
        goto codecnotfound;

    if( avcodec_parameters_to_context( h->ctx, codecpar ) < 0 )
        goto codecfail;

    if( avcodec_open2( h->ctx, h->codec, NULL ) < 0 )
        goto codecfail;

    // Allocate decode frame
    h->decode_frame = av_frame_alloc();
    if( !h->decode_frame )
        goto codecfail;

    h->samplefmt  = h->ctx->sample_fmt;

    // FFmpeg 8.x: Get channels from ch_layout
    int channels = h->ctx->ch_layout.nb_channels;
    uint64_t chanlayout_mask = 0;
    if( h->ctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE )
        chanlayout_mask = h->ctx->ch_layout.u.mask;
    else if( h->ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC )
        chanlayout_mask = av_channel_layout_subset( &h->ctx->ch_layout, UINT64_MAX );

    AVRational stream_timebase = stream->time_base;
    int bytes_per_sample = av_get_bytes_per_sample( h->samplefmt );
    if( channels <= 0 || bytes_per_sample <= 0 || bytes_per_sample > INT_MAX / 8 ||
        h->ctx->sample_rate <= 0 ||
        h->ctx->frame_size < 0 || h->ctx->extradata_size < 0 ||
        stream_timebase.num <= 0 || stream_timebase.den <= 0 ||
        chanlayout_mask > (uint64_t)INT64_MAX )
        goto codecfail;
    int64_t samplesize64 = (int64_t)bytes_per_sample * channels;
    if( samplesize64 > INT_MAX )
        goto codecfail;
    int samplesize = (int)samplesize64;
    int samplerate = h->ctx->sample_rate;
    int frame_length = h->ctx->frame_size;
    int extradata_size = h->ctx->extradata_size;
    int64_t channel_layout = (int64_t)chanlayout_mask;
    uint32_t last_delta = (uint32_t)h->ctx->frame_size;
    size_t decoded_frame_size;
    if( (uint64_t)h->ctx->frame_size > SIZE_MAX / sizeof(float) )
        goto codecfail;
    decoded_frame_size = (size_t)h->ctx->frame_size * sizeof(float);

    h->info = (audio_info_t)
    {
        .codec_name     = h->codec->name,
        .samplerate     = samplerate,
        .channels       = channels,
        .chanlayout     = channel_layout,
        .framelen       = frame_length,
        .framesize      = decoded_frame_size,
        .chansize       = bytes_per_sample,
        .samplesize     = samplesize,
        .depth          = h->ctx->bits_per_coded_sample,
        .timebase       = /* {1, 1000}, /*/ { 1, samplerate },
        .extradata      = h->ctx->extradata,
        .extradata_size = extradata_size,
        .last_delta     = last_delta
    };
    h->origtb = (timebase_t) { stream_timebase.num, stream_timebase.den };

    h->bufsize = DEFAULT_BUFSIZE;
    if( h->info.framesize > (size_t)(INTPTR_MAX / 3 * 2) )
        goto codecfail;
    h->surplus = (intptr_t)(h->info.framesize * 3 / 2);
    if( h->surplus < 0 || h->surplus >= (intptr_t)(( h->bufsize + 1 ) / 2) )
        goto codecfail;
    h->buffer  = av_malloc( (size_t)h->bufsize );
    if( !h->buffer )
        goto codecfail;

    if( !buffer_next_frame( h ) )
        goto codecfail;

    free( opts );
    return 0;

codecfail:
    AF_LOG_ERR( h, "error decoding the %s audio for track %d\n", h->codec->name , h->track );
codecnotfound:
    AF_LOG_ERR( h, "no decoder found for track %d\n", h->track );
fail:
    if( h && h->buffer )
        av_free( h->buffer );
    if( h && h->decode_frame )
        av_frame_free( &h->decode_frame );
    if( h && h->ctx )
        avcodec_free_context( &h->ctx );
    if( h && h->lavf )
        avformat_close_input( &h->lavf );
    if( h )
        free( h );
    *handle = NULL;
fail2:
    free( opts );
    return -1;
}

static inline void free_avpacket( AVPacket *pkt )
{
    if( !pkt )
        return;
    av_packet_unref( pkt );
    free( pkt );
}

static void free_packet( hnd_t handle, audio_packet_t *pkt )
{
    if( !pkt )
        return;
    pkt->owner = NULL;
    x264_af_free_packet( pkt );
}

static struct AVPacket *next_packet( hnd_t handle )
{
    lavf_source_t *h = handle;
    if( !h || !h->lavf )
        return NULL;
    int64_t packet_alloc_size = sizeof( AVPacket );
    AVPacket *pkt = calloc( 1, packet_alloc_size );
    if( !pkt )
    {
        AF_LOG_ERR( h, "malloc failed\n" );
        return NULL;
    }

    int ret;
    do
    {
        if( pkt->data )
            av_packet_unref( pkt );
        if( (ret = av_read_frame( h->lavf, pkt )) )
        {
            if( ret != AVERROR_EOF )
                AF_LOG_ERR( h, "read error: %s\n", strerror( -ret ) );
            else
            {
                AF_LOG( h, X264_LOG_INFO, "end of file reached\n" );
                h->eof = 1;
            }
            free_avpacket( pkt );
            return NULL;
        }
    } while( pkt->stream_index < 0 || (unsigned)pkt->stream_index != h->track );

    return pkt;
}

static hnd_t copy_init( hnd_t filter_chain, const char *opts )
{
    if( !filter_chain )
        return NULL;
    audio_hnd_t *chain = filter_chain;
    if( chain->self == &audio_filter_lavf )
    {
        lavf_source_t *h = filter_chain;
        if( !h->ctx )
            return NULL;

        if( !h->pkt )
        {
            fprintf( stderr, "lavf [error]: demuxing error occured!\n" );
            return NULL;
        }

        audio_packet_t *out = NULL;
        if( ( h->ctx->codec_id == AV_CODEC_ID_AAC ) && !h->ctx->extradata )
        {
            const AVBitStreamFilter *bsf = av_bsf_get_by_name( "aac_adtstoasc" );
            if( !bsf || av_bsf_alloc( bsf, &h->bsfs ) < 0 )
            {
                fprintf( stderr, "lavf [error]: failed to init aac_adtstoasc bitstream filter!\n" );
                return NULL;
            }
            if( avcodec_parameters_from_context( h->bsfs->par_in, h->ctx ) < 0 ||
                av_bsf_init( h->bsfs ) < 0 )
            {
                fprintf( stderr, "lavf [error]: failed to init aac_adtstoasc bitstream filter!\n" );
                av_bsf_free( &h->bsfs );
                return NULL;
            }
            out = convert_to_audio_packet( h, h->pkt, h->info.last_delta );
            if( out )
            {
                h->info.extradata = h->ctx->extradata;
                h->info.extradata_size = h->ctx->extradata_size;
                h->info.codec_name = "aac";
            }
        }
        else if( ( h->ctx->codec_id == AV_CODEC_ID_AC3 ) && !h->ctx->extradata )
        {
            if( h->pkt->size <= 0 || !h->pkt->data )
            {
                fprintf( stderr, "lavf [error]: invalid AC3 packet size!\n" );
                return NULL;
            }
            int extradata_size = h->pkt->size;
            uint8_t *extradata = av_malloc( (size_t)extradata_size );
            if( !extradata )
            {
                fprintf( stderr, "lavf [error]: malloc failed!\n" );
                return NULL;
            }
            memcpy( extradata, h->pkt->data, (size_t)extradata_size );
            out = convert_to_audio_packet( h, h->pkt, h->info.last_delta );
            if( out )
            {
                h->ctx->extradata_size = extradata_size;
                h->ctx->extradata = extradata;
                h->info.extradata = h->ctx->extradata;
                h->info.extradata_size = h->ctx->extradata_size;
                h->info.codec_name = "ac3";
            }
            else
                av_free( extradata );
        }
        else
            out = convert_to_audio_packet( h, h->pkt, h->info.last_delta );

        h->pkt = NULL;
        if( !out )
        {
            av_bsf_free( &h->bsfs );
            fprintf( stderr, "lavf [error]: failed to convert initial audio packet!\n" );
            return NULL;
        }
        h->out = out;
        h->copy = 1;
        return chain;
    }
    fprintf( stderr, "lavf [error]: attempted to enter copy mode with a non-empty filter chain!" ); // as far as CLI users see, lavf isn't a filter
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    audio_hnd_t *h = handle;
    if( !h )
        return NULL;
    return &h->info;
}

static int copy_packet_payload( lavf_source_t *h, audio_packet_t *out, const uint8_t *data, int size, uint32_t last_delta )
{
    if( !h || !out || size < 0 || h->info.samplesize < 0 || (size && !data) )
        return -1;
    out->samplecount = last_delta;
    if( !out->samplecount && h->info.framelen > 0 )
    {
        unsigned frame_samplecount = (unsigned)h->info.framelen;
        out->samplecount = frame_samplecount;
    }
    out->size        = size;
    out->data        = NULL;
    if( size )
    {
        out->data = malloc( (size_t)size );
        if( !out->data )
            return -1;
        memcpy( out->data, data, (size_t)size );
    }
    return 0;
}

static audio_packet_t *convert_to_audio_packet( hnd_t handle, AVPacket *pkt, uint32_t last_delta )
{
    lavf_source_t *h = handle;
    int64_t out_alloc_size = sizeof( audio_packet_t );
    audio_packet_t *out = calloc( 1, out_alloc_size );
    if( !h || !pkt || pkt->size < 0 )
    {
        free_avpacket( pkt );
        free( out );
        return NULL;
    }
    if( !out )
    {
        free_avpacket( pkt );
        return NULL;
    }

    int64_t packet_dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts :
                         pkt->pts != AV_NOPTS_VALUE ? pkt->pts : INVALID_DTS;
    out->dts = x264_convert_timebase( packet_dts, h->origtb, h->info.timebase );
    if( packet_dts != INVALID_DTS && (out->dts == INT64_MAX || out->dts == INT64_MIN) )
    {
        free_avpacket( pkt );
        free( out );
        return NULL;
    }
    out->info        = h->info;
    out->info.last_delta = last_delta;
    unsigned packet_channels = (unsigned)h->info.channels;
    out->channels    = packet_channels;

    if( h->bsfs )
    {
        AVPacket *in_pkt = av_packet_alloc();
        AVPacket *out_pkt = av_packet_alloc();
        if( !in_pkt || !out_pkt )
        {
            if( in_pkt ) av_packet_free( &in_pkt );
            if( out_pkt ) av_packet_free( &out_pkt );
            free_avpacket( pkt );
            free( out );
            return NULL;
        }
        in_pkt->data = pkt->data;
        in_pkt->size = pkt->size;
        int ret = av_bsf_send_packet( h->bsfs, in_pkt );
        if( ret >= 0 )
            ret = av_bsf_receive_packet( h->bsfs, out_pkt );
        av_packet_free( &in_pkt );
        if( ret >= 0 )
        {
            if( copy_packet_payload( h, out, out_pkt->data, out_pkt->size, last_delta ) )
            {
                av_packet_free( &out_pkt );
                free_avpacket( pkt );
                x264_af_free_packet( out );
                return NULL;
            }
        }
        else
        {
            out->samplecount = 0;
            out->size = 0;
            out->data = NULL;
        }
        av_packet_free( &out_pkt );
    }
    else
    {
        if( copy_packet_payload( h, out, pkt->data, pkt->size, last_delta ) )
        {
            free_avpacket( pkt );
            x264_af_free_packet( out );
            return NULL;
        }
    }
    free_avpacket( pkt );
    return out;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    lavf_source_t *h = handle;
    if( !h || !h->copy || !h->ctx )
        return NULL;

    if( h->eof )
        return NULL;

    if( h->out )
    {
        audio_packet_t *out = h->out;
        h->out = NULL;
        return out;
    }

    AVPacket *pkt = next_packet( h );
    if( !pkt )
        return NULL;
    uint32_t last_delta = h->info.last_delta;
    if( pkt->duration )
    {
        int64_t duration_delta = x264_from_timebase( pkt->duration, h->origtb, h->info.timebase.den );
        if( h->info.framelen != duration_delta )
        {
            uint32_t packet_last_delta = duration_delta > 0 && duration_delta <= UINT32_MAX ? (uint32_t)duration_delta : 0;
            last_delta = packet_last_delta;
        }
    }
    audio_packet_t *out = convert_to_audio_packet( h, pkt, last_delta );
    if( out )
        h->info.last_delta = last_delta;
    return out;
}

static audio_packet_t *copy_finish( hnd_t handle )
{
    return NULL; // Any other sensible thing to do?
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    // WARNING: this cannot be made exact
    lavf_source_t *h = handle;
    if( !h || !h->copy )
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
        audio_packet_t *pkt = get_next_packet( h );
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

// FFmpeg 8.x: Use send/receive API for audio decoding
// Returns: >0 = data size decoded, 0 = need more data, <0 = error
static int decode_audio_frame( lavf_source_t *h, uint8_t *buf, intptr_t buflen )
{
    int ret = avcodec_receive_frame( h->ctx, h->decode_frame );
    if( ret == AVERROR( EAGAIN ) )
    {
        // Need more input data
        return 0;
    }
    else if( ret < 0 )
    {
        return ret;
    }

    int planar = av_sample_fmt_is_planar( h->decode_frame->format );
    int channels = h->decode_frame->ch_layout.nb_channels;
    int data_size = av_samples_get_buffer_size( NULL, channels, h->decode_frame->nb_samples,
                                                  h->decode_frame->format, 1 );
    if( h->decode_frame->format != h->ctx->sample_fmt ||
        channels != h->ctx->ch_layout.nb_channels ||
        h->decode_frame->nb_samples <= 0 ||
        channels <= 0 || data_size <= 0 || data_size > buflen )
        return -1;
    if( !h->decode_frame->extended_data || !h->decode_frame->extended_data[0] )
        return -1;
    if( planar && data_size % channels )
        return -1;

    int plane_size = data_size / (planar ? channels : 1);
    if( h->decode_frame->linesize[0] < plane_size )
        return -1;

    memcpy( buf, h->decode_frame->extended_data[0], (size_t)plane_size );

    if( planar && channels > 1 )
    {
        uint8_t *out = ((uint8_t *)buf) + plane_size;
        for( int ch = 1; ch < channels; ch++ )
        {
            if( !h->decode_frame->extended_data[ch] )
                return -1;
            memcpy( out, h->decode_frame->extended_data[ch], (size_t)plane_size );
            out += plane_size;
        }
    }

    return data_size;
}

static int low_decode_audio( lavf_source_t *h, uint8_t *buf, intptr_t buflen )
{
    int ret;
    if( !h || !h->ctx || !h->decode_frame || !buf || buflen <= 0 )
        return -1;

    // Try to get a decoded frame first (might already have one buffered)
    ret = decode_audio_frame( h, buf, buflen );
    if( ret > 0 )
        return ret;
    if( ret < 0 && ret != AVERROR( EAGAIN ) )
    {
        if( !h->desync_warn++ )
            AF_LOG_WARN( h, "Decoding errors may cause audio desync\n" );
    }

    // Need more data, send packets until we get a frame
    while( 1 )
    {
        if( !h->pkt )
        {
            h->pkt = next_packet( h );
            if( !h->pkt )
                return -1; // EOF
        }

        ret = avcodec_send_packet( h->ctx, h->pkt );
        if( ret < 0 && ret != AVERROR( EAGAIN ) )
        {
            if( !h->desync_warn++ )
                AF_LOG_WARN( h, "Decoding errors may cause audio desync\n" );
            free_avpacket( h->pkt );
            h->pkt = NULL;
            continue;
        }

        if( ret != AVERROR( EAGAIN ) )
        {
            // Packet was consumed
            free_avpacket( h->pkt );
            h->pkt = NULL;
        }

        // Try to get a frame
        ret = decode_audio_frame( h, buf, buflen );
        if( ret > 0 )
            return ret;
        if( ret < 0 && ret != AVERROR( EAGAIN ) )
        {
            if( !h->desync_warn++ )
                AF_LOG_WARN( h, "Decoding errors may cause audio desync\n" );
        }
    }
}

static struct AVPacket *decode_next_frame( lavf_source_t *h )
{
    int64_t decode_packet_alloc_size = sizeof( AVPacket );
    AVPacket *dst = calloc( 1, decode_packet_alloc_size );
    if( !dst || av_new_packet( dst, AVCODEC_MAX_AUDIO_FRAME_SIZE ) )
    {
        free( dst );
        return NULL;
    }

    int len = 0;
    while( ( len = low_decode_audio( h, dst->data, dst->size ) ) == 0 )
    {
        // Read more
    }
    if( len < 0 ) // EOF or demuxing error
    {
        free_avpacket( dst );
        return NULL;
    }

    dst->size = len;

    return dst;
}

static int buffer_next_frame( lavf_source_t *h )
{
    AVPacket *dec = decode_next_frame( h );
    if( !dec )
        return 0;

    if( dec->size < 0 || dec->size > h->bufsize )
    {
        free_avpacket( dec );
        return 0;
    }
    intptr_t dec_size = dec->size;

    if( h->len < 0 || h->len > h->bufsize )
    {
        free_avpacket( dec );
        return 0;
    }

    if( dec_size > h->bufsize - h->len )
    {
        intptr_t drop = X264_MIN( h->len, dec_size );
        if( (uint64_t)drop > UINT64_MAX - h->bytepos )
        {
            free_avpacket( dec );
            return 0;
        }
        memmove( h->buffer, h->buffer + drop, (size_t)(h->len - drop) );
        h->len     -= drop;
        h->bytepos += (uint64_t)drop;
    }
    if( dec_size > h->bufsize - h->len )
    {
        free_avpacket( dec );
        return 0;
    }
    memcpy( h->buffer + h->len, dec->data, (size_t)dec_size );
    h->len += dec_size;

    free_avpacket( dec );

    return 1;
}

static inline int sample_to_byte( lavf_source_t *h, int64_t sample, int64_t *byte )
{
    if( sample < 0 || h->info.samplesize <= 0 ||
        sample > INT64_MAX / h->info.samplesize )
        return -1;
    *byte = sample * h->info.samplesize;
    return 0;
}

static inline int not_in_cache( lavf_source_t *h, int64_t sample )
{
    int64_t samplebyte;
    if( sample_to_byte( h, sample, &samplebyte ) )
        return -1;
    if( h->len < 0 )
        return -1;
    if( (uint64_t)samplebyte < h->bytepos )
        return -1; // before
    if( h->bytepos > UINT64_MAX - (uint64_t)h->len )
        return 1;
    else if( (uint64_t)samplebyte < h->bytepos + (uint64_t)h->len )
        return 0; // in cache
    return 1; // after
}

static int64_t fill_buffer_until( lavf_source_t *h, int64_t lastsample )
{
    if( h->decode_error )
        return -1;
    if( not_in_cache( h, lastsample ) < 0 )
    {
        AF_LOG_ERR( h, "backwards seeking not supported yet "
                       "(requested sample %"PRId64", first available is %"PRIu64")\n",
                       lastsample, h->bytepos / (uint64_t)h->info.samplesize );
        return -1;
    }
    int ret;
    while( ( ret = not_in_cache( h, lastsample ) ) > 0 )
    {
        if( !buffer_next_frame( h ) )
        {
            // libavcodec already warns for us
            h->decode_error = 1;
            break;
        }
    }
    if( ret < 0 )
        return -1;
    if( h->len < 0 ||
        h->bytepos > (uint64_t)INT64_MAX ||
        h->len > INT64_MAX - (int64_t)h->bytepos )
        return -1;
    return (int64_t)h->bytepos + h->len;
}


static struct audio_packet_t *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    lavf_source_t *h = handle;
    if( !h )
        return NULL;
    if( h->copy || !h->buffer || first_sample < 0 || last_sample <= first_sample ||
        h->info.channels <= 0 || h->info.samplesize <= 0 )
        return NULL;

    if( fill_buffer_until( h, first_sample ) < 0 )
        return NULL;

    int64_t first_byte, last_byte;
    if( sample_to_byte( h, first_sample, &first_byte ) ||
        sample_to_byte( h, last_sample, &last_byte ) )
        return NULL;

    int64_t samples = last_sample - first_sample;
    if( samples > INT64_MAX / h->info.samplesize )
        return NULL;
    int64_t size = samples * h->info.samplesize;
    if( samples > UINT_MAX || size > INT_MAX )
        return NULL;

    int64_t sample_packet_alloc_size = sizeof( audio_packet_t );
    audio_packet_t *pkt = calloc( 1, sample_packet_alloc_size );
    if( !pkt )
        return NULL;
    int packet_size      = (int)size;
    unsigned packet_channels = (unsigned)h->info.channels;
    unsigned packet_samplecount = (unsigned)samples;
    pkt->info           = h->info;
    pkt->channels       = packet_channels;
    pkt->samplecount    = packet_samplecount;
    pkt->size           = packet_size;
    pkt->dts            = first_sample;

    if( h->surplus > h->bufsize || (intptr_t)pkt->size > h->bufsize - h->surplus )
    {
        if( h->surplus > h->bufsize / 2 )
            goto fail;
        int64_t pivot = first_sample + ( h->bufsize - h->surplus * 2 ) / h->info.samplesize;
        if( pivot <= first_sample || pivot >= last_sample )
            goto fail;
        int64_t chunk_samples = pivot - first_sample;
        if( chunk_samples <= 0 || chunk_samples > INT64_MAX / h->info.samplesize )
            goto fail;
        int64_t expected_size = chunk_samples * h->info.samplesize;
        if( expected_size > INT_MAX )
            goto fail;

        audio_packet_t *prev = get_samples( h, first_sample, pivot );
        if( !prev )
            goto fail;

        if( prev->size < expected_size ) // EOF
        {
            x264_af_free_packet( pkt );
            prev->flags |= AUDIO_FLAG_EOF;
            return prev;
        }
        if( prev->size != expected_size )
        {
            x264_af_free_packet( prev );
            goto fail;
        }

        audio_packet_t *next = get_samples( h, pivot, last_sample );
        if( !next )
        {
            x264_af_free_packet( prev );
            goto fail;
        }

        if( next->size < 0 || prev->samplecount > UINT_MAX - next->samplecount ||
            prev->size > INT_MAX - next->size )
        {
            x264_af_free_packet( prev );
            x264_af_free_packet( next );
            goto fail;
        }
        pkt->samples = x264_af_dup_buffer( prev->samples, prev->channels, prev->samplecount );
        if( !pkt->samples ||
            x264_af_cat_buffer( pkt->samples, prev->samplecount, next->samples, next->samplecount, pkt->channels ) )
        {
            x264_af_free_packet( prev );
            x264_af_free_packet( next );
            goto fail;
        }
        unsigned merged_samplecount = prev->samplecount + next->samplecount;
        int merged_size = prev->size + next->size;
        pkt->samplecount = merged_samplecount;
        pkt->size        = merged_size;

        x264_af_free_packet( prev );
        x264_af_free_packet( next );
    }
    else
    {
        int64_t lastreq   = last_byte;
        int64_t lastavail = fill_buffer_until( h, last_sample );
        if( lastavail < 0 )
            goto fail;

        if( (uint64_t)first_byte < h->bytepos )
            goto fail;
        uint64_t start_pos = (uint64_t)first_byte - h->bytepos;
        if( start_pos > INTPTR_MAX )
            goto fail;
        intptr_t start = (intptr_t)start_pos;

        if( lastavail < lastreq )
        {
            if( (uint64_t)lastavail < h->bytepos )
                goto fail;
            uint64_t available = (uint64_t)lastavail - h->bytepos;
            if( available < (uint64_t)start )
                goto fail;
            uint64_t available_size = available - (uint64_t)start;
            if( available_size > INT_MAX ||
                available_size / (uint64_t)h->info.samplesize > UINT_MAX )
                goto fail;
            int available_packet_size = (int)available_size;
            unsigned available_samples = (unsigned)( available_packet_size / h->info.samplesize );
            pkt->size        = available_packet_size;
            pkt->samplecount = available_samples;
            pkt->flags       = AUDIO_FLAG_EOF;
        }
        if( start > h->bufsize || pkt->size > h->bufsize - start )
            goto fail;
        pkt->samples = x264_af_deinterleave2( h->buffer + start, h->samplefmt, pkt->channels, pkt->samplecount );
        if( !pkt->samples )
            goto fail;
    }

    return pkt;

fail:
    x264_af_free_packet( pkt );
    return NULL;
}

static void lavf_close( hnd_t handle )
{
    if( !handle )
        return;
    lavf_source_t *h = handle;
    av_free( h->buffer );
    if( h->pkt )
        free_avpacket( h->pkt );
    if( h->decode_frame )
        av_frame_free( &h->decode_frame );
    if( h->ctx )
        avcodec_free_context( &h->ctx );
    avformat_close_input( &h->lavf );
    if( h->bsfs )
        av_bsf_free( &h->bsfs );
    free( h );
}

static void copy_close( hnd_t handle )
{
    // do nothing or a double-free will happen when the filter chain is freed
}

const audio_filter_t audio_filter_lavf =
{
    .name        = "lavf",
    .description = "Demuxes and decodes audio files using libavformat + libavcodec",
    .help        = "Arguments: filename[:track]",
    .init        = init,
    .get_samples = get_samples,
    .free_packet = free_packet,
    .close       = lavf_close
};

const audio_encoder_t audio_copy_lavf =
{
    .init            = copy_init,
    .get_next_packet = get_next_packet,
    .get_info        = get_info,
    .skip_samples    = skip_samples,
    .finish          = copy_finish,
    .free_packet     = free_packet,
    .close           = copy_close,
};
