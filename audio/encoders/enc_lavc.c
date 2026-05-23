#undef DECLARE_ALIGNED
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libswresample/swresample.h"
#include "libavutil/channel_layout.h"

#include <float.h>

#include "audio/encoders.h"
#include "filters/audio/internal.h"

typedef struct enc_lavc_t
{
    audio_info_t info;
    audio_info_t preinfo;
    hnd_t        filter_chain;
    int          finishing;
    int64_t      last_sample;
    int          buf_size;
    int64_t      last_dts;

    AVCodecContext         *ctx;
    enum AVSampleFormat     smpfmt;
    AVFrame                *frame;
    SwrContext             *avr;
} enc_lavc_t;

static int set_framesize( audio_info_t *info, const char *name )
{
    if( info->framelen <= 0 || info->samplesize <= 0 ||
        (uint64_t)info->framelen > SIZE_MAX / (uint64_t)info->samplesize )
    {
        x264_cli_log( name, X264_LOG_ERROR, "invalid audio frame size\n" );
        return -1;
    }
    info->framesize = (size_t)info->framelen * (size_t)info->samplesize;
    return 0;
}

static int get_sample_end( int64_t first_sample, int framelen, int64_t *last_sample )
{
    if( first_sample < 0 || framelen <= 0 || first_sample > INT64_MAX - framelen )
        return -1;
    *last_sample = first_sample + framelen;
    return 0;
}

static int add_frame_dts( int64_t *dts, int framelen )
{
    if( framelen <= 0 || ( *dts != INVALID_DTS && *dts > INT64_MAX - framelen ) )
        return -1;
    *dts += framelen;
    return 0;
}

static int add_info_delta_dts( int64_t *dts, audio_info_t *info )
{
    int delta = info->framelen;
    if( info->last_delta )
    {
        if( info->last_delta > INT_MAX )
            return -1;
        delta = (int)info->last_delta;
    }
    return add_frame_dts( dts, delta );
}

static void add_skip_samples( int64_t *last_sample, uint64_t samplecount )
{
    if( *last_sample >= 0 && samplecount <= (uint64_t)INT64_MAX - (uint64_t)*last_sample )
        *last_sample += samplecount;
    else
        *last_sample = INT64_MAX;
}

static int is_encoder_available( const char *name, void **priv )
{
    // avcodec_register_all() removed in FFmpeg 8.x
    const AVCodec *enc = NULL;

    if( (enc = avcodec_find_encoder_by_name( name )) )
    {
        if( priv )
            *priv = (void *)enc;
        return 0;
    }

    return -1;
}

static int parse_bool_option( const char *opt, int def, int *dst )
{
    if( !opt )
    {
        *dst = def;
        return 0;
    }
    return x264_otob_checked( opt, dst );
}

static int parse_float_option( const char *opt, double def, float *dst )
{
    double value;
    if( !opt )
        value = def;
    else if( x264_otof_checked( opt, &value ) )
        return -1;
    if( !(value >= -FLT_MAX && value <= FLT_MAX) )
        return -1;
    *dst = (float)value;
    return 0;
}

static int get_channel_layout_mask( const AVChannelLayout *layout, int64_t *mask )
{
    if( !layout || !mask )
        return -1;

    uint64_t layout_mask = 0;
    if( layout->order == AV_CHANNEL_ORDER_NATIVE )
        layout_mask = layout->u.mask;
    else if( layout->order == AV_CHANNEL_ORDER_UNSPEC )
        layout_mask = av_channel_layout_subset( layout, UINT64_MAX );

    if( layout_mask > (uint64_t)INT64_MAX )
        return -1;

    *mask = (int64_t)layout_mask;
    return 0;
}

static int set_avdict_option( AVDictionary **dict, const char *key, const char *value )
{
    return av_dict_set( dict, key, value, 0 ) < 0;
}

#define MODE_VBR     0x01
#define MODE_BITRATE 0x02
#define MODE_IGNORED MODE_VBR|MODE_BITRATE

static const struct {
    enum AVCodecID id;
    const char *name;
    uint8_t mode;
    float default_brval;
} ffcodecs[] = {
   /* AV_CODEC_ID,           name,        allowed mode,  default quality/bitrate */
    { AV_CODEC_ID_MP2,       "mp2",       MODE_BITRATE,    112 },
    { AV_CODEC_ID_VORBIS,    "vorbis",    MODE_VBR,        5.0 },
    { AV_CODEC_ID_AAC,       "aac",       MODE_BITRATE,     96 },
    { AV_CODEC_ID_AC3,       "ac3",       MODE_BITRATE,     96 },
    { AV_CODEC_ID_EAC3,      "eac3",      MODE_BITRATE,     96 },
    { AV_CODEC_ID_ALAC,      "alac",      MODE_IGNORED,     64 },
    { AV_CODEC_ID_AMR_NB,    "amrnb",     MODE_BITRATE,   12.2 },
    { AV_CODEC_ID_AMR_WB,    "amrwb",     MODE_BITRATE,  12.65 },
    { AV_CODEC_ID_PCM_F32BE, "pcm_f32be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_F32LE, "pcm_f32le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_F64BE, "pcm_f64be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_F64LE, "pcm_f64le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S16BE, "pcm_s16be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S16LE, "pcm_s16le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S24BE, "pcm_s24be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S24LE, "pcm_s24le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S32BE, "pcm_s32be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S32LE, "pcm_s32le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_S8,    "pcm_s8",    MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U16BE, "pcm_u16be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U16LE, "pcm_u16le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U24BE, "pcm_u24be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U24LE, "pcm_u24le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U32BE, "pcm_u32be", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U32LE, "pcm_u32le", MODE_IGNORED,      0 },
    { AV_CODEC_ID_PCM_U8,    "pcm_u8",    MODE_IGNORED,      0 },
    { AV_CODEC_ID_NONE,      NULL, },
};

static int resample_audio( SwrContext *avr, AVFrame *frame, audio_packet_t *pkt )
{
    // FFmpeg 8.x: Get channels from ch_layout
    int channels = frame->ch_layout.nb_channels;
    if( channels == 0 )
        channels = 2;
    int out_linesize;
    if( av_samples_get_buffer_size( &out_linesize, channels, frame->nb_samples, frame->format, 0 ) < 0 )
        return -1;
    if( swr_convert( avr, frame->data, frame->nb_samples,
                     (const uint8_t **)pkt->samples, pkt->samplecount ) < 0 )
        return -1;

    int planes = av_sample_fmt_is_planar( frame->format ) ? channels : 1;
    for( int i = 0; i < planes; i++ )
        frame->linesize[i] = out_linesize;

    return 0;
}

static int encode_audio( AVCodecContext *ctx, audio_packet_t *out, int out_size, AVFrame *frame )
{
    if( !out || !out->data || out_size < 0 )
        return -1;

    AVPacket *avpkt = av_packet_alloc();
    if( !avpkt )
        return -1;

    int ret = avcodec_send_frame( ctx, frame );
    if( ret < 0 )
    {
        av_packet_free( &avpkt );
        return -1;
    }

    ret = avcodec_receive_packet( ctx, avpkt );
    if( ret == 0 )
    {
        if( avpkt->size < 0 || avpkt->size > out_size ||
            (avpkt->size && !avpkt->data) )
        {
            av_packet_free( &avpkt );
            return -1;
        }
        out->size = avpkt->size;
        memcpy( out->data, avpkt->data, avpkt->size );
    }
    else if( ret == AVERROR( EAGAIN ) || ret == AVERROR_EOF )
        out->size = 0;
    else
    {
        av_packet_free( &avpkt );
        return -1;
    }

    av_packet_free( &avpkt );
    return 0;
}

#define ISCODEC( name ) (!strcmp( h->info.codec_name, #name ))

// FFmpeg 8.x: Helper function to get sample formats from codec
static int get_codec_sample_fmts( const AVCodec *codec, enum AVSampleFormat *fmts, int max_fmts )
{
    // FFmpeg 8.x: Use avcodec_get_supported_config
    const enum AVSampleFormat *sample_fmts = NULL;
    int num_fmts = 0;

    if( avcodec_get_supported_config( NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                       0, (const void **)&sample_fmts, &num_fmts ) >= 0 && sample_fmts )
    {
        int count = (num_fmts < max_fmts - 1) ? num_fmts : max_fmts - 1;
        for( int i = 0; i < count && sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++ )
            fmts[i] = sample_fmts[i];
        fmts[count] = AV_SAMPLE_FMT_NONE;
        return count;
    }

    // No sample formats available
    fmts[0] = AV_SAMPLE_FMT_NONE;
    return 0;
}

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    if( !filter_chain )
        return NULL;
    enc_lavc_t *h = calloc( 1, sizeof( enc_lavc_t ) );
    if( !h )
        return NULL;
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->preinfo = h->info = chain->info;
    h->info.opaque = NULL;
    char **opts = NULL;
    AVDictionary *avopts = NULL;
    if( h->info.channels <= 0 || h->info.samplerate <= 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid input audio format\n" );
        goto error;
    }

    static const char * const optlist[] = { AUDIO_CODEC_COMMON_OPTIONS, "profile", "cutoff", NULL };
    opts = x264_split_options( opt_str, optlist );
    if( !opts )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "wrong audio options.\n" );
        goto error;
    }

    const char *codecname = x264_get_option( "codec", opts );
    if( !codecname )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "codec not specified" );
        goto error;
    }

    // avcodec_register_all() removed in FFmpeg 8.x

    const AVCodec *codec = NULL;
    if( is_encoder_available( codecname, (void **)&codec ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "codec %s not supported or compiled in\n", codecname );
        goto error;
    }

    int i, j;
    h->info.codec_name = NULL;
    for( i = 0; ffcodecs[i].id != AV_CODEC_ID_NONE; i++ )
    {
        if( codec->id == ffcodecs[i].id )
        {
            h->info.codec_name = ffcodecs[i].name;
            break;
        }
    }
    if( !h->info.codec_name )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "failed to set codec name for muxer\n" );
        goto error;
    }

    // FFmpeg 8.x: Get sample formats using helper function
    enum AVSampleFormat sample_fmts[64];
    int num_fmts = get_codec_sample_fmts( codec, sample_fmts, 64 );

    for( j = 0; j < num_fmts && sample_fmts[j] != AV_SAMPLE_FMT_NONE; j++ )
    {
        // Prefer floats...
        if( sample_fmts[j] == AV_SAMPLE_FMT_FLT )
        {
            h->smpfmt = AV_SAMPLE_FMT_FLT;
            break;
        }
        else if( h->smpfmt < sample_fmts[j] ) // or the best possible sample format (is this really The Right Thing?)
            h->smpfmt = sample_fmts[j];
    }

    // FFmpeg 8.x: Handle channel layout - set default if not provided
    if( h->info.chanlayout == 0 )
    {
        // Will set default channel layout after context allocation
    }

    h->ctx                  = avcodec_alloc_context3( NULL );
    if( !h->ctx )
        goto error;
    h->ctx->sample_fmt      = h->smpfmt;
    h->ctx->sample_rate     = h->info.samplerate;

    // FFmpeg 8.x: Use ch_layout instead of channels/channel_layout
    if( h->info.chanlayout != 0 )
    {
        if( av_channel_layout_from_mask( &h->ctx->ch_layout, h->info.chanlayout ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "invalid audio channel layout\n" );
            goto error;
        }
    }
    else
    {
        av_channel_layout_default( &h->ctx->ch_layout, h->info.channels );
        if( get_channel_layout_mask( &h->ctx->ch_layout, &h->info.chanlayout ) )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "invalid audio channel layout\n" );
            goto error;
        }
    }

    h->ctx->time_base       = (AVRational){ 1, h->ctx->sample_rate };

    if( set_avdict_option( &avopts, "flags", "global_header" ) || // aac
        set_avdict_option( &avopts, "strict", "-2" ) ||           // aac
        set_avdict_option( &avopts, "reservoir", "1" ) )          // mp3
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "failed to set encoder options\n" );
        goto error;
    }

    char *acutoff = x264_otos( x264_get_option( "cutoff", opts ), NULL );
    if( acutoff && set_avdict_option( &avopts, "cutoff", acutoff ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "failed to set encoder options\n" );
        goto error;
    }

    char *aprofile = x264_otos( x264_get_option( "profile", opts ), NULL );
    if( !strcmp(codecname, "libaacplus") )
        aprofile = "aac_he";
    if( aprofile )
    {
        if( set_avdict_option( &avopts, "profile", aprofile ) )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "failed to set encoder options\n" );
            goto error;
        }
        if( ISCODEC( aac ) && strstr( aprofile, "he" ) )
        {
            audio_aac_info_t *aacinfo = malloc( sizeof( audio_aac_info_t ) );
            if( !aacinfo )
                goto error;
            aacinfo->has_sbr          = 1;
            h->info.opaque            = aacinfo;
        }
    }
    else if( ISCODEC( aac ) )
    {
        if( set_avdict_option( &avopts, "profile", "aac_low" ) ) // TODO: decide by bitrate / quality
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "failed to set encoder options\n" );
            goto error;
        }
    }

    int is_vbr;
    if( parse_bool_option( x264_get_option( "is_vbr", opts ), ffcodecs[i].mode & MODE_VBR ? 1 : 0, &is_vbr ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid is_vbr option\n" );
        goto error;
    }

    if( ( ( !(ffcodecs[i].mode & MODE_BITRATE) && !is_vbr ) || ( !(ffcodecs[i].mode & MODE_VBR) && is_vbr ) ) && ( strcmp(codecname, "libfdk_aac") ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "libavcodec's %s encoder doesn't allow %s mode.\n", codecname, is_vbr ? "VBR" : "bitrate" );
        goto error;
    }

    float default_brval = is_vbr ? ffcodecs[i].default_brval : ffcodecs[i].default_brval * h->ctx->ch_layout.nb_channels;
    float brval;
    if( parse_float_option( x264_get_option( "bitrate", opts ), default_brval, &brval ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid bitrate option\n" );
        goto error;
    }

    float compression_level;
    if( parse_float_option( x264_get_option( "quality", opts ), FF_COMPRESSION_DEFAULT, &compression_level ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid quality option\n" );
        goto error;
    }
    double compression_level_value = compression_level;
    if( !(compression_level_value >= INT_MIN && compression_level_value <= INT_MAX) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid quality option\n" );
        goto error;
    }
    int codec_compression_level = (int)compression_level;
    h->ctx->compression_level = codec_compression_level;

    free( opts );
    opts = NULL;

    if( is_vbr )
    {
        if( set_avdict_option( &avopts, "flags", "qscale" ) )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "failed to set encoder options\n" );
            goto error;
        }
        double global_quality_value = (double)FF_QP2LAMBDA * (double)brval;
        if( !(global_quality_value >= INT_MIN && global_quality_value <= INT_MAX) )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "invalid bitrate option\n" );
            goto error;
        }
        int codec_global_quality = (int)global_quality_value;
        h->ctx->global_quality = codec_global_quality;
    }
    else
    {
        long double bit_rate_value = (long double)brval * 1000.0L;
        if( !(bit_rate_value >= INT64_MIN && bit_rate_value <= INT64_MAX) )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "invalid bitrate option\n" );
            goto error;
        }
        int64_t codec_bit_rate = llrintl( bit_rate_value );
        h->ctx->bit_rate = codec_bit_rate;
    }

    if( avcodec_open2( h->ctx, codec, &avopts ) )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "could not open the %s encoder\n", codec->name );
        goto error;
    }
    av_dict_free( &avopts );

    /* Set up frame buffer. */
    h->frame = av_frame_alloc();
    if( !h->frame )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "av_frame_alloc failed!\n" );
        goto error;
    }

    if( h->ctx->frame_size == 0 )
        /* frame_size == 0 indicates the actual frame size is based on the buf_size passed to avcodec_encode_audio2(). */
        h->ctx->frame_size = 1; /* arbitrary */

    h->frame->format         = h->ctx->sample_fmt;
    // FFmpeg 8.x: Use ch_layout instead of channel_layout
    if( av_channel_layout_copy( &h->frame->ch_layout, &h->ctx->ch_layout ) < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "failed to copy audio channel layout\n" );
        goto error;
    }
    h->frame->nb_samples     = h->ctx->frame_size;
    h->frame->sample_rate    = h->ctx->sample_rate;

    if( avcodec_default_get_buffer2( h->ctx, h->frame, 0 ) < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "could not get frame buffer\n" );
        goto error;
    }

    /* Set up resampler. */
    h->avr = swr_alloc();
    if( !h->avr )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "swr_alloc failed!\n" );
        goto error;
    }

    if( av_opt_set_int( h->avr, "in_channel_layout",  h->info.chanlayout,       0 ) < 0 ||
        av_opt_set_sample_fmt( h->avr, "in_sample_fmt",   AV_SAMPLE_FMT_FLTP,   0 ) < 0 ||
        av_opt_set_int( h->avr, "in_sample_rate",     h->info.samplerate,       0 ) < 0 ||
        av_opt_set_int( h->avr, "out_channel_layout", h->info.chanlayout,       0 ) < 0 ||
        av_opt_set_sample_fmt( h->avr, "out_sample_fmt",  h->frame->format,     0 ) < 0 ||
        av_opt_set_int( h->avr, "out_sample_rate",    h->frame->sample_rate,    0 ) < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "could not configure resampler\n" );
        goto error;
    }

    if( swr_init( h->avr ) < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "could not open resampler\n" );
        goto error;
    }

    h->info.framelen        = h->ctx->frame_size;
    h->info.timebase        = (timebase_t) { 1, h->ctx->sample_rate };
    h->info.depth           = av_get_bits_per_sample( h->ctx->codec->id );
    h->info.chansize        = IS_LPCM_CODEC_ID( h->ctx->codec->id )
                            ? h->info.depth / 8
                            : av_get_bytes_per_sample( h->ctx->sample_fmt );
    if( h->info.chansize <= 0 || h->info.chansize > INT_MAX / 8 ||
        h->ctx->ch_layout.nb_channels <= 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid audio sample size\n" );
        goto error;
    }
    int64_t samplesize64 = (int64_t)h->info.chansize * h->ctx->ch_layout.nb_channels;
    if( samplesize64 > INT_MAX )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid audio sample size\n" );
        goto error;
    }
    int samplesize = (int)samplesize64;
    h->info.samplesize      = samplesize;
    if( set_framesize( &h->info, "lavc" ) )
        goto error;
    uint32_t initial_last_delta = (uint32_t)h->info.framelen;
    h->info.last_delta      = initial_last_delta;

    int buf_size = av_samples_get_buffer_size( NULL, h->ctx->ch_layout.nb_channels, h->ctx->frame_size, h->ctx->sample_fmt, 0 );
    int64_t scaled_buf_size = (int64_t)buf_size * 3 / 2;
    if( buf_size <= 0 || scaled_buf_size > INT_MAX )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid audio buffer size\n" );
        goto error;
    }
    int scaled_output_buf_size = (int)scaled_buf_size;
    h->buf_size = scaled_output_buf_size;
    h->last_dts = INVALID_DTS;

    if( h->ctx->extradata_size < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "invalid audio extradata size\n" );
        goto error;
    }
    int extradata_size = h->ctx->extradata_size;
    h->info.extradata       = h->ctx->extradata;
    h->info.extradata_size  = extradata_size;

    int sample_bits = h->info.chansize * 8;
    x264_cli_log( "audio", X264_LOG_INFO, "opened libavcodec's %s encoder (%s%.1f%s, %dbits, %dch, %dhz)\n", codec->name,
                  is_vbr ? "V" : "", brval, is_vbr ? "" : "kbps", sample_bits, h->info.channels, h->info.samplerate );
    return h;

error:
    free( opts );
    if( avopts )
        av_dict_free( &avopts );
    if( h->ctx )
    {
        avcodec_free_context( &h->ctx );
    }
    if( h->frame )
        av_frame_free( &h->frame );
    if( h->avr )
        swr_free( &h->avr );
    if( h->info.opaque )
        free( h->info.opaque );
    free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    if( !handle )
        return NULL;
    enc_lavc_t *h = handle;

    return &h->info;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lavc_t *h = handle;
    if( !h || !h->filter_chain || !h->ctx || !h->frame || !h->avr || h->buf_size <= 0 )
        return NULL;
    if( h->finishing )
        return NULL;

    audio_packet_t *smp = NULL;
    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
        return NULL;
    out->info = h->info;
    out->data = malloc( h->buf_size );
    if( !out->data )
        goto error;

    while( out->size == 0 )
    {
        int64_t last_sample;
        if( get_sample_end( h->last_sample, h->info.framelen, &last_sample ) )
            goto error;

        smp = x264_af_get_samples( h->filter_chain, h->last_sample, last_sample );
        if( !smp )
            goto error; // not an error but need same handling

        out->samplecount = smp->samplecount;
        out->channels    = smp->channels;
        int staged_finishing = h->finishing;
        uint32_t staged_last_delta = h->info.last_delta;
        int original_frame_size = h->ctx->frame_size;
        int staged_frame_size = original_frame_size;

        /* x264_af_interleave2 allocates only samplecount * channels * bytes per sample per single channel, */
        /* but lavc expects sample buffer contains h->ctx->frame_size samples (at least, alac encoder does). */
        /* If codec has capabilitiy to accept samples < default frame length, need to modify frame_size to */
        /* specify real sample counts in sample buffer, and if not, need to padding buffer. */
        if( smp->samplecount < h->info.framelen )
        {
            staged_finishing = 1;
            if( !(smp->flags & AUDIO_FLAG_EOF) )
            {
                x264_cli_log( "lavc", X264_LOG_ERROR, "samples too few but not EOF???\n" );
                goto error;
            }

            if( h->ctx->codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME )
            {
                staged_frame_size = (int)smp->samplecount;
                staged_last_delta = smp->samplecount;
            }
            else
            {
                if( x264_af_resize_fill_buffer( smp->samples, h->info.framelen, h->info.channels, smp->samplecount, 0.0f ) )
                {
                    x264_cli_log( "lavc", X264_LOG_ERROR, "failed to expand buffer.\n" );
                    goto error;
                }
                smp->samplecount = h->info.framelen;
                staged_last_delta = h->info.framelen;
            }
        }

        if( smp->samplecount > INT_MAX ||
            smp->samplecount > (uint64_t)(INT64_MAX - h->last_sample) )
            goto error;
        int64_t staged_last_sample = h->last_sample + smp->samplecount;
        h->ctx->frame_size = staged_frame_size;
        h->frame->nb_samples  = (int)smp->samplecount;

        if( resample_audio( h->avr, h->frame, smp ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "error resampling audio!\n" );
            h->ctx->frame_size = original_frame_size;
            goto error;
        }

        if( encode_audio( h->ctx, out, h->buf_size, h->frame ) < 0 )
        {
            x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
            h->ctx->frame_size = original_frame_size;
            goto error;
        }

        h->finishing = staged_finishing;
        h->info.last_delta = staged_last_delta;
        h->ctx->frame_size = staged_frame_size;
        if( h->last_dts == INVALID_DTS )
            h->last_dts = h->last_sample;
        h->last_sample = staged_last_sample;

        x264_af_free_packet( smp );
        smp = NULL;
    }

    if( out->size < 0 )
    {
        x264_cli_log( "lavc", X264_LOG_ERROR, "error encoding audio! (%s)\n", strerror( -out->size ) );
        goto error;
    }

    int64_t staged_last_dts = h->last_dts == INVALID_DTS ? h->last_sample : h->last_dts;
    out->dts     = staged_last_dts;
    out->info    = h->info;
    if( add_info_delta_dts( &staged_last_dts, &h->info ) )
        goto error;
    h->last_dts = staged_last_dts;
    return out;

error:
    if( smp )
        x264_af_free_packet( smp );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    enc_lavc_t *h = handle;
    if( !h )
        return;
    add_skip_samples( &h->last_sample, samplecount );
}

static audio_packet_t *finish( hnd_t handle )
{
    enc_lavc_t *h = handle;
    if( !h || !h->ctx || h->buf_size <= 0 )
        return NULL;

    h->finishing = 1;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
        return NULL;
    out->info     = h->info;
    out->channels = h->info.channels;
    out->data     = malloc( h->buf_size );
    if( !out->data )
        goto error;

    if( encode_audio( h->ctx, out, h->buf_size, NULL ) < 0 )
        goto error;

    if( out->size <= 0 )
        goto error;

    int64_t staged_last_dts = h->last_dts == INVALID_DTS ? h->last_sample : h->last_dts;
    out->dts = staged_last_dts;
    out->samplecount = h->info.last_delta ? h->info.last_delta : h->info.framelen;
    out->info = h->info;
    if( add_info_delta_dts( &staged_last_dts, &h->info ) )
        goto error;
    h->last_dts = staged_last_dts;
    return out;

error:
    x264_af_free_packet( out );
    return NULL;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    if( !packet )
        return;
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static void lavc_close( hnd_t handle )
{
    enc_lavc_t *h = handle;
    if( !h )
        return;

    // FFmpeg 8.x: Use avcodec_free_context instead of avcodec_close + av_free
    if( h->ctx )
        avcodec_free_context( &h->ctx );
    if( h->frame )
        av_frame_free( &h->frame );
    if( h->avr )
        swr_free( &h->avr );
    if( h->info.opaque )
        free( h->info.opaque );
    free( h );
}

static void lavc_help_aac( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        --aquality        VBR quality\n" );
    printf( "                          Cannot be used for HE-AAC and possible values are:\n" );
    printf( "                             1, 2, 3, 4, 5\n" );
    printf( "                          1 is lowest and 5 is highest.\n" );
    printf( "        --abitrate        Enables bitrate mode [192]\n" );
    printf( "                          Bitrate should be one of the discrete preset values depending on\n" );
    printf( "                          profile, channels count, and samplerate.\n" );
    printf( "                          Examples for typical configurations\n" );
    printf( "                           - for 44100Hz to 48000Hz with 1ch\n" );
    printf( "                             LC: 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256\n" );
    printf( "                             HE: 12, 16, 24, 32, 40\n" );
    printf( "                           - for 44100Hz to 48000Hz with 2ch\n" );
    printf( "                             LC: 64, 72, 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320, 384, 512\n" );
    printf( "                             HE: 32, 40, 48, 56, 64, 80, 96, 112, 128\n" );
    printf( "                           - for 64000Hz to 96000Hz with 2ch\n" );
    printf( "                             LC: 80, 96, 112, 128, 144, 160, 192, 224, 256, 288, 320, 384, 512, 768, 1024\n" );
    printf( "                             HE: 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 256\n" );
    printf( "                           - for 44100Hz to 48000Hz with 5.1ch\n" );
    printf( "                             LC: 160, 192, 224, 256, 288, 320, 384, 448, 512, 576, 640, 768, 1024, 1280\n");
    printf( "                             HE: 80, 96, 112, 128, 160, 192, 256\n");
    printf( "                           - for 64000Hz to 96000Hz with 5.1ch\n" );
    printf( "                             LC: 256, 288, 320, 384, 448, 512, 576, 640, 768, 1024, 1280, 1600, 1920, 2560\n");
    printf( "                             HE: 128, 160, 192, 256, 320, 384, 512, 640\n");
    printf( "                          The lower samplerate, the lower min/max values are applied\n" );
    printf( "        --aextraopt       Profile and bitrate mode\n" );
    printf( "                             cutoff: set cutoff in Hz\n" );
    printf( "                             profile : profile for aac codec [\"aac_low\"]\n" );
    printf( "                                       \"aac_low\", \"aac_he\"\n" );
    printf( "\n" );
}

static void lavc_help_amrnb( const char * const encoder_name )
{
    printf( "      * (ff)%s encoder help\n", encoder_name );
    printf( "        Accepts only mono (1ch), 8000Hz audio and not capable of quality based VBR\n" );
    printf( "        --abitrate        Only one of the values below can be acceptable\n" );
    printf( "                             4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2\n" );
    printf( "\n" );
}

static void lavc_help( const char * const encoder_name )
{
    const AVCodec *enc = NULL;

    if( is_encoder_available( encoder_name, (void **)&enc ) )
        return;

#define SHOWHELP( encoder, helpname ) if( !strcmp( enc->name, #encoder ) ) lavc_help_##helpname ( #encoder );
    SHOWHELP( libfdk_aac, aac );
    SHOWHELP( libopencore_amrnb, amrnb );
#undef SHOWHELP

    return;
}

const audio_encoder_t audio_encoder_lavc =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = lavc_close,
    .show_help       = lavc_help,
    .is_valid_encoder = is_encoder_available
};
