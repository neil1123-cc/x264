#include "filters/audio/internal.h"
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>

#if USE_AVXSYNTH
#include <dlfcn.h>
#if SYS_MACOSX
#define avs_open dlopen( "libavxsynth.dylib", RTLD_NOW )
#else
#define avs_open dlopen( "libavxsynth.so", RTLD_NOW )
#endif
#define avs_close dlclose
#define avs_address dlsym
#else
#include <windows.h>
#define avs_open LoadLibrary( "avisynth" )
#define avs_close FreeLibrary
#define avs_address GetProcAddress
#endif

#define AVSC_NO_DECLSPEC
#undef EXTERN_C
#if USE_AVXSYNTH
#include "extras/avxsynth_c.h"
#else
#include "extras/avisynth_c.h"
#endif
#define AVSC_DECLARE_FUNC(name) name##_func name

/* AVS uses a versioned interface to control backwards compatibility */
/* YV12 support is required, which was added in 2.5 */
#define AVS_INTERFACE_25 2
#define DEFAULT_BUFSIZE 192000 // 1 second of 48khz 32bit audio
                               // same as AVCODEC_MAX_AUDIO_FRAME_SIZE

#define LOAD_AVS_FUNC(name, continue_on_fail)\
{\
    h->func.name = (void*)avs_address( h->library, #name );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

typedef struct avs_source_t
{
    AUDIO_FILTER_COMMON

    AVS_Clip *clip;
    AVS_ScriptEnvironment *env;
    HMODULE library;
    enum SampleFmt sample_fmt;

    int64_t num_samples;
    int eof;
    uint8_t *buffer;
    intptr_t bufsize;

    struct
    {
        AVSC_DECLARE_FUNC( avs_clip_get_error );
        AVSC_DECLARE_FUNC( avs_create_script_environment );
        AVSC_DECLARE_FUNC( avs_delete_script_environment );
        AVSC_DECLARE_FUNC( avs_get_error );
        AVSC_DECLARE_FUNC( avs_get_audio );
        AVSC_DECLARE_FUNC( avs_get_video_info );
        AVSC_DECLARE_FUNC( avs_function_exists );
        AVSC_DECLARE_FUNC( avs_invoke );
        AVSC_DECLARE_FUNC( avs_release_clip );
        AVSC_DECLARE_FUNC( avs_release_value );
        AVSC_DECLARE_FUNC( avs_take_clip );
    } func;
} avs_source_t;

const audio_filter_t audio_filter_avs;

static int x264_audio_avs_load_library( avs_source_t *h )
{
    h->library = avs_open;
    if( !h->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error, 0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_error, 1 );
    LOAD_AVS_FUNC( avs_get_audio, 0 );
    LOAD_AVS_FUNC( avs_get_video_info, 0 );
    LOAD_AVS_FUNC( avs_function_exists, 0 );
    LOAD_AVS_FUNC( avs_invoke, 0 );
    LOAD_AVS_FUNC( avs_release_clip, 0 );
    LOAD_AVS_FUNC( avs_release_value, 0 );
    LOAD_AVS_FUNC( avs_take_clip, 0 );
    return 0;
fail:
    avs_close( h->library );
    return -1;
}

static int update_clip( avs_source_t *h, const AVS_VideoInfo **vi, AVS_Value *res )
{
    AVS_Clip *clip = h->func.avs_take_clip( *res, h->env );
    const AVS_VideoInfo *new_vi;

    if( !clip )
        return -1;

    new_vi = h->func.avs_get_video_info( clip );
    if( !new_vi )
    {
        h->func.avs_release_clip( clip );
        return -1;
    }

    if( h->clip && h->func.avs_release_clip )
        h->func.avs_release_clip( h->clip );
    h->clip = clip;
    *vi = new_vi;
    return 0;
}

static void avs_release_value_if_defined( avs_source_t *h, AVS_Value *v )
{
    if( h && h->func.avs_release_value && avs_defined( *v ) )
    {
        h->func.avs_release_value( *v );
        *v = avs_void;
    }
}

#if !USE_AVXSYNTH
static AVS_Value check_avisource( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value arg = avs_new_value_string( filename );
    AVS_Value res;

    x264_cli_log( "avs", X264_LOG_INFO, "trying AVISource for audio ..." );
    res = h->func.avs_invoke( h->env, "AVISource", arg, NULL );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg );
    return res;
}

// FFAudioSource(string source, int track)
static AVS_Value check_ffms( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value res;
    x264_cli_log( "avs", X264_LOG_INFO, "trying FFAudioSource for audio ..." );
    if( !h->func.avs_function_exists( h->env, "FFAudioSource" ) )
    {
        x264_cli_printf( X264_LOG_INFO, " not found\n" );
        res = avs_new_value_error( "not found" );
        return res;
    }

    AVS_Value arg_array = avs_new_value_array( (AVS_Value []){ avs_new_value_string( filename ),
                                                 avs_new_value_int( track ) }, 2);
    const char *arg_names[2] = { NULL, "track" };

    res = h->func.avs_invoke( h->env, "FFAudioSource", arg_array, arg_names );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg_array );
    return res;
}

// LSMASHAudioSource(string source, int track)
static AVS_Value check_lsmas( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value res;
    x264_cli_log( "avs", X264_LOG_INFO, "trying LSMASHAudioSource for audio ..." );
    if( !h->func.avs_function_exists( h->env, "LSMASHAudioSource" ) )
    {
        x264_cli_printf( X264_LOG_INFO, " not found\n" );
        res = avs_new_value_error( "not found" );
        return res;
    }

    AVS_Value arg_array = avs_new_value_array( (AVS_Value []){ avs_new_value_string( filename ),
                                                 avs_new_value_int( track == TRACK_ANY ? 0 : track ) }, 2);
    const char *arg_names[2] = { NULL, "track" };

    res = h->func.avs_invoke( h->env, "LSMASHAudioSource", arg_array, arg_names );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg_array );
    return res;
}

// LWLibavAudioSource(string source, int stream_index)
static AVS_Value check_lwls( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value res;
    x264_cli_log( "avs", X264_LOG_INFO, "trying LWLibavAudioSource for audio ..." );
    if( !h->func.avs_function_exists( h->env, "LWLibavAudioSource" ) )
    {
        x264_cli_printf( X264_LOG_INFO, " not found\n" );
        res = avs_new_value_error( "not found" );
        return res;
    }

    AVS_Value arg_array = avs_new_value_array( (AVS_Value []){ avs_new_value_string( filename ),
                                                 avs_new_value_int( track ) }, 2);
    const char *arg_names[2] = { NULL, "stream_index" };

    res = h->func.avs_invoke( h->env, "LWLibavAudioSource", arg_array, arg_names );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg_array );
    return res;
}

//DirectShowSource (string filename, bool "audio", bool "video" )
static AVS_Value check_directshowsource( hnd_t handle, const char *filename, int track )
{
    avs_source_t *h = handle;
    AVS_Value res;
    x264_cli_log( "avs", X264_LOG_INFO, "trying DirectShowSource for audio ..." );
    if( !h->func.avs_function_exists( h->env, "DirectShowSource" ) )
    {
        x264_cli_printf( X264_LOG_INFO, " not found\n" );
        res = avs_new_value_error( "not found" );
        return res;
    }

    AVS_Value arg_array = avs_new_value_array( (AVS_Value []){ avs_new_value_string( filename ),
                                                 avs_new_value_bool( 0 ),
                                                 avs_new_value_bool( 1 ) }, 3);
    const char *arg_names[3] = { NULL, "video", "audio" };

    res = h->func.avs_invoke( h->env, "DirectShowSource", arg_array, arg_names );
    if( avs_is_error( res ) )
        x264_cli_printf( X264_LOG_INFO, " failed\n" );
    else
        x264_cli_printf( X264_LOG_INFO, " succeeded\n" );

    h->func.avs_release_value( arg_array );
    return res;
}

static void avs_audio_build_filter_sequence( const char *ext, int track,
    AVS_Value (*filters[])( hnd_t handle, const char *filename, int track ) )
{
    int i = 0;
    if( track != TRACK_ANY )
    {
        // audio track selection doesn't work except for lsmas, lwls or ffms
        if( !strcmp( ext, "mp4" ) || !strcmp( ext, "mov" ) || !strcmp( ext, "qt" ) ||
            !strcmp( ext, "3gp" ) || !strcmp( ext, "3g2" ) || !strcmp( ext, "m4a" ) )
            filters[i++] = check_lsmas;
        filters[i++] = check_lwls;
        filters[i++] = check_ffms;
    }
    else if( !strcmp( ext, "mp4" ) || !strcmp( ext, "mov" ) || !strcmp( ext, "qt" ) ||
             !strcmp( ext, "3gp" ) || !strcmp( ext, "3g2" ) || !strcmp( ext, "m4a" ) )
    {
        // try AVISource first
        filters[i++] = check_lsmas;
        filters[i++] = check_lwls;
        filters[i++] = check_ffms;
        filters[i++] = check_directshowsource;
    }
    else if( !strcmp( ext, "avi" ) )
    {
        // try AVISource first
        filters[i++] = check_avisource;
        filters[i++] = check_lwls;
        filters[i++] = check_ffms;
        filters[i++] = check_directshowsource;
    }
    else if( !strcmp( ext, "wma" ) || !strcmp( ext, "wmv" ) || !strcmp( ext, "asf" ) )
    {
        filters[i++] = check_directshowsource;
        filters[i++] = check_lwls;
        filters[i++] = check_ffms;
    }
    else
    {
        filters[i++] = check_lwls;
        filters[i++] = check_ffms;
        filters[i++] = check_directshowsource;
    }
    filters[i] = NULL;
    return;
}
#endif

#define GOTO_IF( cond, label, ... ) \
if( cond ) \
{ \
    x264_cli_log( "avs", X264_LOG_ERROR, __VA_ARGS__ ); \
    goto label; \
}

static int avs_parse_audio_track( const char *trackstr, int *track )
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
#if !USE_AVXSYNTH
    const char *filename_ext = NULL;
#endif
    int track;
    AVS_Value res = avs_void;

    GOTO_IF( avs_parse_audio_track( x264_get_option( "track", opts ), &track ),
             fail2, "no valid track requested ('any', 0 or a positive integer)\n" )
#if USE_AVXSYNTH
    GOTO_IF( track > 0, fail2, "only script imports are supported by this filter\n" )
#endif
    GOTO_IF( !filename, fail2, "no filename given\n" )
    GOTO_IF( !x264_is_regular_file_path( filename ), fail2, "reading audio from non-regular files is not supported\n" )
#if !USE_AVXSYNTH
    filename_ext = get_filename_extension( filename );
#endif

    INIT_FILTER_STRUCT( audio_filter_avs, avs_source_t );

    GOTO_IF( x264_audio_avs_load_library( h ), error, "failed to load avisynth\n" )
    h->env = h->func.avs_create_script_environment( AVS_INTERFACE_25 );
    GOTO_IF( !h->env, error, "failed to create avisynth script environment\n" )
    if( h->func.avs_get_error )
    {
        const char *error = h->func.avs_get_error( h->env );
        GOTO_IF( error, error, "%s\n", error );
    }

#if USE_AVXSYNTH
    AVS_Value arg = avs_new_value_string( filename );
    res = h->func.avs_invoke( h->env, "Import", arg, NULL );
    h->func.avs_release_value( arg );
    GOTO_IF( avs_is_error( res ), error, "%s\n", avs_as_string( res ) )
#else
    if( !strcmp( filename_ext, "avs" ) )
    {
        // normal avs script
        AVS_Value arg = avs_new_value_string( filename );
        res = h->func.avs_invoke( h->env, "Import", arg, NULL );
        h->func.avs_release_value( arg );
        GOTO_IF( avs_is_error( res ), error, "%s\n", avs_as_string( res ) )
    }
    else
    {
        AVS_Value (*filters[4])( hnd_t handle, const char *filename, int track );
        int i;

        avs_audio_build_filter_sequence( filename_ext, track, filters );

        for( i = 0; filters[i]; i++ )
        {
            res = filters[i]( h, filename, track );
            if( !avs_is_error( res ) )
                break;
            avs_release_value_if_defined( h, &res );
        }
        GOTO_IF( !filters[i], error, "no working input filter is found for audio input\n" )
    }
#endif

    GOTO_IF( !avs_is_clip( res ), error, "no valid clip is found\n" )

    const AVS_VideoInfo *vi = NULL;
    GOTO_IF( update_clip( h, &vi, &res ), error, "no valid audio track is found\n" )
    GOTO_IF( !avs_has_audio( vi ), error, "no valid audio track is found\n" )

    // video is unneeded, so disable it if any
    AVS_Value next_res = h->func.avs_invoke( h->env, "KillVideo", res, NULL );
    avs_release_value_if_defined( h, &res );
    res = next_res;
    GOTO_IF( update_clip( h, &vi, &res ), error, "failed to update audio clip\n" )

    switch( avs_sample_type( vi ) )
    {
      case AVS_SAMPLE_INT16:
        h->sample_fmt = SMPFMT_S16;
        break;
      case AVS_SAMPLE_INT32:
        h->sample_fmt = SMPFMT_S32;
        break;
      case AVS_SAMPLE_FLOAT:
        h->sample_fmt = SMPFMT_FLT;
        break;
      case AVS_SAMPLE_INT8:
        h->sample_fmt = SMPFMT_U8;
        break;
      case AVS_SAMPLE_INT24:
      default:
        h->sample_fmt = SMPFMT_NONE;
        break;
    }

    if( h->sample_fmt == SMPFMT_NONE )
    {
        int input_chansize = avs_bytes_per_channel_sample( vi );
        GOTO_IF( input_chansize <= 0 || input_chansize > INT_MAX / 8,
                 error, "invalid audio sample format\n" )
        int input_sample_bits = input_chansize * 8;
        x264_cli_log( "avs", X264_LOG_INFO, "detected %dbit sample format, converting to float\n", input_sample_bits );
        next_res = h->func.avs_invoke( h->env, "ConvertAudioToFloat", res, NULL );
        avs_release_value_if_defined( h, &res );
        res = next_res;
        GOTO_IF( avs_is_error( res ), error, "failed to convert audio sample format\n" )
        GOTO_IF( update_clip( h, &vi, &res ), error, "failed to update audio clip\n" )
        h->sample_fmt = SMPFMT_FLT;
    }

    avs_release_value_if_defined( h, &res );

    int samplerate         = avs_samples_per_second( vi );
    int channels           = avs_audio_channels( vi );
    int chansize           = avs_bytes_per_channel_sample( vi );
    GOTO_IF( samplerate <= 0 || channels <= 0 || chansize <= 0 || chansize > INT_MAX / 8 ||
             vi->num_audio_samples < 0,
             error, "invalid audio parameters\n" )

    h->info.samplerate     = samplerate;
    h->info.channels       = channels;
    h->info.framelen       = 1;
    h->info.chansize       = chansize;
    int64_t samplesize64   = (int64_t)chansize * channels;
    GOTO_IF( samplesize64 > INT_MAX, error, "invalid audio parameters\n" )
    int samplesize         = (int)samplesize64;
    h->info.samplesize     = samplesize;
    h->info.framesize      = h->info.samplesize;
    h->info.depth          = h->info.chansize * 8;
    h->info.timebase       = (timebase_t){ 1, h->info.samplerate };

    h->num_samples = vi->num_audio_samples;
    h->bufsize = DEFAULT_BUFSIZE;
    int64_t avs_buffer_alloc_size = (int64_t)h->bufsize;
    h->buffer = malloc( avs_buffer_alloc_size );
    GOTO_IF( !h->buffer, error, "malloc failed\n" )

    free( opts );
    return 0;

error:
    AF_LOG_ERR( h, "error opening audio\n" );
    avs_release_value_if_defined( h, &res );
fail:
    if( h )
    {
        if( h->clip && h->func.avs_release_clip )
            h->func.avs_release_clip( h->clip );
        if( h->env && h->func.avs_delete_script_environment )
            h->func.avs_delete_script_environment( h->env );
        if( h->library )
            avs_close( h->library );
        free( h->buffer );
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

static struct audio_packet_t *get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    avs_source_t *h = handle;
    if( !h || !h->clip || !h->buffer || first_sample < 0 || last_sample <= first_sample ||
        h->info.channels <= 0 || h->info.samplesize <= 0 || h->num_samples < 0 ||
        h->sample_fmt == SMPFMT_NONE || !h->func.avs_get_audio )
        return NULL;

    if( h->eof )
        return NULL;

    if( first_sample >= h->num_samples )
    {
        h->eof = 1;
        return NULL;
    }

    int64_t nsamples = last_sample - first_sample;
    int eof = 0;
    if( h->num_samples <= last_sample )
    {
        nsamples = h->num_samples - first_sample;
        eof = 1;
    }
    if( nsamples <= 0 || nsamples > UINT_MAX ||
        nsamples > INT_MAX / h->info.samplesize )
        return NULL;
    int64_t size64 = nsamples * h->info.samplesize;
    if( size64 > INT_MAX )
        return NULL;
    int size = (int)size64;
    unsigned packet_channels = (unsigned)h->info.channels;
    unsigned packet_samplecount = (unsigned)nsamples;
    if( size > h->bufsize )
        return NULL;

    int64_t packet_alloc_size = sizeof( audio_packet_t );
    audio_packet_t *pkt = calloc( 1, packet_alloc_size );
    if( !pkt )
        return NULL;
    pkt->info           = h->info;
    pkt->dts            = first_sample;
    pkt->channels       = packet_channels;
    pkt->samplecount    = packet_samplecount;
    pkt->size           = size;

    if( h->func.avs_get_audio( h->clip, h->buffer, first_sample, nsamples ) )
        goto fail;

    pkt->samples = x264_af_deinterleave2( h->buffer, h->sample_fmt, pkt->channels, pkt->samplecount );
    if( !pkt->samples )
        goto fail;

    if( eof )
    {
        h->eof = 1;
        pkt->flags |= AUDIO_FLAG_EOF;
    }

    return pkt;

fail:
    h->failed = 1;
    x264_af_free_packet( pkt );
    return NULL;
}

static int avs_is_failed( hnd_t handle )
{
    avs_source_t *h = handle;
    return h && h->failed;
}

static void avs_close_file( hnd_t handle )
{
    if( !handle )
        return;
    avs_source_t *h = handle;
    if( h->clip && h->func.avs_release_clip )
        h->func.avs_release_clip( h->clip );
    if( h->env && h->func.avs_delete_script_environment )
        h->func.avs_delete_script_environment( h->env );
    if( h->library )
        avs_close( h->library );
    free( h->buffer );
    free( h );
}

const audio_filter_t audio_filter_avs =
{
        .name        = "avs",
#if USE_AVXSYNTH
        .description = "Retrieve PCM samples from specified audio track with AvxSynth",
#else
        .description = "Retrieve PCM samples from specified audio track with AviSynth",
#endif
        .help        = "Arguments: filename",
        .init        = init,
        .get_samples = get_samples,
        .is_failed   = avs_is_failed,
        .free_packet = free_packet,
        .close       = avs_close_file
};
