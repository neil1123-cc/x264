#include "filters/audio/internal.h"

hnd_t x264_audio_open_from_file( const char *preferred_filter_name, const char *path, int trackno )
{
    if( !path )
        return NULL;

    const char *source_name = preferred_filter_name ? preferred_filter_name : "lavf";
    audio_filter_t *source = x264_af_get_filter( source_name );

    if( !source || !source->init )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "no decoder / demuxer avilable!\n" );
        return NULL;
    }

    char trackno_arg[16];
    int trackno_len = snprintf( trackno_arg, sizeof(trackno_arg), "%d", trackno );
    if( trackno_len < 0 || (size_t)trackno_len >= sizeof(trackno_arg) )
        return NULL;

    size_t path_len = strlen( path );
    if( path_len > SIZE_MAX - (size_t)trackno_len - 2 )
        return NULL;

    hnd_t h = NULL;
    size_t init_arg_size = path_len + (size_t)trackno_len + 2;
    char *init_arg = malloc( init_arg_size );
    if( !init_arg )
        return NULL;

    memcpy( init_arg, path, path_len );
    init_arg[path_len] = ',';
    memcpy( init_arg + path_len + 1, trackno_arg, (size_t)trackno_len + 1 );

    if( source->init( &h, init_arg ) < 0 || !h )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "error initializing source filter!\n" );
        free( init_arg );
        return NULL;
    }
    free( init_arg );
    return h;
}
