#include "filters/audio/internal.h"
#include <stdint.h>
#include <math.h>

static int x264_af_mul_size( size_t a, size_t b, size_t *out )
{
    if( b && a > SIZE_MAX / b )
        return -1;
    *out = a * b;
    return 0;
}

static int x264_af_buffer_size( unsigned channels, unsigned samplecount, size_t elem_size, size_t *size )
{
    size_t samples;
    if( !elem_size || (samplecount && !channels) )
        return -1;
    if( x264_af_mul_size( channels, samplecount, &samples ) || x264_af_mul_size( samples, elem_size, size ) )
        return -1;
    return 0;
}

float **x264_af_get_buffer( unsigned channels, unsigned samplecount )
{
    size_t sample_size;
    size_t channels_size;
    if( samplecount && !channels )
        return NULL;
    if( x264_af_mul_size( channels, sizeof(float*), &channels_size ) ||
        x264_af_mul_size( samplecount, sizeof(float), &sample_size ) )
        return NULL;
    float **samples = malloc( channels_size );
    if( !samples && channels_size )
        return NULL;
    for( unsigned i = 0; i < channels; i++ )
    {
        samples[i] = sample_size ? malloc( sample_size ) : NULL;
        if( !samples[i] && sample_size )
        {
            x264_af_free_buffer( samples, i );
            return NULL;
        }
    }
    return samples;
}

int x264_af_resize_buffer( float **buffer, unsigned channels, unsigned old_samplecount, unsigned new_samplecount )
{
    size_t old_sample_size;
    size_t new_sample_size;
    if( (old_samplecount || new_samplecount) && !channels )
        return -1;
    if( channels && !buffer )
        return -1;
    if( !channels )
        return 0;
    if( x264_af_mul_size( old_samplecount, sizeof(float), &old_sample_size ) ||
        x264_af_mul_size( new_samplecount, sizeof(float), &new_sample_size ) )
        return -1;
    float **resized = x264_af_get_buffer( channels, new_samplecount );
    if( !resized )
        return -1;
    for( unsigned c = 0; c < channels; c++ )
    {
        if( old_sample_size && !buffer[c] )
        {
            x264_af_free_buffer( resized, channels );
            return -1;
        }
        if( old_sample_size && new_sample_size )
            memcpy( resized[c], buffer[c], X264_MIN( old_sample_size, new_sample_size ) );
    }
    for( unsigned c = 0; c < channels; c++ )
    {
        free( buffer[c] );
        buffer[c] = resized[c];
    }
    free( resized );
    return 0;
}

int x264_af_resize_fill_buffer( float **buffer, unsigned out_samplecount, unsigned channels, unsigned in_samplecount, float value )
{
    if( in_samplecount > out_samplecount )
        return -1;
    if( x264_af_resize_buffer( buffer, channels, in_samplecount, out_samplecount ) )
        return -1;
    for( unsigned c = 0; c < channels; c++ )
        for( unsigned s = in_samplecount; s < out_samplecount; s++ )
            buffer[c][s] = value;
    return 0;
}

float **x264_af_dup_buffer( float **buffer, unsigned channels, unsigned samplecount )
{
    size_t sample_size;
    if( samplecount && (!channels || !buffer) )
        return NULL;
    if( x264_af_mul_size( samplecount, sizeof(float), &sample_size ) )
        return NULL;
    float **buf = x264_af_get_buffer( channels, samplecount );
    if( !buf )
        return NULL;
    for( unsigned c = 0; c < channels; c++ )
    {
        if( sample_size )
        {
            if( !buffer[c] )
            {
                x264_af_free_buffer( buf, channels );
                return NULL;
            }
            memcpy( buf[c], buffer[c], sample_size );
        }
    }
    return buf;
}

void x264_af_free_buffer( float **buffer, unsigned channels )
{
    if( !buffer )
        return;
    for( unsigned c = 0; c < channels; c++ )
        free( buffer[c] );
    free( buffer );
}

int x264_af_cat_buffer( float **buf, unsigned bufsamples, float **in, unsigned insamples, unsigned channels )
{
    if( (bufsamples || insamples) && !channels )
        return -1;
    if( bufsamples > UINT_MAX - insamples )
        return -1;
    if( channels && !buf )
        return -1;
    if( insamples )
    {
        if( !in )
            return -1;
        for( unsigned c = 0; c < channels; c++ )
            if( !in[c] )
                return -1;
    }
    if( x264_af_resize_buffer( buf, channels, bufsamples, bufsamples + insamples ) < 0 )
        return -1;
    for( unsigned c = 0; c < channels; c++ )
    {
        if( insamples && !buf[c] )
            return -1;
        for( unsigned s = 0; s < insamples; s++ )
            buf[c][bufsamples+s] = in[c][s];
    }
    return 0;
}

static inline int x264_is_interleaved_format(int fmt)
{
    return fmt >= SMPFMT_U8 && fmt <= SMPFMT_DBL;
}

static inline int x264_interleaved_format(int fmt)
{
    if (fmt >= SMPFMT_U8P && fmt <= SMPFMT_DBLP)
        fmt -= (SMPFMT_U8P - SMPFMT_U8);
    return fmt;
}

float **x264_af_deinterleave ( float *samples, unsigned channels, unsigned samplecount )
{
    size_t totalsamples;
    if( x264_af_mul_size( channels, samplecount, &totalsamples ) ||
        (samplecount && !channels) || (totalsamples && !samples) )
        return NULL;
    float **deint = x264_af_get_buffer( channels, samplecount );
    if( !deint )
        return NULL;
    for( unsigned s = 0; s < samplecount; s++ )
        for( unsigned c = 0; c < channels; c++ )
            deint[c][s] = samples[(size_t)s * channels + c];
    return deint;
}

float *x264_af_interleave ( float **in, unsigned channels, unsigned samplecount )
{
    size_t size;
    if( samplecount && !channels )
        return NULL;
    if( x264_af_buffer_size( channels, samplecount, sizeof(float), &size ) )
        return NULL;
    if( !size )
        return NULL;
    if( !in )
        return NULL;
    for( unsigned c = 0; c < channels; c++ )
        if( !in[c] )
            return NULL;
    float *inter = size ? malloc( size ) : NULL;
    if( !inter )
        return NULL;
    for( unsigned c = 0; c < channels; c++ )
        for( unsigned s = 0; s < samplecount; s++ )
            inter[(size_t)s * channels + c] = in[c][s];
    return inter;
}

float **x264_af_deinterleave2( uint8_t *samples, enum SampleFmt fmt, unsigned channels, unsigned samplecount )
{
    if( samplecount && (!channels || !samples) )
        return NULL;
    float  *in  = (float*) x264_af_convert( SMPFMT_FLT, samples, fmt, channels, samplecount );
    if( !in )
        return NULL;
    float **out;

    if (x264_is_interleaved_format(fmt))
        out = x264_af_deinterleave( in, channels, samplecount );
    else {
        unsigned i, j;
        out = x264_af_get_buffer( channels, samplecount );
        if( !out )
        {
            free( in );
            return NULL;
        }
        for (i = 0; i < channels; ++i) {
            float *base = &in[(size_t)i * samplecount];
            for (j = 0; j < samplecount; ++j)
                out[i][j] = base[j];
        }
    }
    free( in );
    return out;
}

uint8_t *x264_af_interleave2( enum SampleFmt outfmt, float **in, unsigned channels, unsigned samplecount )
{
    if( samplecount && (!channels || !in) )
        return NULL;
    float   *tmp = x264_af_interleave( in, channels, samplecount );
    if( !tmp )
        return NULL;
    uint8_t *out = x264_af_convert( outfmt, (uint8_t*) tmp, SMPFMT_FLT, channels, samplecount );
    free( tmp );
    return out;
}

uint8_t *x264_af_interleave3( enum SampleFmt outfmt, float **in, unsigned channels, unsigned samplecount, int *map )
{
    float *map_tmp[8];
    if( channels > ARRAY_ELEMS(map_tmp) )
        return NULL;
    if( !samplecount )
        return NULL;
    if( !channels || !in || !map )
        return NULL;
    for( unsigned i = 0; i < channels; i++ )
    {
        if( map[i] < 0 || (unsigned)map[i] >= channels || !in[map[i]] )
            return NULL;
        map_tmp[i] = in[map[i]];
    }
    float   *tmp = x264_af_interleave( map_tmp, channels, samplecount );
    if( !tmp )
        return NULL;
    uint8_t *out = x264_af_convert( outfmt, (uint8_t*) tmp, SMPFMT_FLT, channels, samplecount );
    free( tmp );
    return out;
}

static inline int samplesize( enum SampleFmt fmt )
{
    switch( fmt )
    {
    case SMPFMT_U8:
    case SMPFMT_U8P:
        return 1;
    case SMPFMT_S16:
    case SMPFMT_S16P:
        return 2;
    case SMPFMT_S32:
    case SMPFMT_S32P:
    case SMPFMT_FLT:
    case SMPFMT_FLTP:
        return 4;
    case SMPFMT_DBL:
    case SMPFMT_DBLP:
        return 8;
    default:
        return 0;
    }
}

#define CLIPFUN( num, type, min, max )                                  \
    static inline type clip##num( int64_t i ) {                         \
        return (type)( ( i > max ) ? max : ( ( i < min ) ? min : i ) ); \
    }
CLIPFUN( 8,  uint8_t, 0,         UINT8_MAX )
CLIPFUN( 16, int16_t, INT16_MIN, INT16_MAX )
CLIPFUN( 32, int32_t, INT32_MIN, INT32_MAX )
#undef CLIPFUN

static inline int64_t x264_af_round_clip_float( float value, float scale, int64_t min, int64_t max )
{
    if( value != value )
        return 0;
    float scaled = value * scale;
    if( scaled <= (float)min )
        return min;
    if( scaled >= (float)max )
        return max;
    return (int64_t)llrintf( scaled );
}

static inline int64_t x264_af_round_clip_double( double value, double scale, int64_t min, int64_t max )
{
    if( value != value )
        return 0;
    double scaled = value * scale;
    if( scaled <= (double)min )
        return min;
    if( scaled >= (double)max )
        return max;
    return (int64_t)llrint( scaled );
}

static inline int32_t x264_af_floor_shift_i32( int32_t value, unsigned shift )
{
    if( !shift )
        return value;
    if( shift >= 31 )
        return value < 0 ? -1 : 0;
    if( value >= 0 )
        return value >> shift;
    return -1 - (int32_t)(((int64_t)-1 - value) >> shift);
}

uint8_t *x264_af_convert( enum SampleFmt outfmt, uint8_t *in, enum SampleFmt fmt, unsigned channels, unsigned samplecount )
{
    size_t totalsamples;
    size_t sz;
    int in_sample_size  = samplesize( fmt );
    int out_sample_size = samplesize( outfmt );
    if( in_sample_size <= 0 || out_sample_size <= 0 ||
        x264_af_buffer_size( channels, samplecount, (size_t)out_sample_size, &sz ) ||
        x264_af_mul_size( channels, samplecount, &totalsamples ) )
        return NULL;
    if( !sz )
        return NULL;
    if( !in )
        return NULL;
    uint8_t *out = malloc( sz );
    if( !out )
        return NULL;

    fmt = x264_interleaved_format(fmt);
    outfmt = x264_interleaved_format(outfmt);

    if( fmt == outfmt )
    {
        memcpy( out, in, sz );
        return out;
    }

#define CONVERT( ifmt, ofmt, otype, expr )                  \
    if( ifmt == fmt && ofmt == outfmt ) {                   \
        for( size_t i = 0; i < totalsamples; i++ )          \
        {                                                   \
            ((otype*)out)[i] = (otype)expr;                 \
        }                                                   \
        return out;                                         \
    }
#define INPUT( itype ) (((itype*)in)[i])

    CONVERT( SMPFMT_U8,  SMPFMT_S16, int16_t, ((int)INPUT( uint8_t ) - 0x80) * 256 );
    CONVERT( SMPFMT_U8,  SMPFMT_S32, int32_t, ((int)INPUT( uint8_t ) - 0x80) * 16777216 );
    CONVERT( SMPFMT_U8,  SMPFMT_FLT, float,   (INPUT( uint8_t ) - 0x80) * (1.0f / 128.0f) );
    CONVERT( SMPFMT_U8,  SMPFMT_DBL, double,  (INPUT( uint8_t ) - 0x80) * (1.0 / 128.0) );
    CONVERT( SMPFMT_S16, SMPFMT_U8,  uint8_t, x264_af_floor_shift_i32( INPUT( int16_t ), 8 ) + 0x80 );
    CONVERT( SMPFMT_S16, SMPFMT_S32, int32_t,  (int32_t)INPUT( int16_t ) * 65536 );
    CONVERT( SMPFMT_S16, SMPFMT_FLT, float,    INPUT( int16_t ) * (1.0f / 32768.0f) );
    CONVERT( SMPFMT_S16, SMPFMT_DBL, double,   INPUT( int16_t ) * (1.0 / 32768.0) );
    CONVERT( SMPFMT_S32, SMPFMT_U8,  uint8_t, x264_af_floor_shift_i32( INPUT( int32_t ), 24 ) + 0x80 );
    CONVERT( SMPFMT_S32, SMPFMT_S16, int16_t,  x264_af_floor_shift_i32( INPUT( int32_t ), 16 ) );
    CONVERT( SMPFMT_S32, SMPFMT_FLT, float,    INPUT( int32_t ) * (1.0f / 2147483648.0f) );
    CONVERT( SMPFMT_S32, SMPFMT_DBL, double,   INPUT( int32_t ) * (1.0 / 2147483648.0) );
    CONVERT( SMPFMT_FLT, SMPFMT_U8,  uint8_t,  clip8(  x264_af_round_clip_float( INPUT( float ), 128.0f, -0x80, 0x7f ) + 0x80 ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S16, int16_t, clip16( x264_af_round_clip_float( INPUT( float ), 32768.0f, INT16_MIN, INT16_MAX ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_S32, int32_t, clip32( x264_af_round_clip_float( INPUT( float ), 2147483648.0f, INT32_MIN, INT32_MAX ) ) );
    CONVERT( SMPFMT_FLT, SMPFMT_DBL, double,   INPUT( float ) );
    CONVERT( SMPFMT_DBL, SMPFMT_U8,  uint8_t,  clip8(  x264_af_round_clip_double( INPUT( double ), 128.0, -0x80, 0x7f ) + 0x80 ) );
    CONVERT( SMPFMT_DBL, SMPFMT_S16, int16_t, clip16( x264_af_round_clip_double( INPUT( double ), 32768.0, INT16_MIN, INT16_MAX ) ) );
    CONVERT( SMPFMT_DBL, SMPFMT_S32, int32_t, clip32( x264_af_round_clip_double( INPUT( double ), 2147483648.0, INT32_MIN, INT32_MAX ) ) );
    CONVERT( SMPFMT_DBL, SMPFMT_FLT, float,    INPUT( double ) );
#undef INPUT
#undef CONVERT
    free( out );
    return NULL;
}
