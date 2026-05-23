#include "filters/audio/internal.h"

hnd_t x264_audio_open_from_file( const char *preferred_filter_name, const char *path, int trackno )
{
    const char *source_name = preferred_filter_name ? preferred_filter_name : "lavf";
    audio_filter_t *source = x264_af_get_filter( source_name );

    if( !source )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "no decoder / demuxer avilable!\n" );
        return NULL;
    }
    int len = snprintf( NULL, 0, "%s,%d", path, trackno );
    if( len < 0 )
        return NULL;
    hnd_t h = NULL;
    size_t init_arg_size = (size_t)len + 1;
    char *init_arg = malloc( init_arg_size );
    if( !init_arg )
        return NULL;
    snprintf( init_arg, init_arg_size, "%s,%d", path, trackno );
    if( source->init( &h, init_arg ) < 0 || !h )
    {
        x264_cli_log( "audio", X264_LOG_ERROR, "error initializing source filter!\n" );
        free( init_arg );
        return NULL;
    }
    free( init_arg );
    return h;
}
