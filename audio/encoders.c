#include "audio/encoders.h"

#include <stdlib.h>

typedef struct {
    const char *codec;
    const char *name;
    const audio_encoder_t *encoder;
} audio_encoder_entry_t;

const audio_encoder_entry_t registered_audio_encoders[] = {
#if HAVE_AUDIO
    { "raw",        "raw",               &audio_encoder_raw, },
#if HAVE_LAME
    { "mp3",        "lame",              &audio_encoder_lame, },
#endif
#if HAVE_QT_AAC
    { "aac",        "qtaac",             &audio_encoder_qtaac, },
#endif
#if HAVE_FAAC
    { "aac",        "faac",              &audio_encoder_faac, },
#endif
#if HAVE_LAVF
#if HAVE_NONFREE
    { "aac",        "libfdk_aac",        &audio_encoder_lavc, },
#endif
    { "aac",        "libvo_aacenc",      &audio_encoder_lavc, },
    { "aac",        "aac",               &audio_encoder_lavc, },
#if HAVE_NONFREE
    { "aac",        "libaacplus",        &audio_encoder_lavc, },
#endif
    { "ac3",        "ac3",               &audio_encoder_lavc, },
    { "alac",       "alac",              &audio_encoder_lavc, },
    { "amrwb",      "libvo_amrwbenc",    &audio_encoder_lavc, },
    { "mp2",        "mp2",               &audio_encoder_lavc, },
    { "pcm_f32be",  "pcm_f32be",         &audio_encoder_lavc, },
    { "pcm_f32le",  "pcm_f32le",         &audio_encoder_lavc, },
    { "pcm_f64be",  "pcm_f64be",         &audio_encoder_lavc, },
    { "pcm_f64le",  "pcm_f64le",         &audio_encoder_lavc, },
    { "pcm_s16be",  "pcm_s16be",         &audio_encoder_lavc, },
    { "pcm_s16le",  "pcm_s16le",         &audio_encoder_lavc, },
    { "pcm_s24be",  "pcm_s24be",         &audio_encoder_lavc, },
    { "pcm_s24le",  "pcm_s24le",         &audio_encoder_lavc, },
    { "pcm_s32be",  "pcm_s32be",         &audio_encoder_lavc, },
    { "pcm_s32le",  "pcm_s32le",         &audio_encoder_lavc, },
    { "pcm_s8",     "pcm_s8",            &audio_encoder_lavc, },
    { "pcm_u16be",  "pcm_u16be",         &audio_encoder_lavc, },
    { "pcm_u16le",  "pcm_u16le",         &audio_encoder_lavc, },
    { "pcm_u24be",  "pcm_u24be",         &audio_encoder_lavc, },
    { "pcm_u24le",  "pcm_u24le",         &audio_encoder_lavc, },
    { "pcm_u32be",  "pcm_u32be",         &audio_encoder_lavc, },
    { "pcm_u32le",  "pcm_u32le",         &audio_encoder_lavc, },
    { "pcm_u8",     "pcm_u8",            &audio_encoder_lavc, },
    { "vorbis",     "libvorbis",         &audio_encoder_lavc, },
    { "vorbis",     "vorbis",            &audio_encoder_lavc, },
    { "amrnb",      "libopencore_amrnb", &audio_encoder_lavc, },
#endif
#if HAVE_AMRWB_3GPP
    { "amrwb",      "amrnb_3gpp",        &audio_encoder_amrwb_3gpp, },
#endif
#endif /* HAVE_AUDIO */
    { NULL, NULL, NULL },
};

static int x264_audio_encoder_entry_valid( const audio_encoder_entry_t *entry )
{
    return entry->codec && entry->name && entry->encoder;
}

static int x264_audio_encoder_entry_is_lavc( const audio_encoder_entry_t UNUSED *entry )
{
#if HAVE_AUDIO && HAVE_LAVF
    return entry->encoder == &audio_encoder_lavc;
#else
    return 0;
#endif
}

static int x264_audio_encoder_name_matches( const audio_encoder_entry_t *entry, const char *name, int mode )
{
    if( !entry || !name || !x264_audio_encoder_entry_valid( entry ) )
        return 0;
    if( !strcmp( name, mode == QUERY_CODEC ? entry->codec : entry->name ) )
        return 1;
    if( mode == QUERY_ENCODER && x264_audio_encoder_entry_is_lavc( entry ) &&
        !strncmp( name, "ff", 2 ) && !strcmp( name + 2, entry->name ) )
        return 1;
    return 0;
}

static size_t x264_audio_saturating_add( size_t a, size_t b )
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

struct aenc_t
{
    const audio_encoder_t *enc;
    hnd_t handle;
    hnd_t filters;
};

hnd_t x264_audio_encoder_open( const audio_encoder_t *encoder, hnd_t filter_chain, const char *opts )
{
    if( !encoder || !filter_chain || !encoder->init )
        return NULL;
    struct aenc_t *enc = calloc( 1, sizeof( struct aenc_t ) );
    if( !enc )
    {
        x264_af_close( filter_chain );
        return NULL;
    }
    enc->enc           = encoder;
    enc->handle        = encoder->init( filter_chain, opts );
    enc->filters       = filter_chain;

    if( !enc->handle )
    {
        x264_af_close( filter_chain );
        free( enc );
        return NULL;
    }
    return enc;
}

audio_info_t *x264_audio_encoder_info( hnd_t encoder )
{
    struct aenc_t *enc = encoder;
    if( !enc || !enc->enc || !enc->handle || !enc->enc->get_info )
        return NULL;

    return enc->enc->get_info( enc->handle );
}

audio_packet_t *x264_audio_encode_frame( hnd_t encoder )
{
    struct aenc_t *enc = encoder;
    if( !enc || !enc->enc || !enc->handle || !enc->enc->get_next_packet )
        return NULL;

    return enc->enc->get_next_packet( enc->handle );
}

void x264_audio_encoder_skip_samples( hnd_t encoder, uint64_t samplecount )
{
    struct aenc_t *enc = encoder;
    if( !enc || !enc->enc || !enc->handle || !enc->enc->skip_samples )
        return;

    return enc->enc->skip_samples( enc->handle, samplecount );
}

audio_packet_t *x264_audio_encoder_finish( hnd_t encoder )
{
    struct aenc_t *enc = encoder;
    if( !enc || !enc->enc || !enc->handle || !enc->enc->finish )
        return NULL;

    return enc->enc->finish( enc->handle );
}

int x264_audio_encoder_failed( hnd_t encoder )
{
    struct aenc_t *enc = encoder;
    if( !enc || !enc->enc || !enc->handle || !enc->enc->is_failed )
        return 0;

    return enc->enc->is_failed( enc->handle );
}

void x264_audio_free_frame( hnd_t encoder, audio_packet_t *frame )
{
    struct aenc_t *enc = encoder;
    if( !enc || !enc->enc || !enc->handle || !frame || !enc->enc->free_packet )
        return;

    return enc->enc->free_packet( enc->handle, frame );
}

void x264_audio_encoder_close( hnd_t encoder )
{
    if( !encoder )
        return;
    struct aenc_t *enc = encoder;

    if( enc->enc && enc->handle && enc->enc->close )
        enc->enc->close( enc->handle );
    if( enc->filters )
        x264_af_close( enc->filters );
    free( enc );
}

const audio_encoder_t *x264_audio_encoder_by_name( const char *name, int mode, const char **used_enc )
{
    if( used_enc )
        *used_enc = NULL;
    if( !name || (mode != QUERY_CODEC && mode != QUERY_ENCODER) )
        return NULL;

    const audio_encoder_entry_t *cur = NULL;
    const audio_encoder_t *ret = NULL;

    for( const audio_encoder_entry_t *entry = registered_audio_encoders; entry->codec; entry++ )
    {
        if( !x264_audio_encoder_entry_valid( entry ) ||
            !x264_audio_encoder_name_matches( entry, name, mode ) )
            continue;

        cur = entry;
        ret = cur->encoder;

        if( ret )
        {
            if( !ret->is_valid_encoder )
                break;
            else if( !ret->is_valid_encoder( cur->name, NULL ) )
                break;
            else
                ret = NULL;
        }
    }

    if( used_enc && ret && cur )
        *used_enc = cur->name;

    return ret;
}

const audio_encoder_t *x264_select_audio_encoder( const char *encoder, char* allowed_list[], const char **used_enc )
{
    if( used_enc )
        *used_enc = NULL;
    if( !encoder )
        return NULL;
    if( allowed_list )
    {
        if( !strcmp( encoder, "auto" ) )
        {
            const audio_encoder_t *enc;
            for( int i = 0; allowed_list[i] != NULL; i++ )
            {
                enc = x264_audio_encoder_by_name( allowed_list[i], QUERY_CODEC, used_enc );
                if( enc )
                    return enc;
            }
            return NULL;
        }
        else
        {
            const audio_encoder_t *enc;
            for( int i = 0; allowed_list[i] != NULL; i++ )
            {
                if( !strcmp( encoder, allowed_list[i] ) )
                {
                    enc = x264_audio_encoder_by_name( allowed_list[i], QUERY_CODEC, used_enc );
                    if( enc )
                        return enc;
                }
            }
        }
    }
    return x264_audio_encoder_by_name( encoder, QUERY_ENCODER, used_enc );
}

#define INDENT "                              "

static int x264_audio_encoder_has_later_codec( const audio_encoder_entry_t *entry, const char *prev_name )
{
    for( entry++; entry->codec; entry++ )
        if( x264_audio_encoder_entry_valid( entry ) &&
            strcmp( prev_name, entry->codec ) &&
            x264_audio_encoder_by_name( entry->codec, QUERY_CODEC, NULL ) )
            return 1;
    return 0;
}

static int x264_audio_encoder_has_later_encoder( const audio_encoder_entry_t *entry )
{
    for( entry++; entry->codec; entry++ )
        if( x264_audio_encoder_entry_valid( entry ) &&
            x264_audio_encoder_by_name( entry->name, QUERY_ENCODER, NULL ) )
            return 1;
    return 0;
}

void x264_audio_encoder_list_codecs( int longhelp )
{
    if( longhelp < 1 )
        return;

    const char *prev_name = "";
    size_t len = strlen( INDENT ) + 6;

    printf( INDENT "    - " );
    for( const audio_encoder_entry_t *cur = registered_audio_encoders; cur->codec; cur++ )
    {
        if( !x264_audio_encoder_entry_valid( cur ) )
            continue;

        const char *codec_name = cur->codec;

        if( x264_audio_encoder_by_name( codec_name, QUERY_CODEC, NULL ) &&
            strcmp( prev_name, codec_name ) )
        {
            printf( "%s", codec_name );
            len = x264_audio_saturating_add( len, strlen( codec_name ) );
            prev_name = codec_name;

            if( x264_audio_encoder_has_later_codec( cur, prev_name ) )
            {
                if( len >= 80 - strlen( ", " ) )
                {
                    printf( ",\n" INDENT "      " );
                    len = strlen( INDENT ) + 6;
                }
                else
                    printf( ", " );
            }
        }
    }
    printf( "\n" );
    return;
}

void x264_audio_encoder_list_encoders( int longhelp )
{
    if( longhelp < 2 )
        return;

    size_t len = strlen( INDENT ) + 6;

    printf( INDENT "    - " );
    for( const audio_encoder_entry_t *cur = registered_audio_encoders; cur->codec; cur++ )
    {
        if( !x264_audio_encoder_entry_valid( cur ) )
            continue;

        const char *encoder_name = cur->name;
        int is_lavc = x264_audio_encoder_entry_is_lavc( cur );

        if( x264_audio_encoder_by_name( encoder_name, QUERY_ENCODER, NULL ) )
        {
            printf( "%s%s", (is_lavc ? "(ff)" : "" ), encoder_name );
            len = x264_audio_saturating_add( len, strlen( encoder_name ) );
            len = x264_audio_saturating_add( len, is_lavc ? 4 : 0 );

            if( x264_audio_encoder_has_later_encoder( cur ) )
            {
                if( len >= 80 - strlen( ", " ) )
                {
                    printf( ",\n" INDENT "      " );
                    len = strlen( INDENT ) + 6;
                }
                else
                    printf( ", " );
            }
        }
    }
    printf( "\n" );
    return;
}

#undef INDENT

void x264_audio_encoder_show_help( int longhelp )
{
    if( longhelp < 2 )
    {
        printf( "      For the encoder specific helps, see --fullhelp.\n" );
        return;
    }

    printf( "      Encoder specific helps:\n" );
#if !HAVE_AUDIO
    printf( "            There is no available audio encoder in this x264 build.\n" );
    return;
#endif
    for( const audio_encoder_entry_t *cur = registered_audio_encoders; cur->codec; cur++ )
    {
        const audio_encoder_t *enc = NULL;

        if( !x264_audio_encoder_entry_valid( cur ) )
            continue;

        enc = x264_audio_encoder_by_name( cur->name, QUERY_ENCODER, NULL );

        if( !enc || !enc->show_help )
            continue;
        enc->show_help( cur->name );
    }

    return;
}

#include "filters/audio/internal.h"

hnd_t x264_audio_copy_open( hnd_t handle )
{
    if( !handle )
        return NULL;
    audio_hnd_t UNUSED *h = handle;
    if( !h->self || !h->self->name )
        return NULL;
#define IFRET( dec )                                                                \
        extern const audio_encoder_t audio_copy_ ## dec;                            \
        if( !strcmp( #dec, h->self->name ) )                                        \
            return x264_audio_encoder_open( &( audio_copy_ ## dec ), handle, NULL );
#if HAVE_AUDIO
#if HAVE_LAVF
    IFRET( lavf );
#endif
#endif // HAVE_AUDIO
#undef IFRET
    return NULL;
}
