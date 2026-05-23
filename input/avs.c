/*****************************************************************************
 * avs.c: avisynth input
 *****************************************************************************
 * Copyright (C) 2009-2025 x264 project
 *
 * Authors: Steven Walters <kemuri9@gmail.com>
 *          Anton Mitrofanov <BugMaster@narod.ru>
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

#include "input.h"

#define X264_AVS_PLANES 3

#if SYS_WINDOWS || SYS_CYGWIN
#include <windows.h>
#define avs_open() LoadLibraryW( L"avisynth" )
#define avs_close FreeLibrary
#define avs_address GetProcAddress
#else
#include <dlfcn.h>
#if SYS_MACOSX
#define avs_open() dlopen( "libavisynth.dylib", RTLD_NOW )
#else
#define avs_open() dlopen( "libavisynth.so", RTLD_NOW )
#endif
#define avs_close dlclose
#define avs_address dlsym
#endif

#define AVSC_NO_DECLSPEC
#undef EXTERN_C
#include "extras/avisynth_c.h"
#define AVSC_DECLARE_FUNC(name) name##_func name

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "avs", __VA_ARGS__ )

#if HAVE_AUDIO
#include "audio/audio.h"
#endif

/* AVS uses a versioned interface to control backwards compatibility */
/* YV12 support is required, which was added in 2.5 */
#define AVS_INTERFACE_25 2

#if HAVE_SWSCALE
#include <libavutil/pixfmt.h>
#endif

/* maximum size of the sequence of filters to try on non script files */
#define AVS_MAX_SEQUENCE 5

#define LOAD_AVS_FUNC(name, continue_on_fail)\
{\
    h->func.name = (void*)avs_address( h->library, #name );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

#define LOAD_AVS_FUNC_ALIAS(name, alias, continue_on_fail)\
{\
    if( !h->func.name )\
        h->func.name = (void*)avs_address( h->library, alias );\
    if( !continue_on_fail && !h->func.name )\
        goto fail;\
}

typedef struct
{
    AVS_Clip *clip;
    AVS_ScriptEnvironment *env;
    void *library;
    int num_frames;
    struct
    {
        AVSC_DECLARE_FUNC( avs_clip_get_error );
        AVSC_DECLARE_FUNC( avs_create_script_environment );
        AVSC_DECLARE_FUNC( avs_delete_script_environment );
        AVSC_DECLARE_FUNC( avs_get_error );
        AVSC_DECLARE_FUNC( avs_get_frame );
        AVSC_DECLARE_FUNC( avs_get_video_info );
        AVSC_DECLARE_FUNC( avs_function_exists );
        AVSC_DECLARE_FUNC( avs_invoke );
        AVSC_DECLARE_FUNC( avs_release_clip );
        AVSC_DECLARE_FUNC( avs_release_value );
        AVSC_DECLARE_FUNC( avs_release_video_frame );
        AVSC_DECLARE_FUNC( avs_take_clip );
        AVSC_DECLARE_FUNC( avs_is_yv24 );
        AVSC_DECLARE_FUNC( avs_is_yv16 );
        AVSC_DECLARE_FUNC( avs_is_yv12 );
        AVSC_DECLARE_FUNC( avs_is_yv411 );
        AVSC_DECLARE_FUNC( avs_is_y8 );
        AVSC_DECLARE_FUNC( avs_get_pitch_p );
        AVSC_DECLARE_FUNC( avs_get_read_ptr_p );
        // AviSynth+ extension
        AVSC_DECLARE_FUNC( avs_is_rgb48 );
        AVSC_DECLARE_FUNC( avs_is_rgb64 );
        AVSC_DECLARE_FUNC( avs_is_yuv444p16 );
        AVSC_DECLARE_FUNC( avs_is_yuv422p16 );
        AVSC_DECLARE_FUNC( avs_is_yuv420p16 );
        AVSC_DECLARE_FUNC( avs_is_y16 );
        AVSC_DECLARE_FUNC( avs_is_444 );
        AVSC_DECLARE_FUNC( avs_is_422 );
        AVSC_DECLARE_FUNC( avs_is_420 );
        AVSC_DECLARE_FUNC( avs_is_y );
    } func;
#if HAVE_AUDIO
    char *filename;
    int has_audio;
#endif
} avs_hnd_t;

static inline int invalid_dimensions( int width, int height )
{
    return width <= 0 || height <= 0 || width > MAX_RESOLUTION || height > MAX_RESOLUTION;
}

static inline int invalid_avs_dimensions( int width, int height, int allow_double_width )
{
    int max_width = allow_double_width ? MAX_RESOLUTION * 2 : MAX_RESOLUTION;
    return width <= 0 || height <= 0 || width > max_width || height > MAX_RESOLUTION;
}

static void avs_release_value_if_defined( avs_hnd_t *h, AVS_Value *v )
{
    if( h && h->func.avs_release_value && avs_defined( *v ) )
    {
        h->func.avs_release_value( *v );
        *v = avs_void;
    }
}

static void avs_cleanup_handle( avs_hnd_t *h )
{
    if( !h )
        return;
    if( h->func.avs_release_clip && h->clip )
        h->func.avs_release_clip( h->clip );
    if( h->func.avs_delete_script_environment && h->env )
        h->func.avs_delete_script_environment( h->env );
    if( h->library )
        avs_close( h->library );
#if HAVE_AUDIO
    free( h->filename );
#endif
    free( h );
}

/* load the library and functions we require from it */
static int custom_avs_load_library( avs_hnd_t *h )
{
    h->library = avs_open();
    if( !h->library )
        return -1;
    LOAD_AVS_FUNC( avs_clip_get_error, 0 );
    LOAD_AVS_FUNC( avs_create_script_environment, 0 );
    LOAD_AVS_FUNC( avs_delete_script_environment, 1 );
    LOAD_AVS_FUNC( avs_get_error, 1 );
    LOAD_AVS_FUNC( avs_get_frame, 0 );
    LOAD_AVS_FUNC( avs_get_video_info, 0 );
    LOAD_AVS_FUNC( avs_function_exists, 0 );
    LOAD_AVS_FUNC( avs_invoke, 0 );
    LOAD_AVS_FUNC( avs_release_clip, 0 );
    LOAD_AVS_FUNC( avs_release_value, 0 );
    LOAD_AVS_FUNC( avs_release_video_frame, 0 );
    LOAD_AVS_FUNC( avs_take_clip, 0 );
    LOAD_AVS_FUNC( avs_is_yv24, 1 );
    LOAD_AVS_FUNC( avs_is_yv16, 1 );
    LOAD_AVS_FUNC( avs_is_yv12, 1 );
    LOAD_AVS_FUNC( avs_is_yv411, 1 );
    LOAD_AVS_FUNC( avs_is_y8, 1 );
    LOAD_AVS_FUNC( avs_get_pitch_p, 1 );
    LOAD_AVS_FUNC( avs_get_read_ptr_p, 1 );
    // AviSynth+ extension
    LOAD_AVS_FUNC( avs_is_rgb48, 1 );
    LOAD_AVS_FUNC_ALIAS( avs_is_rgb48, "_avs_is_rgb48@4", 1 );
    LOAD_AVS_FUNC( avs_is_rgb64, 1 );
    LOAD_AVS_FUNC_ALIAS( avs_is_rgb64, "_avs_is_rgb64@4", 1 );
    LOAD_AVS_FUNC( avs_is_yuv444p16, 1 );
    LOAD_AVS_FUNC( avs_is_yuv422p16, 1 );
    LOAD_AVS_FUNC( avs_is_yuv420p16, 1 );
    LOAD_AVS_FUNC( avs_is_y16, 1 );
    LOAD_AVS_FUNC( avs_is_444, 1 );
    LOAD_AVS_FUNC( avs_is_422, 1 );
    LOAD_AVS_FUNC( avs_is_420, 1 );
    LOAD_AVS_FUNC( avs_is_y, 1 );
    return 0;
fail:
    avs_close( h->library );
    h->library = NULL;
    return -1;
}

#define AVS_IS_YV24( vi ) (h->func.avs_is_yv24 ? h->func.avs_is_yv24( vi ) : avs_is_yv24( vi ))
#define AVS_IS_YV16( vi ) (h->func.avs_is_yv16 ? h->func.avs_is_yv16( vi ) : avs_is_yv16( vi ))
#define AVS_IS_YV12( vi ) (h->func.avs_is_yv12 ? h->func.avs_is_yv12( vi ) : avs_is_yv12( vi ))
#define AVS_IS_YV411( vi ) (h->func.avs_is_yv411 ? h->func.avs_is_yv411( vi ) : avs_is_yv411( vi ))
#define AVS_IS_Y8( vi ) (h->func.avs_is_y8 ? h->func.avs_is_y8( vi ) : avs_is_y8( vi ))
#define AVS_GET_PITCH_P( p, plane ) (h->func.avs_get_pitch_p ? h->func.avs_get_pitch_p( p, plane ) : avs_get_pitch_p( p, plane ))
#define AVS_GET_READ_PTR_P( p, plane ) (h->func.avs_get_read_ptr_p ? h->func.avs_get_read_ptr_p( p, plane ) : avs_get_read_ptr_p( p, plane ))

#define AVS_IS_AVISYNTHPLUS (h->func.avs_is_420 && h->func.avs_is_422 && h->func.avs_is_444)
#define AVS_IS_420( vi ) (h->func.avs_is_420 ? h->func.avs_is_420( vi ) : AVS_IS_YV12( vi ))
#define AVS_IS_422( vi ) (h->func.avs_is_422 ? h->func.avs_is_422( vi ) : AVS_IS_YV16( vi ))
#define AVS_IS_444( vi ) (h->func.avs_is_444 ? h->func.avs_is_444( vi ) : AVS_IS_YV24( vi ))
#define AVS_IS_RGB48( vi ) (h->func.avs_is_rgb48 && h->func.avs_is_rgb48( vi ))
#define AVS_IS_RGB64( vi ) (h->func.avs_is_rgb64 && h->func.avs_is_rgb64( vi ))
#define AVS_IS_YUV420P16( vi ) (h->func.avs_is_yuv420p16 && h->func.avs_is_yuv420p16( vi ))
#define AVS_IS_YUV422P16( vi ) (h->func.avs_is_yuv422p16 && h->func.avs_is_yuv422p16( vi ))
#define AVS_IS_YUV444P16( vi ) (h->func.avs_is_yuv444p16 && h->func.avs_is_yuv444p16( vi ))
#define AVS_IS_Y( vi ) (h->func.avs_is_y ? h->func.avs_is_y( vi ) : AVS_IS_Y8( vi ))
#define AVS_IS_Y16( vi ) (h->func.avs_is_y16 && h->func.avs_is_y16( vi ))

/* generate a filter sequence to try based on the filename extension */
static void avs_build_filter_sequence( const char *filename_ext, const char *filter[AVS_MAX_SEQUENCE+1] )
{
    int i = 0;
#if SYS_WINDOWS || SYS_CYGWIN
    const char *all_purpose[] = { "LWLibavVideoSource", "FFmpegSource2", "DSS2", "DirectShowSource", 0 };
    if( !strcasecmp( filename_ext, "vpy" ) )
        filter[i++] = "VSImport";
    if( !strcasecmp( filename_ext, "avi" ) || !strcasecmp( filename_ext, "vpy" ) )
    {
        filter[i++] = "AVISource";
        filter[i++] = "HBVFWSource";
    }
    if( !strcasecmp( filename_ext, "mp4" ) || !strcasecmp( filename_ext, "mov" ) || !strcasecmp( filename_ext, "qt" ) ||
        !strcasecmp( filename_ext, "3gp" ) || !strcasecmp( filename_ext, "3g2" ) )
        filter[i++] = "LSMASHVideoSource";
    if( !strcasecmp( filename_ext, "d2v" ) )
        filter[i++] = "MPEG2Source";
    if( !strcasecmp( filename_ext, "dga" ) )
        filter[i++] = "AVCSource";
    if( !strcasecmp( filename_ext, "dgi" ) )
        filter[i++] = "DGSource";
#else
    const char *all_purpose[] = { "FFVideoSource", 0 };
#endif
    for( int j = 0; all_purpose[j] && i < AVS_MAX_SEQUENCE; j++ )
        filter[i++] = all_purpose[j];
}

static AVS_Value update_clip( avs_hnd_t *h, const AVS_VideoInfo **vi, AVS_Value res, AVS_Value release )
{
    AVS_Clip *clip = h->func.avs_take_clip( res, h->env );
    const AVS_VideoInfo *new_vi;

    h->func.avs_release_value( release );
    if( !clip )
    {
        *vi = NULL;
        return res;
    }

    new_vi = h->func.avs_get_video_info( clip );
    if( !new_vi )
    {
        h->func.avs_release_clip( clip );
        *vi = NULL;
        return res;
    }

    if( h->clip && h->func.avs_release_clip )
        h->func.avs_release_clip( h->clip );
    h->clip = clip;
    *vi = new_vi;
    return res;
}

static float get_avs_version( avs_hnd_t *h )
{
    if( !h->func.avs_function_exists( h->env, "VersionNumber" ) )
    {
        x264_cli_log( "avs", X264_LOG_ERROR, "VersionNumber does not exist\n" );
        return -1;
    }
    AVS_Value ver = h->func.avs_invoke( h->env, "VersionNumber", avs_new_value_array( NULL, 0 ), NULL );
    if( avs_is_error( ver ) )
    {
        x264_cli_log( "avs", X264_LOG_ERROR, "unable to determine avisynth version: %s\n", avs_as_error( ver ) );
        avs_release_value_if_defined( h, &ver );
        return -1;
    }
    if( !avs_is_float( ver ) )
    {
        x264_cli_log( "avs", X264_LOG_ERROR, "VersionNumber did not return a float value\n" );
        avs_release_value_if_defined( h, &ver );
        return -1;
    }
    float ret = avs_as_float( ver );
    avs_release_value_if_defined( h, &ver );
    return ret;
}

#ifdef _WIN32
static char *utf16_to_ansi( const wchar_t *utf16 )
{
    if( !utf16 )
        return NULL;
    BOOL invalid = FALSE;
    int len = WideCharToMultiByte( CP_ACP, WC_NO_BEST_FIT_CHARS, utf16, -1, NULL, 0, NULL, &invalid );
    if( len && !invalid )
    {
        size_t ansi_size = (size_t)len;
        int64_t ansi_alloc_size = (int64_t)ansi_size;
        char *ansi = malloc( ansi_alloc_size );
        if( ansi )
        {
            invalid = FALSE;
            int written = WideCharToMultiByte( CP_ACP, WC_NO_BEST_FIT_CHARS, utf16, -1, ansi, len, NULL, &invalid );
            if( written == len && !invalid )
                return ansi;
            free( ansi );
        }
    }
    return NULL;
}

static char *utf8_to_ansi( const char *filename )
{
    char *ansi = NULL;
    wchar_t *filename_utf16 = x264_utf8_to_utf16( filename );
    if( filename_utf16 )
    {
        /* Check if the filename already is valid ANSI. */
        if( !(ansi = utf16_to_ansi( filename_utf16 )) )
        {
            /* Check for a legacy 8.3 short filename. */
            DWORD len = GetShortPathNameW( filename_utf16, NULL, 0 );
            if( len )
            {
                size_t short_utf16_size = (size_t)len * sizeof( wchar_t );
                int64_t short_utf16_alloc_size = (int64_t)short_utf16_size;
                wchar_t *short_utf16 = short_utf16_size / sizeof( wchar_t ) == len ? malloc( short_utf16_alloc_size ) : NULL;
                if( short_utf16 )
                {
                    DWORD written = GetShortPathNameW( filename_utf16, short_utf16, len );
                    if( written && written < len )
                        ansi = utf16_to_ansi( short_utf16 );
                    free( short_utf16 );
                }
            }
        }
        free( filename_utf16 );
    }
    return ansi;
}
#endif

#define FAIL_IF_ERROR_CLEANUP( cond, ... )\
do\
{\
    if( cond )\
    {\
        x264_cli_log( "avs", X264_LOG_ERROR, __VA_ARGS__ );\
        goto fail;\
    }\
} while( 0 )

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    if( !psz_filename || !p_handle || !info || !opt )
        return -1;
    *p_handle = NULL;

    FILE *fh = x264_fopen( psz_filename, "r" );
    if( !fh )
        return -1;
    int b_regular = x264_is_regular_file( fh );
    FAIL_IF_ERROR( fclose( fh ), "failed to close input probe file `%s'\n", psz_filename );
    FAIL_IF_ERROR( !b_regular, "AVS input is incompatible with non-regular file `%s'\n", psz_filename );

    AVS_Value res = avs_void;
#ifdef _WIN32
    char *ansi_filename = NULL;
#endif
    avs_hnd_t *h = calloc( 1, sizeof(avs_hnd_t) );
    if( !h )
        return -1;
    video_info_t updated_info = *info;
    int input_range = opt->input_range;
    int bit_depth = opt->bit_depth;
    FAIL_IF_ERROR_CLEANUP( custom_avs_load_library( h ), "failed to load avisynth\n" );
    h->env = h->func.avs_create_script_environment( AVS_INTERFACE_25 );
    FAIL_IF_ERROR_CLEANUP( !h->env, "failed to create avisynth script environment\n" );
    if( h->func.avs_get_error )
    {
        const char *error = h->func.avs_get_error( h->env );
        FAIL_IF_ERROR_CLEANUP( error, "%s\n", error );
    }
    float avs_version = get_avs_version( h );
    if( avs_version <= 0 )
        goto fail;
    x264_cli_log( "avs", X264_LOG_DEBUG, "using avisynth version %.2f\n", avs_version );

#ifdef _WIN32
    /* Avisynth doesn't support Unicode filenames. */
    ansi_filename = utf8_to_ansi( psz_filename );
    FAIL_IF_ERROR_CLEANUP( !ansi_filename, "invalid ansi filename\n" );
    AVS_Value arg = avs_new_value_string( ansi_filename );
#else
    AVS_Value arg = avs_new_value_string( psz_filename );
#endif

    const char *filename_ext = get_filename_extension( psz_filename );

    if( !strcasecmp( filename_ext, "avs" ) )
    {
        res = h->func.avs_invoke( h->env, "Import", arg, NULL );
        FAIL_IF_ERROR_CLEANUP( avs_is_error( res ), "%s\n", avs_as_error( res ) );
        /* check if the user is using a multi-threaded script and apply distributor if necessary.
           adapted from avisynth's vfw interface */
        AVS_Value mt_test = h->func.avs_invoke( h->env, "GetMTMode", avs_new_value_bool( 0 ), NULL );
        int mt_mode = avs_is_int( mt_test ) ? avs_as_int( mt_test ) : 0;
        h->func.avs_release_value( mt_test );
        if( mt_mode > 0 && mt_mode < 5 )
        {
            AVS_Value temp = h->func.avs_invoke( h->env, "Distributor", res, NULL );
            if( avs_is_error( temp ) )
            {
                x264_cli_log( "avs", X264_LOG_ERROR, "%s\n", avs_as_error( temp ) );
                avs_release_value_if_defined( h, &temp );
                goto fail;
            }
            h->func.avs_release_value( res );
            res = temp;
        }
    }
    else /* non script file */
    {
        /* AviSynth+ need explicit invoke of AutoloadPlugins() for registering plugins functions */
        if( h->func.avs_function_exists( h->env, "AutoloadPlugins" ) )
        {
            AVS_Value autoload = h->func.avs_invoke( h->env, "AutoloadPlugins", avs_new_value_array( NULL, 0 ), NULL );
            if( avs_is_error( autoload ) )
                x264_cli_log( "avs", X264_LOG_INFO, "AutoloadPlugins failed: %s\n", avs_as_error( autoload ) );
            avs_release_value_if_defined( h, &autoload );
        }

        /* cycle through known source filters to find one that works */
        const char *filter[AVS_MAX_SEQUENCE+1] = { 0 };
        avs_build_filter_sequence( filename_ext, filter );
        int i;
        for( i = 0; filter[i]; i++ )
        {
            x264_cli_log( "avs", X264_LOG_INFO, "trying %s... ", filter[i] );
            if( !h->func.avs_function_exists( h->env, filter[i] ) )
            {
                x264_cli_printf( X264_LOG_INFO, "not found\n" );
                continue;
            }
            if( !strncasecmp( filter[i], "FFmpegSource", 12 ) )
            {
                x264_cli_printf( X264_LOG_INFO, "indexing... " );
                fflush( stderr );
            }
            res = h->func.avs_invoke( h->env, filter[i], arg, NULL );
            if( !avs_is_error( res ) )
            {
                x264_cli_printf( X264_LOG_INFO, "succeeded\n" );
                break;
            }
            x264_cli_printf( X264_LOG_INFO, "failed\n" );
            avs_release_value_if_defined( h, &res );
        }
        FAIL_IF_ERROR_CLEANUP( !filter[i], "unable to find source filter to open `%s'\n", psz_filename );
        if( !strcasecmp( filter[i], "HBVFWSource" ) )
            bit_depth = 16;
    }
#ifdef _WIN32
    free( ansi_filename );
    ansi_filename = NULL;
#endif
    FAIL_IF_ERROR_CLEANUP( !avs_is_clip( res ), "`%s' didn't return a video clip\n", psz_filename );
    AVS_Clip *clip = h->func.avs_take_clip( res, h->env );
    FAIL_IF_ERROR_CLEANUP( !clip, "failed to take video clip\n" );
    const AVS_VideoInfo *vi = h->func.avs_get_video_info( clip );
    FAIL_IF_ERROR_CLEANUP( !vi, "failed to read video info\n" );
    h->clip = clip;
    FAIL_IF_ERROR_CLEANUP( !avs_has_video( vi ), "`%s' has no video data\n", psz_filename );
    FAIL_IF_ERROR_CLEANUP( invalid_avs_dimensions( vi->width, vi->height, bit_depth > 8 ),
                           "invalid video dimensions (%dx%d)\n", vi->width, vi->height );
    FAIL_IF_ERROR_CLEANUP( vi->num_frames <= 0, "invalid frame count %d\n", vi->num_frames );
    FAIL_IF_ERROR_CLEANUP( !vi->fps_numerator || !vi->fps_denominator,
                           "invalid framerate/timebase %u/%u\n", vi->fps_numerator, vi->fps_denominator );
	
#if HAVE_AUDIO
    h->filename = strdup( psz_filename );
    FAIL_IF_ERROR_CLEANUP( !h->filename, "malloc failed\n" );
    h->has_audio = !!avs_has_audio( vi );
#endif

    /* if the clip is made of fields instead of frames, call weave to make them frames */
    if( avs_is_field_based( vi ) )
    {
        x264_cli_log( "avs", X264_LOG_WARNING, "detected fieldbased (separated) input, weaving to frames\n" );
        AVS_Value tmp = h->func.avs_invoke( h->env, "Weave", res, NULL );
        if( avs_is_error( tmp ) )
        {
            x264_cli_log( "avs", X264_LOG_ERROR, "couldn't weave fields into frames: %s\n", avs_as_error( tmp ) );
            avs_release_value_if_defined( h, &tmp );
            goto fail;
        }
        res = update_clip( h, &vi, tmp, res );
        FAIL_IF_ERROR_CLEANUP( !vi, "failed to read video info\n" );
        FAIL_IF_ERROR_CLEANUP( invalid_avs_dimensions( vi->width, vi->height, bit_depth > 8 ),
                               "invalid video dimensions (%dx%d)\n", vi->width, vi->height );
        FAIL_IF_ERROR_CLEANUP( vi->num_frames <= 0, "invalid frame count %d\n", vi->num_frames );
        updated_info.interlaced = 1;
        updated_info.tff = avs_is_tff( vi );
    }
#if !HAVE_SWSCALE
    /* if swscale is not available, convert the CSP if necessary */
    FAIL_IF_ERROR_CLEANUP( avs_version < 2.6f && (opt->output_csp == X264_CSP_I400 || opt->output_csp == X264_CSP_I422 || opt->output_csp == X264_CSP_I444),
                           "avisynth >= 2.6 is required for i400/i422/i444 output\n" );
    if( (opt->output_csp == X264_CSP_I400 && !AVS_IS_Y( vi )) ||
        (opt->output_csp == X264_CSP_I420 && !AVS_IS_420( vi )) ||
        (opt->output_csp == X264_CSP_I422 && !AVS_IS_422( vi )) ||
        (opt->output_csp == X264_CSP_I444 && !AVS_IS_444( vi )) ||
        (opt->output_csp == X264_CSP_RGB && !avs_is_rgb( vi )) )
    {
        const char *csp;
        if( AVS_IS_AVISYNTHPLUS )
        {
            csp = opt->output_csp == X264_CSP_I400 ? "Y" :
                  opt->output_csp == X264_CSP_I420 ? "YUV420" :
                  opt->output_csp == X264_CSP_I422 ? "YUV422" :
                  opt->output_csp == X264_CSP_I444 ? "YUV444" :
                  "RGB";
        }
        else
        {
            csp = opt->output_csp == X264_CSP_I400 ? "Y8" :
                  opt->output_csp == X264_CSP_I420 ? "YV12" :
                  opt->output_csp == X264_CSP_I422 ? "YV16" :
                  opt->output_csp == X264_CSP_I444 ? "YV24" :
                  "RGB";
        }
        x264_cli_log( "avs", X264_LOG_WARNING, "converting input clip to %s\n", csp );
        if( opt->output_csp != X264_CSP_I400 )
        {
            FAIL_IF_ERROR_CLEANUP( opt->output_csp < X264_CSP_I444 && (vi->width&1),
                                   "input clip width not divisible by 2 (%dx%d)\n", vi->width, vi->height );
            FAIL_IF_ERROR_CLEANUP( opt->output_csp == X264_CSP_I420 && updated_info.interlaced && (vi->height&3),
                                   "input clip height not divisible by 4 (%dx%d)\n", vi->width, vi->height );
            FAIL_IF_ERROR_CLEANUP( (opt->output_csp == X264_CSP_I420 || updated_info.interlaced) && (vi->height&1),
                                   "input clip height not divisible by 2 (%dx%d)\n", vi->width, vi->height );
        }
        char conv_func[16];
        int conv_func_len = snprintf( conv_func, sizeof(conv_func), "ConvertTo%s", csp );
        FAIL_IF_ERROR_CLEANUP( conv_func_len < 0,
                               "conversion function name too long\n" );
        size_t conv_func_size = (size_t)conv_func_len;
        FAIL_IF_ERROR_CLEANUP( conv_func_size >= sizeof(conv_func),
                               "conversion function name too long\n" );
        AVS_Value arg_arr[3];
        const char *arg_name[3];
        int arg_count = 1;
        arg_arr[0] = res;
        arg_name[0] = NULL;
        if( opt->output_csp != X264_CSP_I400 )
        {
            arg_arr[arg_count] = avs_new_value_bool( updated_info.interlaced );
            arg_name[arg_count] = "interlaced";
            arg_count++;
        }
        /* if doing a rgb <-> yuv conversion then range is handled via 'matrix'. though it's only supported in 2.56+ */
        char matrix[7];
        if( avs_version >= 2.56f && ((opt->output_csp == X264_CSP_RGB && avs_is_yuv( vi )) || (opt->output_csp != X264_CSP_RGB && avs_is_rgb( vi ))) )
        {
            // if converting from yuv, then we specify the matrix for the input, otherwise use the output's.
            int use_pc_matrix = avs_is_yuv( vi ) ? input_range == RANGE_PC : opt->output_range == RANGE_PC;
            int matrix_len = snprintf( matrix, sizeof(matrix), "%s%s", use_pc_matrix ? "PC." : "Rec",
                                       ( vi->width > 1024 || vi->height > 576 ) ? "709" : "601" );
            FAIL_IF_ERROR_CLEANUP( matrix_len < 0,
                                   "matrix name too long\n" );
            size_t matrix_size = (size_t)matrix_len;
            FAIL_IF_ERROR_CLEANUP( matrix_size >= sizeof(matrix),
                                   "matrix name too long\n" );
            arg_arr[arg_count] = avs_new_value_string( matrix );
            arg_name[arg_count] = "matrix";
            arg_count++;
            // notification that the input range has changed to the desired one
            input_range = opt->output_range;
        }
        AVS_Value res2 = h->func.avs_invoke( h->env, conv_func, avs_new_value_array( arg_arr, arg_count ), arg_name );
        if( avs_is_error( res2 ) )
        {
            x264_cli_log( "avs", X264_LOG_ERROR, "couldn't convert input clip to %s: %s\n", csp, avs_as_error( res2 ) );
            avs_release_value_if_defined( h, &res2 );
            goto fail;
        }
        res = update_clip( h, &vi, res2, res );
        FAIL_IF_ERROR_CLEANUP( !vi, "failed to read video info\n" );
        FAIL_IF_ERROR_CLEANUP( invalid_avs_dimensions( vi->width, vi->height, bit_depth > 8 ),
                               "invalid video dimensions (%dx%d)\n", vi->width, vi->height );
        FAIL_IF_ERROR_CLEANUP( vi->num_frames <= 0, "invalid frame count %d\n", vi->num_frames );
    }
    /* if swscale is not available, change the range if necessary. This only applies to YUV-based CSPs however */
    if( avs_is_yuv( vi ) && opt->output_range != RANGE_AUTO && ((input_range == RANGE_PC) != opt->output_range) )
    {
        const char *levels = opt->output_range ? "TV->PC" : "PC->TV";
        x264_cli_log( "avs", X264_LOG_WARNING, "performing %s conversion\n", levels );
        AVS_Value arg_arr[2];
        arg_arr[0] = res;
        arg_arr[1] = avs_new_value_string( levels );
        const char *arg_name[] = { NULL, "levels" };
        AVS_Value res2 = h->func.avs_invoke( h->env, "ColorYUV", avs_new_value_array( arg_arr, 2 ), arg_name );
        if( avs_is_error( res2 ) )
        {
            x264_cli_log( "avs", X264_LOG_ERROR, "couldn't convert range: %s\n", avs_as_error( res2 ) );
            avs_release_value_if_defined( h, &res2 );
            goto fail;
        }
        res = update_clip( h, &vi, res2, res );
        FAIL_IF_ERROR_CLEANUP( !vi, "failed to read video info\n" );
        FAIL_IF_ERROR_CLEANUP( invalid_avs_dimensions( vi->width, vi->height, bit_depth > 8 ),
                               "invalid video dimensions (%dx%d)\n", vi->width, vi->height );
        FAIL_IF_ERROR_CLEANUP( vi->num_frames <= 0, "invalid frame count %d\n", vi->num_frames );
        // notification that the input range has changed to the desired one
        input_range = opt->output_range;
    }
#endif
    if( bit_depth > 8 && bit_depth != opt->x264_bit_depth && bit_depth != 16 )
    {
        AVS_Value arg_arr[] = { res, avs_new_value_int( 0 ), avs_new_value_int( 0 ), avs_new_value_int( 0 ),
                                     avs_new_value_int( 0 ), avs_new_value_int( 0 ),
                                     avs_new_value_int( bit_depth ), avs_new_value_int( 2 ),
                                     avs_new_value_int( opt->x264_bit_depth ), avs_new_value_int( opt->x264_bit_depth > 8 ? 2 : 0 ) };
        const char *arg_name[] = { NULL, "Y", "Cb", "Cr",
                                          "grainY", "grainC",
                                          "input_depth", "input_mode",
                                          "output_depth", "output_mode" };
        AVS_Value res2 = h->func.avs_invoke( h->env, "f3kdb", avs_new_value_array( arg_arr, 10 ), arg_name );
        x264_cli_log( "avs", X264_LOG_WARNING, "performing bit depth conversion using f3kdb: %d->%d\n", bit_depth, opt->x264_bit_depth );
        if( avs_is_error( res2 ) )
        {
            x264_cli_log( "avs", X264_LOG_ERROR, "couldn't convert bit depth: %s\n", avs_as_error( res2 ) );
            avs_release_value_if_defined( h, &res2 );
            goto fail;
        }
        res = update_clip( h, &vi, res2, res );
        FAIL_IF_ERROR_CLEANUP( !vi, "failed to read video info\n" );
        FAIL_IF_ERROR_CLEANUP( invalid_avs_dimensions( vi->width, vi->height, bit_depth > 8 ),
                               "invalid video dimensions (%dx%d)\n", vi->width, vi->height );
        FAIL_IF_ERROR_CLEANUP( vi->num_frames <= 0, "invalid frame count %d\n", vi->num_frames );
        // notification that the input bit depth has changed to the desired one
        bit_depth = opt->x264_bit_depth;
    }

    FAIL_IF_ERROR_CLEANUP( !vi->fps_numerator || !vi->fps_denominator,
                           "invalid framerate/timebase %u/%u\n", vi->fps_numerator, vi->fps_denominator );

    updated_info.width   = vi->width;
    updated_info.height  = vi->height;
    updated_info.fps_num = vi->fps_numerator;
    updated_info.fps_den = vi->fps_denominator;
    h->num_frames = updated_info.num_frames = vi->num_frames;
    updated_info.thread_safe = 1;
    if( AVS_IS_RGB64( vi ) )
        updated_info.csp = X264_CSP_BGRA | X264_CSP_VFLIP | X264_CSP_HIGH_DEPTH;
    else if( avs_is_rgb32( vi ) )
        updated_info.csp = X264_CSP_BGRA | X264_CSP_VFLIP;
    else if( AVS_IS_RGB48( vi ) )
        updated_info.csp = X264_CSP_BGR | X264_CSP_VFLIP | X264_CSP_HIGH_DEPTH;
    else if( avs_is_rgb24( vi ) )
        updated_info.csp = X264_CSP_BGR | X264_CSP_VFLIP;
    else if( AVS_IS_YUV444P16( vi ) )
        updated_info.csp = X264_CSP_I444 | X264_CSP_HIGH_DEPTH;
    else if( AVS_IS_YV24( vi ) )
        updated_info.csp = X264_CSP_I444;
    else if( AVS_IS_YUV422P16( vi ) )
        updated_info.csp = X264_CSP_I422 | X264_CSP_HIGH_DEPTH;
    else if( AVS_IS_YV16( vi ) )
        updated_info.csp = X264_CSP_I422;
    else if( AVS_IS_YUV420P16( vi ) )
        updated_info.csp = X264_CSP_I420 | X264_CSP_HIGH_DEPTH;
    else if( AVS_IS_YV12( vi ) )
        updated_info.csp = X264_CSP_I420;
    else if( AVS_IS_Y16( vi ) )
        updated_info.csp = X264_CSP_I400 | X264_CSP_HIGH_DEPTH;
    else if( AVS_IS_Y8( vi ) )
        updated_info.csp = X264_CSP_I400;
    else if( avs_is_yuy2( vi ) )
        updated_info.csp = X264_CSP_YUYV;
#if HAVE_SWSCALE
    else if( AVS_IS_YV411( vi ) )
        updated_info.csp = AV_PIX_FMT_YUV411P | X264_CSP_OTHER;
#endif
    else
    {
        AVS_Value pixel_type = h->func.avs_invoke( h->env, "PixelType", res, NULL );
        const char *pixel_type_name = avs_is_string( pixel_type ) ? avs_as_string( pixel_type ) : "unknown";
        x264_cli_log( "avs", X264_LOG_ERROR, "not supported pixel type: %s\n", pixel_type_name );
        avs_release_value_if_defined( h, &pixel_type );
        goto fail;
    }
    avs_release_value_if_defined( h, &res );
    updated_info.vfr = 0;
    if( !opt->b_accurate_fps )
        x264_ntsc_fps( &updated_info.fps_num, &updated_info.fps_den );

    if( bit_depth > 8  && !(updated_info.csp & X264_CSP_HIGH_DEPTH) )
    {
        FAIL_IF_ERROR_CLEANUP( updated_info.width & 3, "avisynth 16bit hack requires that width is at least mod4\n" );
        x264_cli_log( "avs", X264_LOG_INFO, "avisynth 16bit hack enabled\n" );
        updated_info.csp |= X264_CSP_HIGH_DEPTH;
        updated_info.width >>= 1;
        if( bit_depth == opt->x264_bit_depth )
        {
            /* HACK: totally skips depth filter to prevent dither error */
            updated_info.csp |= X264_CSP_SKIP_DEPTH_FILTER;
        }
    }
    FAIL_IF_ERROR_CLEANUP( invalid_dimensions( updated_info.width, updated_info.height ),
                           "invalid video dimensions (%dx%d)\n", updated_info.width, updated_info.height );

    *info = updated_info;
    opt->input_range = input_range;
    opt->bit_depth = bit_depth;
    *p_handle = h;
    return 0;

fail:
#ifdef _WIN32
    free( ansi_filename );
#endif
    avs_release_value_if_defined( h, &res );
    avs_cleanup_handle( h );
    return -1;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    if( !pic )
        return -1;
    if( x264_cli_pic_alloc( pic, X264_CSP_NONE, width, height ) )
        return -1;
    pic->img.csp = csp;
    const x264_cli_csp_t *cli_csp = x264_cli_get_csp( csp );
    if( cli_csp )
        pic->img.planes = cli_csp->planes;
#if HAVE_SWSCALE
    else if( csp == (AV_PIX_FMT_YUV411P | X264_CSP_OTHER) )
        pic->img.planes = 3;
    else
        pic->img.planes = 1; //y8 and yuy2 are one plane
#endif
    return 0;
}

static int read_frame( cli_pic_t *pic, hnd_t handle, int i_frame )
{
    static const int plane[] = { AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    X264_STATIC_ASSERT( ARRAY_ELEMS(plane) == X264_AVS_PLANES, "Avisynth plane table size must match YUV plane domain" );
    avs_hnd_t *h = handle;
    if( !pic || !h || !h->clip || !h->func.avs_get_frame || !h->func.avs_clip_get_error ||
        !h->func.avs_release_video_frame || pic->img.planes <= 0 || pic->img.planes > X264_AVS_PLANES ||
        i_frame < 0 || i_frame >= h->num_frames )
        return -1;
    AVS_VideoFrame *frm = h->func.avs_get_frame( h->clip, i_frame );
    const char *err = h->func.avs_clip_get_error( h->clip );
    if( err )
    {
        if( frm )
            h->func.avs_release_video_frame( frm );
        x264_cli_log( "avs", X264_LOG_ERROR, "%s occurred while reading frame %d\n", err, i_frame );
        return -1;
    }
    if( !frm )
    {
        x264_cli_log( "avs", X264_LOG_ERROR, "failed to read frame %d\n", i_frame );
        return -1;
    }
    uint8_t *pic_plane[X264_AVS_PLANES];
    int pic_stride[X264_AVS_PLANES];
    for( int i = 0; i < pic->img.planes; i++ )
    {
        /* explicitly cast away the const attribute to avoid a warning */
        pic_plane[i] = (uint8_t*)AVS_GET_READ_PTR_P( frm, plane[i] );
        if( !pic_plane[i] )
        {
            h->func.avs_release_video_frame( frm );
            return -1;
        }
        pic_stride[i] = AVS_GET_PITCH_P( frm, plane[i] );
    }
    pic->opaque = frm;
    for( int i = 0; i < pic->img.planes; i++ )
    {
        pic->img.plane[i] = pic_plane[i];
        pic->img.stride[i] = pic_stride[i];
    }
    return 0;
}

static int release_frame( cli_pic_t *pic, hnd_t handle )
{
    avs_hnd_t *h = handle;
    if( !pic || !h || !h->func.avs_release_video_frame || !pic->opaque )
        return -1;
    h->func.avs_release_video_frame( pic->opaque );
    return 0;
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    if( !pic )
        return;
    memset( pic, 0, sizeof(cli_pic_t) );
}

static int close_file( hnd_t handle )
{
    avs_hnd_t *h = handle;
    avs_cleanup_handle( h );
    return 0;
}

#if HAVE_AUDIO
static hnd_t open_audio( hnd_t handle, int track )
{
    avs_hnd_t *h = handle;
    if( !h || !h->filename )
        return NULL;
    if( !h->has_audio )
        return NULL;
    return x264_audio_open_from_file( "avs", h->filename, track );
}

const cli_input_t avs_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file, open_audio };
#else
const cli_input_t avs_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
#endif
