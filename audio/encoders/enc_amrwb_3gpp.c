#include "audio/encoders.h"
#include "filters/audio/internal.h"

#include "typedef.h"
#include "enc_if.h"
#include <math.h>
#include <float.h>

typedef struct enc_amrwb_3gpp_t
{
    audio_info_t info;
    hnd_t filter_chain;
    int64_t packet_count;

    void* amrwb_3gpp;
    Word16 dtx;
    Word16 mode;

    int finishing;
    int64_t last_sample;
    int64_t last_dts;
    size_t bufsize;
} enc_amrwb_3gpp_t;

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

static hnd_t init( hnd_t filter_chain, const char *opt_str )
{
    if( !filter_chain )
        return NULL;
    audio_hnd_t *chain = filter_chain;

    if( chain->info.channels != 1 )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "only 1ch is supported\n" );
        return NULL;
    }
    if( chain->info.samplerate != 16000 )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "only 16kHz is supported\n" );
        return NULL;
    }

	static const char * const optlist[] = { AUDIO_CODEC_COMMON_OPTIONS, "dtx", NULL };
    char **opts = x264_split_options( opt_str, optlist);
    if( !opts )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "wrong audio options.\n" );
        return NULL;
    }

    enc_amrwb_3gpp_t *h     = calloc( 1, sizeof( enc_amrwb_3gpp_t ) );
    if( !h )
    {
        free( opts );
        return NULL;
    }
    h->filter_chain         = chain;
    h->info                 = chain->info;
    h->info.codec_name      = "amrwb";
    h->info.chansize        = 2;
    h->info.samplesize      = 2 * h->info.channels;
    h->info.framelen        = L_FRAME16k;
    if( set_framesize( &h->info, "amrwb_3gpp" ) )
        goto error;
    h->info.depth           = 16;
    h->info.timebase        = (timebase_t) { 1, h->info.samplerate };
    h->info.last_delta      = h->info.framelen;
    h->bufsize              = NB_SERIAL_MAX;

    h->info.extradata       = NULL; /* These are created in muxer modules. AMR-WB does not have general structure. */
    h->info.extradata_size  = 0;

    int dtx;
    float bitrate;
    if( parse_bool_option( x264_get_option( "dtx", opts ), 1, &dtx ) )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "invalid dtx option\n" );
        goto error;
    }
    h->dtx = (Word16)dtx;
    if( parse_float_option( x264_get_option( "bitrate", opts ), 23.85, &bitrate ) )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "invalid bitrate option\n" );
        goto error;
    }
    switch( lrintf( bitrate ) )
    {
    case 7: /* 6.60 */
        h->mode = 0; break;
    case 9: /* 8.85 */
        h->mode = 1; break;
    case 13: /* 12.65 */
        h->mode = 2; break;
    case 14: /* 14.25 */
        h->mode = 3; break;
    case 16: /* 15.85 */
        h->mode = 4; break;
    case 18: /* 18.25 */
        h->mode = 5; break;
    case 20: /* 19.85 */
        h->mode = 6; break;
    case 23: /* 23.05 */
        h->mode = 7; break;
    case 24: /* 23.85 */
        h->mode = 8; break;
    default:
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "invalid bitrate (%f) .\n", bitrate );
        goto error;
        break;
    }

    free( opts );
    opts = NULL;

    h->amrwb_3gpp = E_IF_init();
    if( !h->amrwb_3gpp )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "failed to initialize 3gpp amrwb encoder.\n" );
        goto error;
    }

    h->last_dts = INVALID_DTS;

    const char *bitrate_tbl[] = { "6.60", "8.85", "12.65", "14.25", "15.85", "18.25", "19.85", "23.05", "23.85" };
    x264_cli_log( "audio", X264_LOG_INFO, "opened amrwb_3gpp encoder (%skbps, dtx=%s)\n",
                  bitrate_tbl[h->mode], h->dtx ? "on" : "off", h->info.samplerate );

    return h;

error:
    free( opts );
    if( h && h->amrwb_3gpp )
        E_IF_exit( h->amrwb_3gpp );
    if( h )
        free( h );
    return NULL;
}

static audio_info_t *get_info( hnd_t handle )
{
    enc_amrwb_3gpp_t *h = handle;
    return h ? &h->info : NULL;
}

static void free_packet( hnd_t handle, audio_packet_t *packet )
{
    if( !packet )
        return;
    packet->owner = NULL;
    x264_af_free_packet( packet );
}

static audio_packet_t *get_next_packet( hnd_t handle )
{
    enc_amrwb_3gpp_t *h = handle;
    audio_packet_t *in = NULL;

    if( !h || !h->filter_chain || !h->amrwb_3gpp || !h->bufsize || h->finishing )
        return NULL;

    audio_packet_t *out = calloc( 1, sizeof( audio_packet_t ) );
    if( !out )
        return NULL;
    out->info = h->info;
    out->data = malloc( h->bufsize );
    if( !out->data )
        goto error;

    if( h->finishing )
        goto error; // Not an error here but it'd do the same handling

    int64_t last_sample;
    if( get_sample_end( h->last_sample, h->info.framelen, &last_sample ) )
        goto error;

    if( !( in = x264_af_get_samples( h->filter_chain, h->last_sample, last_sample ) ) )
        goto error;
    /* ensure buffer length */
    if( in->samplecount < h->info.framelen )
    {
        if( !(in->flags & AUDIO_FLAG_EOF) )
        {
            x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "samples too few but not EOF???\n" );
            goto error;
        }
        h->finishing = 1;
        if( x264_af_resize_fill_buffer( in->samples, h->info.framelen, h->info.channels, in->samplecount, 0.0f ) )
        {
            x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "failed to expand buffer.\n" );
            goto error;
        }
    }
    if( h->last_dts == INVALID_DTS )
        h->last_dts = h->last_sample;
    h->last_sample += in->samplecount;
    in->samplecount = h->info.framelen;

    /* convert to integer */
    void *samplebuffer = x264_af_interleave2( SMPFMT_S16, in->samples, h->info.channels, in->samplecount );
    if( in->samplecount && !samplebuffer )
        goto error;
    x264_af_free_packet( in );
    in = NULL;

    out->size = E_IF_encode( h->amrwb_3gpp, h->mode, samplebuffer, out->data, h->dtx );
    free( samplebuffer );
    if( out->size == 0 )
    {
        x264_cli_log( "amrwb_3gpp", X264_LOG_ERROR, "failed to encode audio.\n" );
        goto error;
    }

    out->dts = h->last_dts;
    if( add_frame_dts( &h->last_dts, h->info.framelen ) )
        goto error;
    return out;

error:
    if( in )
        x264_af_free_packet( in );
    x264_af_free_packet( out );
    return NULL;
}

static void skip_samples( hnd_t handle, uint64_t samplecount )
{
    enc_amrwb_3gpp_t *h = handle;
    if( !h )
        return;
    add_skip_samples( &h->last_sample, samplecount );
}

static audio_packet_t *finish( hnd_t encoder )
{
    return NULL;
}

static void amrwb_3gpp_close( hnd_t handle )
{
    enc_amrwb_3gpp_t *h = handle;
    if( !h )
        return;

    if( h->amrwb_3gpp )
        E_IF_exit( h->amrwb_3gpp );
    if( h->info.extradata )
        free( h->info.extradata );
    free( h );
}

static void amrwb_3gpp_help( const char * const codec_name )
{
    printf( "      * amrwb_3gpp encoder help\n" );
    printf( "        --abitrate        Bitrate in kbits/s. [23.85]\n" );
    printf( "        --dtx             Use DTX comfort noise generation [1].\n" );
}

const audio_encoder_t audio_encoder_amrwb_3gpp =
{
    .init            = init,
    .get_info        = get_info,
    .get_next_packet = get_next_packet,
    .skip_samples    = skip_samples,
    .finish          = finish,
    .free_packet     = free_packet,
    .close           = amrwb_3gpp_close,
    .show_help       = amrwb_3gpp_help,
    .is_valid_encoder = NULL
};
