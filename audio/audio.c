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
    const char *track_arg = "any";
    size_t track_arg_len = 3;
    if( trackno != TRACK_ANY )
    {
        int trackno_len = snprintf( trackno_arg, sizeof(trackno_arg), "%d", trackno );
        if( trackno_len < 0 || (size_t)trackno_len >= sizeof(trackno_arg) )
            return NULL;
        track_arg = trackno_arg;
        track_arg_len = (size_t)trackno_len;
    }

    size_t path_len = strlen( path );
    if( path_len > SIZE_MAX - track_arg_len - 2 )
        return NULL;

    hnd_t h = NULL;
    size_t init_arg_size = path_len + track_arg_len + 2;
    int64_t init_arg_alloc_size = (int64_t)init_arg_size;
    char *init_arg = malloc( init_arg_alloc_size );
    if( !init_arg )
        return NULL;

    memcpy( init_arg, path, path_len );
    init_arg[path_len] = ',';
    memcpy( init_arg + path_len + 1, track_arg, track_arg_len + 1 );

    if( source->init( &h, init_arg ) < 0 || !h )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "error initializing source filter!\n" );
        x264_af_close( h );
        free( init_arg );
        return NULL;
    }
    free( init_arg );
    return h;
}
