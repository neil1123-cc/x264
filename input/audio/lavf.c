#undef DECLARE_ALIGNED
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavcodec/bsf.h"
#include <assert.h>
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
    AVFrame *decode_frame;
} lavf_source_t;

#ifndef AVCODEC_MAX_AUDIO_FRAME_SIZE
// 1 second of 192khz, 5.1 or 6 channel, 32bit audio
#define AVCODEC_MAX_AUDIO_FRAME_SIZE 4608000
#endif

#define DEFAULT_BUFSIZE AVCODEC_MAX_AUDIO_FRAME_SIZE * 2

static int buffer_next_frame( lavf_source_t *h );
static audio_packet_t *convert_to_audio_packet( hnd_t handle, AVPacket *pkt );

const audio_filter_t audio_filter_lavf;

static int init( hnd_t *handle, const char *opt_str )
{
    assert( opt_str );
    assert( !(*handle) ); // This must be the first filter
    static const char * const optlist[] = { "filename", "track", NULL };
    char **opts = x264_split_options( opt_str, optlist );

    if( !opts )
        return -1;

    char *filename = x264_get_option( "filename", opts );
    char *trackstr = x264_otos( x264_get_option( "track", opts ), "any" );

    if( !filename )
    {
        x264_cli_log( "lavf", X264_LOG_ERROR, "no filename given" );
        goto fail2;
    }

    int track;
    if( !strcmp( trackstr, "any" ) )
        track = TRACK_ANY;
    else
        track = x264_otoi( trackstr, TRACK_NONE );

    if( track == TRACK_NONE )
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

    unsigned tid = TRACK_NONE;
    if( track >= 0 )
    {
        if( track < h->lavf->nb_streams &&
            h->lavf->streams[track]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO )
            tid = track;
        else
            AF_LOG_ERR( h, "requested track %d is unavailable "
                           "or is not an audio track\n", track );
    }
    else // TRACK_ANY (pick first)
    {
        for( track = 0;
             track < h->lavf->nb_streams &&
             h->lavf->streams[track]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO; )
            ++track;
        if( track < h->lavf->nb_streams )
            tid = track;
        else
            AF_LOG_ERR( h, "could not find any audio track\n" );
    }

    if( tid == TRACK_NONE )
        goto fail;

    h->track = tid;

    // FFmpeg 8.x: Use codecpar instead of stream->codec
    AVCodecParameters *codecpar = h->lavf->streams[tid]->codecpar;
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
    int64_t chanlayout = 0;
    if( h->ctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE )
        chanlayout = h->ctx->ch_layout.u.mask;
    else if( h->ctx->ch_layout.order == AV_CHANNEL_ORDER_DEFAULT )
        chanlayout = av_channel_layout_subset( &h->ctx->ch_layout, UINT64_MAX );

    h->info = (audio_info_t)
    {
        .codec_name     = h->codec->name,
        .samplerate     = h->ctx->sample_rate,
        .channels       = channels,
        .chanlayout     = chanlayout,
        .framelen       = h->ctx->frame_size,
        .framesize      = h->ctx->frame_size * sizeof( float ),
        .chansize       = av_get_bytes_per_sample( h->samplefmt ),
        .samplesize     = av_get_bytes_per_sample( h->samplefmt ) * channels,
        .depth          = h->ctx->bits_per_coded_sample,
        .timebase       = /* {1, 1000}, /*/ { 1, h->ctx->sample_rate },
        .extradata      = h->ctx->extradata,
        .extradata_size = h->ctx->extradata_size,
        .last_delta     = h->ctx->frame_size
    };
    h->origtb = (timebase_t) { h->lavf->streams[track]->time_base.num, h->lavf->streams[track]->time_base.den };

    h->bufsize = DEFAULT_BUFSIZE;
    h->surplus = h->info.framesize * 3 / 2;
    assert( h->bufsize > h->surplus * 2 );
    h->buffer  = av_malloc( h->bufsize );

    if( !buffer_next_frame( h ) )
        goto codecfail;

    free( opts );
    return 0;

codecfail:
    AF_LOG_ERR( h, "error decoding the %s audio for track %d\n", h->codec->name , h->track );
codecnotfound:
    AF_LOG_ERR( h, "no decoder found for track %d\n", h->track );
fail:
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
    av_packet_unref( pkt );
    free( pkt );
}

static void free_packet( hnd_t handle, audio_packet_t *pkt )
{
    pkt->owner = NULL;
    x264_af_free_packet( pkt );
}

static struct AVPacket *next_packet( hnd_t handle )
{
    lavf_source_t *h = handle;
    AVPacket *pkt = calloc( 1, sizeof( AVPacket ) );

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
    } while( pkt->stream_index != h->track );

    return pkt;
}

static hnd_t copy_init( hnd_t filter_chain, const char *opts )
{
    assert( filter_chain );
    audio_hnd_t *chain = filter_chain;
    if( chain->self == &audio_filter_lavf )
    {
        lavf_source_t *h = filter_chain;
        h->copy = 1;

        if( !h->pkt )
        {
            fprintf( stderr, "lavf [error]: demuxing error occured!\n" );
            return NULL;
        }

        if( ( h->ctx->codec_id == AV_CODEC_ID_AAC ) && !h->ctx->extradata )
        {
            const AVBitStreamFilter *bsf = av_bsf_get_by_name( "aac_adtstoasc" );
            if( !bsf || av_bsf_alloc( bsf, &h->bsfs ) < 0 )
            {
                fprintf( stderr, "lavf [error]: failed to init aac_adtstoasc bitstream filter!\n" );
                return NULL;
            }
            avcodec_parameters_from_context( h->bsfs->par_in, h->ctx );
            av_bsf_init( h->bsfs );
            h->out = convert_to_audio_packet( h, h->pkt );
            h->info.extradata = h->ctx->extradata;
            h->info.extradata_size = h->ctx->extradata_size;
            h->info.codec_name = "aac";
        }
        else if( ( h->ctx->codec_id == AV_CODEC_ID_AC3 ) && !h->ctx->extradata )
        {
            h->ctx->extradata_size = h->pkt->size;
            h->ctx->extradata = av_malloc( h->ctx->extradata_size );
            if( !h->ctx->extradata )
            {
                fprintf( stderr, "lavf [error]: malloc failed!\n" );
                return NULL;
            }
            memcpy( h->ctx->extradata, h->pkt->data, h->ctx->extradata_size );
            h->out = convert_to_audio_packet( h, h->pkt );
            h->info.extradata = h->ctx->extradata;
            h->info.extradata_size = h->ctx->extradata_size;
            h->info.codec_name = "ac3";
        }
        else
            h->out = convert_to_audio_packet( h, h->pkt );

        h->pkt = NULL;
        return chain;
    }
    fprintf( stderr, "lavf [error]: attempted to enter copy mode with a non-empty filter chain!" ); // as far as CLI users see, lavf isn't a filter
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    audio_hnd_t *h = handle;
    return &h->info;
}

static audio_packet_t *convert_to_audio_packet( hnd_t handle, AVPacket *pkt )
{
    lavf_source_t *h = handle;
    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );

    out->dts = x264_convert_timebase( pkt->dts != AV_NOPTS_VALUE ? pkt->dts :
                                      pkt->pts != AV_NOPTS_VALUE ? pkt->pts : INVALID_DTS,
                                      h->origtb, h->info.timebase );
    out->info        = h->info;
    out->channels    = h->info.channels;

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
            out->samplecount = out_pkt->size * h->info.samplesize;
            out->size = out_pkt->size;
            out->data = malloc( out->size );
            memcpy( out->data, out_pkt->data, out->size );
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
        out->samplecount = pkt->size * h->info.samplesize;
        out->size        = pkt->size;
        out->data        = malloc( out->size );
        memcpy( out->data, pkt->data, pkt->size );
    }
    free_avpacket( pkt );
    return out;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    lavf_source_t *h = handle;

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
    if( pkt->duration && ( h->info.framelen != x264_from_timebase( pkt->duration, h->origtb, h->info.timebase.den ) ) )
        h->info.last_delta = x264_from_timebase( pkt->duration, h->origtb, h->info.timebase.den );
    return convert_to_audio_packet( h, pkt );
}

static audio_packet_t *copy_finish( hnd_t handle )
{
    return NULL; // Any other sensible thing to do?
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    // WARNING: this cannot be made exact
    lavf_source_t *h = handle;
    if( samplecount < h->info.framelen )
        return; // Nothing to do due to low accuracy
    audio_packet_t *pkt;
    uint64_t samples_skipped = 0;
    while( samples_skipped <= ( samplecount - h->info.framelen ) && ( pkt = get_next_packet( h ) ) )
    {
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

    int planar = av_sample_fmt_is_planar( h->ctx->sample_fmt );
    int channels = h->ctx->ch_layout.nb_channels;
    int data_size = av_samples_get_buffer_size( NULL, channels, h->decode_frame->nb_samples,
                                                  h->ctx->sample_fmt, 1 );
    if( data_size > buflen )
        return -1;

    int plane_size = data_size / (planar ? channels : 1);

    memcpy( buf, h->decode_frame->extended_data[0], plane_size );

    if( planar && channels > 1 )
    {
        uint8_t *out = ((uint8_t *)buf) + plane_size;
        for( int ch = 1; ch < channels; ch++ )
        {
            memcpy( out, h->decode_frame->extended_data[ch], plane_size );
            out += plane_size;
        }
    }

    return data_size;
}

static int low_decode_audio( lavf_source_t *h, uint8_t *buf, intptr_t buflen )
{
    static uint8_t desync_warn = 0;
    int ret;

    // Try to get a decoded frame first (might already have one buffered)
    ret = decode_audio_frame( h, buf, buflen );
    if( ret > 0 )
        return ret;
    if( ret < 0 && ret != AVERROR( EAGAIN ) )
    {
        if( !desync_warn++ )
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
            if( !desync_warn++ )
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
            if( !desync_warn++ )
                AF_LOG_WARN( h, "Decoding errors may cause audio desync\n" );
        }
    }
}

static struct AVPacket *decode_next_frame( lavf_source_t *h )
{
    AVPacket *dst = calloc( 1, sizeof( AVPacket ) );
    assert( !av_new_packet( dst, AVCODEC_MAX_AUDIO_FRAME_SIZE ) );

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

    if( h->len + dec->size > h->bufsize )
    {
        memmove( h->buffer, h->buffer + dec->size, h->bufsize - dec->size );
        h->len     -= dec->size;
        h->bytepos += dec->size;
    }
    memcpy( h->buffer + h->len, dec->data, dec->size );
    h->len += dec->size;

    free_avpacket( dec );

    return 1;
}

static inline int not_in_cache( lavf_source_t *h, int64_t sample )
{
    int64_t samplebyte = sample * h->info.samplesize;
    if( samplebyte < h->bytepos )
        return -1; // before
    else if( samplebyte < h->bytepos + h->len )
        return 0; // in cache
    return 1; // after
}

static int64_t fill_buffer_until( lavf_source_t *h, int64_t lastsample )
{
    static int errored = 0;
    if( errored )
        return -1;
    if( not_in_cache( h, lastsample ) < 0 )
    {
        AF_LOG_ERR( h, "backwards seeking not supported yet "
                       "(requested sample %"PRIu64", first available is %"PRIu64")\n",
                       lastsample, h->bytepos / h->info.samplesize );
        return -1;
    }
    int ret;
    while( ( ret = not_in_cache( h, lastsample ) ) > 0 )
    {
        if( !buffer_next_frame( h ) )
        {
            // libavcodec already warns for us
            errored = 1;
            break;
        }
    }
    assert( ret >= 0 );
    return h->bytepos + h->len;
}


static struct audio_packet_t *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    lavf_source_t *h = handle;
    assert( !h->copy );
    assert( first_sample >= 0 && last_sample > first_sample );

    if( fill_buffer_until( h, first_sample ) < 0 )
        return NULL;

    audio_packet_t *pkt = calloc( 1, sizeof( audio_packet_t ) );
    pkt->info           = h->info;
    pkt->channels       = h->info.channels;
    pkt->samplecount    = last_sample - first_sample;
    pkt->size           = pkt->samplecount * h->info.samplesize;
    pkt->dts            = first_sample;

    if( pkt->size + h->surplus > h->bufsize )
    {
        int64_t pivot = first_sample + ( h->bufsize - h->surplus * 2 ) / h->info.samplesize;
        int64_t expected_size = ( pivot - first_sample ) * h->info.samplesize;

        audio_packet_t *prev = get_samples( h, first_sample, pivot );
        if( !prev )
            goto fail;

        if( prev->size < expected_size ) // EOF
        {
            x264_af_free_packet( pkt );
            prev->flags |= AUDIO_FLAG_EOF;
            return prev;
        }
        assert( prev->size == expected_size );

        audio_packet_t *next = get_samples( h, pivot, last_sample );
        if( !next )
        {
            x264_af_free_packet( prev );
            goto fail;
        }

        pkt->samples = x264_af_dup_buffer( prev->samples, prev->channels, prev->samplecount );
        x264_af_cat_buffer( pkt->samples, pkt->samplecount, next->samples, next->samplecount, pkt->channels );

        pkt->samplecount = prev->samplecount + next->samplecount;
        pkt->size        = prev->size + next->size;

        x264_af_free_packet( prev );
        x264_af_free_packet( next );
    }
    else
    {
        int64_t lastreq   = last_sample * h->info.samplesize;
        int64_t lastavail = fill_buffer_until( h, last_sample );
        if( lastavail < 0 )
            goto fail;

        intptr_t start = first_sample * h->info.samplesize - h->bytepos;

        if( lastavail < lastreq )
        {
            pkt->size        = lastavail - h->bytepos - start;
            pkt->samplecount = pkt->size / h->info.samplesize;
            pkt->flags       = AUDIO_FLAG_EOF;
        }
        assert( start + pkt->size <= h->bufsize );
        pkt->samples = x264_af_deinterleave2( h->buffer + start, h->samplefmt, pkt->channels, pkt->samplecount );
    }

    return pkt;

fail:
    x264_af_free_packet( pkt );
    return NULL;
}

static void lavf_close( hnd_t handle )
{
    assert( handle );
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
