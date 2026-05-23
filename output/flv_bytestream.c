/*****************************************************************************
 * flv_bytestream.c: flv muxer utilities
 *****************************************************************************
 * Copyright (C) 2009-2025 x264 project
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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

#include "output.h"
#include "flv_bytestream.h"
#include <limits.h>

uint64_t flv_dbl2int( double value )
{
    return (union {double f; uint64_t i;}){value}.i;
}

/* Put functions  */

void flv_put_byte( flv_buffer *c, uint8_t b )
{
    flv_append_data( c, &b, 1 );
}

void flv_put_be32( flv_buffer *c, uint32_t val )
{
    flv_put_byte( c, (uint8_t)(val >> 24) );
    flv_put_byte( c, (uint8_t)(val >> 16) );
    flv_put_byte( c, (uint8_t)(val >> 8) );
    flv_put_byte( c, (uint8_t)val );
}

static void flv_put_be64( flv_buffer *c, uint64_t val )
{
    flv_put_be32( c, (uint32_t)(val >> 32) );
    flv_put_be32( c, (uint32_t)val );
}

void flv_put_be16( flv_buffer *c, uint16_t val )
{
    flv_put_byte( c, (uint8_t)(val >> 8) );
    flv_put_byte( c, (uint8_t)val );
}

void flv_put_be24( flv_buffer *c, uint32_t val )
{
    flv_put_be16( c, (uint16_t)(val >> 8) );
    flv_put_byte( c, (uint8_t)val );
}

void flv_put_tag( flv_buffer *c, const char *tag )
{
    if( !c || !tag )
    {
        if( c )
            c->error = 1;
        return;
    }

    while( *tag )
        flv_put_byte( c, (uint8_t)*tag++ );
}

void flv_put_amf_string( flv_buffer *c, const char *str )
{
    if( !c || !str )
    {
        if( c )
            c->error = 1;
        return;
    }

    size_t len = strlen( str );
    if( len > UINT16_MAX )
    {
        c->error = 1;
        return;
    }

    uint16_t amf_len = (uint16_t)len;
    flv_put_be16( c, amf_len );
    flv_append_data( c, (const uint8_t*)str, amf_len );
}

void flv_put_amf_double( flv_buffer *c, double d )
{
    flv_put_byte( c, AMF_DATA_TYPE_NUMBER );
    flv_put_be64( c, flv_dbl2int( d ) );
}

void flv_put_amf_bool( flv_buffer *c, int i )
{
    flv_put_byte( c, AMF_DATA_TYPE_BOOL );
    flv_put_byte( c, !!i );
}

/* flv writing functions */

flv_buffer *flv_create_writer( const char *filename )
{
    if( !filename )
        return NULL;

    int64_t writer_alloc_size = sizeof(flv_buffer);
    flv_buffer *c = calloc( 1, writer_alloc_size );
    if( !c )
        return NULL;

    if( !strcmp( filename, "-" ) )
        c->fp = stdout;
    else
        c->fp = x264_fopen( filename, "wb" );
    if( !c->fp )
    {
        free( c );
        return NULL;
    }

    return c;
}

static int flv_reserve_data( flv_buffer *c, unsigned size )
{
    if( size <= c->d_max )
        return 0;

    unsigned dn = c->d_max ? c->d_max : 16;
    while( size > dn )
    {
        if( dn > UINT_MAX / 2 )
        {
            dn = size;
            break;
        }
        dn <<= 1;
    }

    int64_t data_alloc_size = dn;
    void *dp = realloc( c->data, data_alloc_size );
    if( !dp )
        return -1;

    c->data = dp;
    c->d_max = dn;
    return 0;
}

int flv_append_data( flv_buffer *c, const uint8_t *data, unsigned size )
{
    if( !c )
        return -1;
    if( c->error )
        return -1;
    if( size && !data )
    {
        c->error = 1;
        return -1;
    }
    if( size > UINT_MAX - c->d_cur )
    {
        c->error = 1;
        return -1;
    }
    unsigned ns = c->d_cur + size;

    if( flv_reserve_data( c, ns ) )
    {
        c->error = 1;
        return -1;
    }

    if( size )
        memcpy( c->data + c->d_cur, data, size );

    c->d_cur = ns;

    return 0;
}

void flv_rewrite_amf_be24( flv_buffer *c, unsigned length, unsigned start )
{
    if( !c || c->error )
        return;
    if( length > 0xFFFFFF || start > c->d_cur || c->d_cur - start < 3 || !c->data )
    {
        c->error = 1;
        return;
    }

    *(c->data + start + 0) = (uint8_t)(length >> 16);
    *(c->data + start + 1) = (uint8_t)(length >> 8);
    *(c->data + start + 2) = (uint8_t)length;
}

int flv_flush_data( flv_buffer *c )
{
    if( !c || c->error || !c->fp )
        return -1;
    if( !c->d_cur )
        return 0;
    if( !c->data )
    {
        c->error = 1;
        return -1;
    }
    if( c->d_total > UINT64_MAX - c->d_cur )
    {
        c->error = 1;
        return -1;
    }

    if( fwrite( c->data, c->d_cur, 1, c->fp ) != 1 )
    {
        c->error = 1;
        return -1;
    }

    c->d_total += c->d_cur;

    c->d_cur = 0;

    return 0;
}
