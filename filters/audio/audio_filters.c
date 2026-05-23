#include "filters/audio/internal.h"

static void x264_af_free_packet_default( audio_packet_t *pkt )
{
    if( pkt->priv )
        free( pkt->priv );
    if( pkt->data )
        free( pkt->data );
    if( pkt->samples )
    {
        unsigned channels = pkt->channels;
        if( pkt->info.channels > 0 && (!channels || channels > (unsigned)pkt->info.channels) )
            channels = (unsigned)pkt->info.channels;
        x264_af_free_buffer( pkt->samples, channels );
    }
    free( pkt );
}

static int x264_af_validate_sample_packet( audio_hnd_t *h, audio_packet_t *pkt, uint64_t requested )
{
    if( pkt->size < 0 || pkt->samplecount > requested )
        return -1;
    if( pkt->samplecount )
    {
        if( h->info.channels <= 0 || pkt->channels != (unsigned)h->info.channels || !pkt->samples )
            return -1;
        for( unsigned c = 0; c < pkt->channels; c++ )
            if( !pkt->samples[c] )
                return -1;
    }
    return 0;
}

audio_info_t *x264_af_get_info( hnd_t handle )
{
    return handle ? &((audio_hnd_t*)handle)->info : NULL;
}

audio_filter_t *x264_af_get_filter( const char *name )
{
    if( !name )
        return NULL;
#if HAVE_AUDIO
#define CHECK( filter )                                 \
    extern audio_filter_t audio_filter_##filter;        \
    if ( !strcmp( name, audio_filter_##filter.name ) )  \
        return &audio_filter_##filter
#if HAVE_LAVF
    CHECK( lavf );
#endif
#if HAVE_AVS
    CHECK( avs );
#endif
#undef CHECKFLT
#undef CHECK
#endif /* HAVE_AUDIO */
    return NULL;
}

audio_packet_t *x264_af_get_samples( hnd_t handle, int64_t first_sample, int64_t last_sample )
{
    audio_hnd_t *h = handle;
    if( !h || !h->self || !h->self->get_samples ||
        first_sample < 0 || last_sample <= first_sample )
        return NULL;

    uint64_t requested = (uint64_t)last_sample - (uint64_t)first_sample;
    if( requested > UINT_MAX )
        return NULL;

    audio_packet_t *out = h->self->get_samples( h, first_sample, last_sample );
    if( out )
    {
        out->owner = h;
        if( x264_af_validate_sample_packet( h, out, requested ) )
        {
            x264_af_free_packet( out );
            return NULL;
        }
        return out;
    }
    return 0;
}

void x264_af_free_packet( audio_packet_t *pkt )
{
    if( !pkt )
        return;
    audio_hnd_t *owner = pkt->owner;
    if( owner && owner->self && owner->self->free_packet )
    {
        pkt->owner = NULL;
        owner->self->free_packet( owner, pkt );
    }
    else
        x264_af_free_packet_default( pkt );
}

void x264_af_close( hnd_t chain )
{
    audio_hnd_t *h = chain;
    if( !h )
        return;
    if( h->prev )
        x264_af_close( h->prev );
    if( h->self && h->self->close )
        h->self->close( h );
}
