#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include "lame/lame.h"
#include <float.h>

typedef struct enc_lame_t
{
    audio_info_t info;
    hnd_t filter_chain;
    int64_t packet_count;

    int finishing;
    lame_global_flags *lame;
    int64_t last_sample;
    int64_t last_dts;
    uint8_t *buffer;
    size_t buf_index;
    size_t bufsize;
    audio_packet_t *in;
} enc_lame_t;

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

static void add_skip_samples( int64_t *last_sample, uint64_t samplecount )
{
    if( *last_sample >= 0 && samplecount <= (uint64_t)INT64_MAX - (uint64_t)*last_sample )
        *last_sample += samplecount;
    else
        *last_sample = INT64_MAX;
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

static int parse_int_option( const char *opt, int def, int *dst )
{
    if( !opt )
    {
        *dst = def;
        return 0;
    }
    return x264_otoi_checked( opt, dst );
}

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    if( !filter_chain )
        return NULL;
    audio_hnd_t *chain = filter_chain;
    if( chain->info.channels <= 0 || chain->info.samplerate <= 0 )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "invalid input audio parameters\n" );
        return 0;
    }
    if( chain->info.channels > 2 )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "only mono or stereo audio is supported\n" );
        return 0;
    }

    static const char * const optlist[] = { AUDIO_CODEC_COMMON_OPTIONS, "samplerate", NULL };
	char **opts = x264_split_options( opt_str, optlist);
    if( !opts )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "wrong audio options.\n" );
        return 0;
    }

    enc_lame_t *h   = calloc( 1, sizeof( enc_lame_t ) );
    if( !h )
    {
        free( opts );
        return NULL;
    }
    h->filter_chain = chain;
    h->info         = chain->info;
    h->info.codec_name = "mp3";

    int is_vbr;
    if( parse_bool_option( x264_get_option( "is_vbr", opts ), 1, &is_vbr ) )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "invalid is_vbr option\n" );
        goto error;
    }
    float brval;
    if( parse_float_option( x264_get_option( "bitrate", opts ), is_vbr ? 6.0 : 128.0, &brval ) )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "invalid bitrate option\n" );
        goto error;
    }

    float quality_value;
    if( parse_float_option( x264_get_option( "quality", opts ), 0.0, &quality_value ) )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "invalid quality option\n" );
        goto error;
    }
    if( !(quality_value >= INT_MIN && quality_value <= INT_MAX) )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "invalid quality option\n" );
        goto error;
    }
    int quality = (int)quality_value;

    if( parse_int_option( x264_get_option( "samplerate", opts ), chain->info.samplerate, &h->info.samplerate ) ||
        h->info.samplerate <= 0 )
    {
        x264_cli_log( "lame", X264_LOG_ERROR, "invalid samplerate option\n" );
        goto error;
    }

    free( opts );
    opts = NULL;

    h->info.extradata      = NULL;
    h->info.extradata_size = 0;

    h->lame = lame_init();
    if( !h->lame )
        goto error;
    // lame expects floats to be in the same range as shorts, our floats are -1..1 so tell it to scale
    lame_set_scale( h->lame, 32768 );
    lame_set_in_samplerate( h->lame, chain->info.samplerate );
    lame_set_out_samplerate( h->lame, h->info.samplerate );
    lame_set_num_channels( h->lame, h->info.channels );
    lame_set_quality( h->lame, quality );
    lame_set_VBR( h->lame, vbr_default );

    if( !is_vbr )
    {
        if( !(brval >= INT_MIN && brval <= INT_MAX) )
        {
            x264_cli_log( "lame", X264_LOG_ERROR, "invalid bitrate option\n" );
            goto error;
        }
        int bitrate_kbps = (int)brval;
        lame_set_VBR( h->lame, vbr_off );
        lame_set_brate( h->lame, bitrate_kbps );
    }
    else
        lame_set_VBR_quality( h->lame, brval );

    lame_set_bWriteVbrTag( h->lame, 0 );

    if( lame_init_params( h->lame ) < 0 )
        goto error;

    h->info.framelen   = lame_get_framesize( h->lame );
    h->info.chansize   = 2;
    if( h->info.framelen <= 0 )
        goto error;
    int64_t samplesize64 = (int64_t)h->info.chansize * h->info.channels;
    if( h->info.chansize <= 0 || h->info.chansize > INT_MAX / 8 || samplesize64 > INT_MAX )
        goto error;
    int samplesize = (int)samplesize64;
    h->info.samplesize = samplesize;
    if( set_framesize( &h->info, "lame" ) )
        goto error;
    h->info.depth      = 16;
    h->info.timebase   = (timebase_t) { 1, h->info.samplerate };
    h->info.last_delta = h->info.framelen;

    if( (uint64_t)h->info.framelen > (SIZE_MAX - 7200) / 125 )
        goto error;
    h->bufsize = (size_t)125 * (size_t)h->info.framelen / 100 + 7200; // from lame.h, largest frame that the encoding functions may return
    if( h->bufsize > INT_MAX )
        goto error;
    h->buffer = malloc( h->bufsize );
    if( !h->buffer )
        goto error;
    h->buf_index = 0;
    h->last_dts = INVALID_DTS;

    x264_cli_log( "audio", X264_LOG_INFO, "opened lame mp3 encoder (%s: %g%s, quality: %d, samplerate: %dhz)\n",
                  ( !is_vbr ? "bitrate" : "VBR" ), brval,
                  ( !is_vbr ? "kbps" : "" ), quality, h->info.samplerate );

    return h;

error:
    free( opts );
    if( h->lame )
        lame_close( h->lame );
    free( h->buffer );
    free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    if( !handle )
        return NULL;
    enc_lame_t *h = handle;

    return &h->info;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    if( !packet )
        return;
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

/* Ripped from ffmpeg's libmp3lame.c */
static const int samplerates_tab[] = {
    44100, 48000,  32000, 22050, 24000, 16000, 11025, 12000, 8000, 0,
};

// frame bitrate table in kbits/s
static const int bitrate_tab[2][3][15] = {
    {  {   0,  32,  64,  96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
       {   0,  32,  48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384},
       {   0,  32,  40,  48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320},
    },
    {  {   0,  32,  48,  56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256},
       {   0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160},
       {   0,   8,  16,  24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160},
    },
};

// frame length in samples/frame
static const int samples_per_frame_tab[2][3] = {
    {  384,     1152,    1152 },
    {  384,     1152,     576 },
};

static const int bits_per_slot_tab[3] = {
    32, 8, 8,
};

static int const mode_tab[4] = {
    2, 3, 1, 0,
};

static int mp3len( uint8_t *data )
{
    uint32_t header       = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    int layer_id          = 3 - ((header >> 17) & 0x03);
    int bitrate_id        = ((header >> 12) & 0x0f);
    int samplerate_id     = ((header >> 10) & 0x03);
    int mode              = mode_tab[(header >> 19) & 0x03];
    int mpeg_id           = !!(mode > 0) ;
    int is_pad            = ((header >> 9) & 0x01);

    if ( (( header >> 21 ) & 0x7ff) != 0x7ff || mode == 3 || layer_id == 3 ||
         bitrate_id == 0 || bitrate_id == 15 || samplerate_id == 3 ) {
        return -1;
    }

    int bits_per_slot     = bits_per_slot_tab[layer_id];
    int samplerate        = samplerates_tab[samplerate_id]>>mode;
    int samples_per_frame = samples_per_frame_tab[mpeg_id][layer_id];
    int bitrate           = bitrate_tab[mpeg_id][layer_id][bitrate_id]*1000;

    return samples_per_frame * bitrate / (bits_per_slot * samplerate) + is_pad;
}

static int get_next_mp3frame( hnd_t handle, uint8_t *data )
{
    enc_lame_t *h = handle;
    if( !h || !data || !h->buffer )
        return 0;
    int outlen;

    if( h->buf_index < 4 )
        return 0;

    outlen = mp3len( h->buffer );

    if( (outlen <= 0) || (outlen > h->buf_index) )
        return 0;

    memcpy( data, h->buffer, outlen );
    h->buf_index -= outlen;
    memmove( h->buffer, h->buffer+outlen, h->buf_index);

    return outlen;
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_lame_t *h = handle;
    if( !h || !h->filter_chain || !h->lame || !h->buffer || !h->bufsize )
        return NULL;
    int len;

    if( h->finishing )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
        return NULL;
    out->info = h->info;
    out->data = malloc( h->bufsize );
    if( !out->data )
        goto error;

    do
    {
        if( h->in && h->in->flags & AUDIO_FLAG_EOF )
        {
            h->finishing = 1;
            goto error; // Not an error here but it'd do the same handling
        }
        x264_af_free_packet( h->in );
        h->in = NULL;

        int64_t last_sample;
        if( get_sample_end( h->last_sample, h->info.framelen, &last_sample ) )
            goto error;

        if( !( h->in = x264_af_get_samples( h->filter_chain, h->last_sample, last_sample ) ) )
            goto error;

        if( h->buf_index > h->bufsize )
            goto error;
        if( h->in->channels != (unsigned)h->info.channels ||
            !h->in->samples || !h->in->samples[0] ||
            (h->info.channels > 1 && !h->in->samples[1]) )
            goto error;
        if( h->in->samplecount > INT_MAX ||
            h->in->samplecount > (uint64_t)(INT64_MAX - h->last_sample) )
            goto error;
        int64_t staged_last_sample = h->last_sample + h->in->samplecount;
        float *right = h->info.channels > 1 ? h->in->samples[1] : h->in->samples[0];
        len = lame_encode_buffer_float( h->lame, h->in->samples[0], right,
                                        h->in->samplecount, h->buffer + h->buf_index, h->bufsize - h->buf_index );

        if( len < 0 || (size_t)len > h->bufsize - h->buf_index )
            goto error;

        h->buf_index += len;
        if( h->last_dts == INVALID_DTS )
            h->last_dts = h->last_sample;
        h->last_sample = staged_last_sample;

        out->size = get_next_mp3frame( h, out->data );
    } while( !out->size );

    int64_t staged_last_dts = h->last_dts == INVALID_DTS ? h->last_sample : h->last_dts;
    out->dts = staged_last_dts;
    if( add_frame_dts( &staged_last_dts, h->info.framelen ) )
        goto error;
    h->last_dts = staged_last_dts;
    return out;

error:
    if( h->in )
    {
        x264_af_free_packet( h->in );
        h->in = NULL;
    }
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    enc_lame_t *h = handle;
    if( !h )
        return;
    add_skip_samples( &h->last_sample, samplecount );
}

static audio_packet_t *finish( hnd_t encoder )
{
    enc_lame_t *h = encoder;
    if( !h || !h->lame || !h->buffer || !h->bufsize )
        return NULL;
    int len;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
        return NULL;
    out->info = h->info;
    out->data = malloc( h->bufsize );
    if( !out->data )
        goto error;

    if( h->buf_index > h->bufsize )
        goto error;
    len = lame_encode_flush( h->lame, h->buffer + h->buf_index, h->bufsize - h->buf_index );
    if( len < 0 || (size_t)len > h->bufsize - h->buf_index )
        goto error;

    h->buf_index += len;

    out->size = get_next_mp3frame( h, out->data );
    if( !out->size )
        goto error;

    int64_t staged_last_dts = h->last_dts == INVALID_DTS ? h->last_sample : h->last_dts;
    out->dts = staged_last_dts;
    if( add_frame_dts( &staged_last_dts, h->info.framelen ) )
        goto error;
    h->last_dts = staged_last_dts;
    return out;

error:
    x264_af_free_packet( out );
    return NULL;
}

static void mp3_close( hnd_t handle )
{
    enc_lame_t *h = handle;
    if( !h )
        return;

    if( h->lame )
        lame_close( h->lame );
    if( h->in )
        x264_af_free_packet( h->in );
    if( h->buffer )
        free( h->buffer );
    free( h );
}

static void mp3_help( const char * const encoder_name )
{
    printf( "      * lame encoder help\n" );
    printf( "        --aquality        VBR quality [6]\n" );
    printf( "                             9 (lowest) to 0 (highest)\n" );
    printf( "        --abitrate        Enables CBR mode. Bitrate should be one of the values below\n" );
    printf( "                           - for 32000Hz or 44100Hz or 48000Hz\n" );
    printf( "                             32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320\n" );
    printf( "                           - for 16000Hz or 22050Hz or 24000Hz\n" );
    printf( "                             8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160\n" );
    printf( "                           - for 8000Hz or 11025Hz or 12000Hz\n" );
    printf( "                             8, 16, 24, 32, 40, 48, 56, 64\n" );
    printf( "        --asamplerate     Output samplerate. Should be one of the values below\n" );
    printf( "                             8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000\n" );
    printf( "        --acodec-quality  Internal algorithmic complexity [0]\n" );
    printf( "                             9 (poor quality) to 0 (best quality)\n" );
    printf( "\n" );
}

const audio_encoder_t audio_encoder_lame =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = mp3_close,
    .show_help       = mp3_help,
    .is_valid_encoder = NULL
};
