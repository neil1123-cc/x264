#include "audio/encoders.h"
#include "filters/audio/internal.h"

typedef struct enc_raw_t
{
    audio_info_t info;
    int finishing;
    hnd_t filter_chain;
    int64_t last_sample;
} enc_raw_t;

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

static void add_skip_samples( int64_t *last_sample, uint64_t samplecount )
{
    if( *last_sample >= 0 && samplecount <= (uint64_t)INT64_MAX - (uint64_t)*last_sample )
        *last_sample += samplecount;
    else
        *last_sample = INT64_MAX;
}

static hnd_t init( hnd_t filter_chain, const char *opts )
{
    if( !filter_chain )
        return NULL;
    enc_raw_t *h = calloc( 1, sizeof( enc_raw_t ) );
    if( !h )
        return NULL;
    audio_hnd_t *chain = h->filter_chain = filter_chain;
    h->info = chain->info;

    h->info.codec_name     = "raw";
    h->info.extradata      = NULL;
    h->info.extradata_size = 0;
    h->info.framelen       = h->info.framelen ? h->info.framelen : 1;
    h->info.depth          = 16;
    h->info.chansize       = h->info.depth / 8;
    if( h->info.channels <= 0 || h->info.samplerate <= 0 ||
        h->info.channels > INT_MAX / h->info.chansize )
    {
        free( h );
        return NULL;
    }
    h->info.samplesize     = h->info.chansize * h->info.channels;
    if( set_framesize( &h->info, "raw" ) )
    {
        free( h );
        return NULL;
    }
    h->info.timebase       = (timebase_t) { 1, h->info.samplerate };

    x264_cli_log( "audio", X264_LOG_INFO, "opened raw encoder (%dbits, %dch, %dhz)\n",
                  h->info.chansize * 8, h->info.channels, h->info.samplerate );
    return h;
}

static audio_info_t *get_info( hnd_t handle )
{
    if( !handle )
        return NULL;
    enc_raw_t *h = handle;

    return &h->info;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_raw_t *h = handle;
    if( !h || !h->filter_chain )
        return NULL;
    if( h->finishing )
        return NULL;

    int64_t last_sample;
    if( get_sample_end( h->last_sample, h->info.framelen, &last_sample ) )
        return NULL;

    audio_packet_t *smp = x264_af_get_samples( h->filter_chain, h->last_sample, last_sample );
    if( !smp )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
    {
        x264_af_free_packet( smp );
        return NULL;
    }
    memcpy( out, smp, sizeof( audio_packet_t ) );
    out->info        = h->info;
    out->owner       = NULL;
    out->priv        = NULL;
    out->samples     = NULL;
    out->data        = NULL;
    out->samplecount = smp->samplecount;
    if( h->info.samplesize <= 0 ||
        smp->samplecount > (unsigned)(INT_MAX / h->info.samplesize) )
        goto error;
    out->data        = x264_af_interleave2( SMPFMT_S16, smp->samples, smp->channels, smp->samplecount );
    if( smp->samplecount && !out->data )
        goto error;
    out->size        = smp->samplecount * h->info.samplesize;
    out->dts         = h->last_sample;
    out->info.last_delta = smp->samplecount;
    h->last_sample  += smp->samplecount;
    x264_af_free_packet( smp );

    return out;

error:
    x264_af_free_packet( smp );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    enc_raw_t *h = handle;
    if( !h )
        return;
    add_skip_samples( &h->last_sample, samplecount );
}

static audio_packet_t *finish( hnd_t handle )
{
    enc_raw_t *h = handle;
    if( !h )
        return NULL;
    h->finishing = 1;
    return NULL;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    x264_af_free_packet( packet );
}

static void raw_close( hnd_t handle )
{
    free( handle );
}

static void raw_help( const char * const codec_name )
{
    printf( "      * raw encoder help\n" );
    printf( "        Directly pass the decoded PCM samples (in native endian) to muxer.\n" );
    printf( "        All audio options except for --acodec and --audiofile are ignored.\n" );
    printf( "\n" );
}

const audio_encoder_t audio_encoder_raw =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = raw_close,
    .show_help       = raw_help,
    .is_valid_encoder = NULL
};
