/*****************************************************************************
 * filters.c: common filter functions
 *****************************************************************************
 * Copyright (C) 2010-2025 x264 project
 *
 * Authors: Diogo Franco <diogomfranco@gmail.com>
 *          Steven Walters <kemuri9@gmail.com>
 *          Henrik Gramner <henrik@gramner.com>
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

#include "filters.h"
#include <errno.h>

#define RETURN_IF_ERROR( cond, ... ) RETURN_IF_ERR( cond, "options", NULL, __VA_ARGS__ )

static int x264_size_add( size_t *dst, size_t add )
{
    if( add > SIZE_MAX - *dst )
        return -1;
    *dst += add;
    return 0;
}

static int x264_insert_opt( char **opts, size_t *i, size_t opt_count, size_t *offset, size_t size,
                            const char *src, size_t length )
{
    if( *i >= opt_count || *offset >= size || length >= size - *offset )
        return -1;
    opts[(*i)++] = memcpy( (char*)opts + *offset, src, length );
    *offset += length + 1;
    return 0;
}

char **x264_split_options( const char *opt_str, const char * const *options )
{
    size_t opt_count = 0, options_count = 0, opt_str_size = 0, size = 0;
    int found_named = 0;
    const char *opt = opt_str;

    if( !opt_str )
        return NULL;
    RETURN_IF_ERROR( !options, "No option list provided\n" );

    while( options[options_count] )
        options_count++;

    do
    {
        size_t length = strcspn( opt, "," );
        const char *equals = memchr( opt, '=', length );
        RETURN_IF_ERROR( length == SIZE_MAX || x264_size_add( &opt_str_size, length + 1 ),
                         "options are too long\n" );
        if( equals )
        {
            size_t name_length = equals - opt;
            const char * const *option = options;
            RETURN_IF_ERROR( name_length > INT_MAX, "Invalid option name is too long\n" );
            while( *option && (strlen( *option ) != name_length || strncmp( opt, *option, name_length )) )
                option++;

            RETURN_IF_ERROR( !*option, "Invalid option '%.*s'\n", (int)name_length, opt );
            found_named = 1;
        }
        else
        {
            size_t option_length;
            RETURN_IF_ERROR( found_named && !length, "Empty option given after named\n" );
            RETURN_IF_ERROR( opt_count >= options_count, "Too many options given\n" );
            RETURN_IF_ERROR( found_named, "Ordered option given after named\n" );
            option_length = strlen( options[opt_count] );
            RETURN_IF_ERROR( option_length == SIZE_MAX || x264_size_add( &size, option_length + 1 ),
                             "options are too long\n" );
        }
        RETURN_IF_ERROR( opt_count == SIZE_MAX, "Too many options given\n" );
        opt_count++;
        opt += length;
    } while( *opt++ );

    RETURN_IF_ERROR( opt_count > SIZE_MAX / (2 * sizeof(char*)) - 1, "Too many options given\n" );
    size_t opt_value_count = 2 * opt_count;
    size_t opt_array_count = opt_value_count + 2;
    size_t offset = opt_array_count * sizeof(char*);
    RETURN_IF_ERROR( x264_size_add( &size, offset ) || x264_size_add( &size, opt_str_size ),
                     "options are too long\n" );
    char **opts = calloc( 1, size );
    RETURN_IF_ERROR( !opts, "malloc failed\n" );

    for( size_t i = 0; i < opt_value_count; )
    {
        size_t length = strcspn( opt_str, "," );
        const char *equals = memchr( opt_str, '=', length );
        if( equals )
        {
            size_t name_length = equals - opt_str;
            size_t value_length = length - name_length - 1;
            if( x264_insert_opt( opts, &i, opt_value_count, &offset, size, opt_str, name_length ) ||
                x264_insert_opt( opts, &i, opt_value_count, &offset, size, equals + 1, value_length ) )
                goto fail;
        }
        else
        {
            const char *option = options[i/2];
            size_t option_length = strlen( option );
            if( x264_insert_opt( opts, &i, opt_value_count, &offset, size, option, option_length ) ||
                x264_insert_opt( opts, &i, opt_value_count, &offset, size, opt_str, length ) )
                goto fail;
        }
        opt_str += length;
        opt_str += !!*opt_str;
    }

    if( offset != size )
        goto fail;
    return opts;

fail:
    free( opts );
    RETURN_IF_ERROR( 1, "options are too long\n" );
}

char *x264_get_option( const char *name, char **split_options )
{
    if( name && split_options )
    {
        size_t last_i = SIZE_MAX;
        for( size_t i = 0; split_options[i]; i += 2 )
            if( !strcmp( split_options[i], name ) )
                last_i = i;
        if( last_i != SIZE_MAX && split_options[last_i+1][0] )
            return split_options[last_i+1];
    }
    return NULL;
}

int x264_otob( const char *str, int def )
{
   if( str )
       return !strcasecmp( str, "true" ) || !strcmp( str, "1" ) || !strcasecmp( str, "yes" );
   return def;
}

int x264_otob_checked( const char *str, int *dst )
{
    if( !str )
        return -1;
    if( !strcmp( str, "1" ) || !strcasecmp( str, "true" ) || !strcasecmp( str, "yes" ) )
    {
        *dst = 1;
        return 0;
    }
    if( !strcmp( str, "0" ) || !strcasecmp( str, "false" ) || !strcasecmp( str, "no" ) )
    {
        *dst = 0;
        return 0;
    }
    return -1;
}

static int x264_parse_filter_float( const char *str, double *dst )
{
    double ret;
    char *end;
    const char *p;

    if( !str || !dst )
        return -1;

    p = str;
    if( *p == '-' )
        p++;
    if( !( (*p >= '0' && *p <= '9') || *p == '.' ) )
        return -1;

    errno = 0;
    ret = strtod( str, &end );
    if( end == str || *end != '\0' || errno == ERANGE )
        return -1;

    *dst = ret;
    return 0;
}

double x264_otof( const char *str, double def )
{
   double ret;
   return x264_parse_filter_float( str, &ret ) ? def : ret;
}

int x264_otof_checked( const char *str, double *dst )
{
    return x264_parse_filter_float( str, dst );
}

static int x264_parse_filter_int( const char *str, int *dst )
{
    long ret;
    char *end;

    if( !str || !dst )
        return -1;

    if( !( (*str >= '0' && *str <= '9') || *str == '-' ) )
        return -1;

    errno = 0;
    ret = strtol( str, &end, 0 );
    if( end == str || *end != '\0' || errno == ERANGE || ret < INT_MIN || ret > INT_MAX )
        return -1;

    int parsed_value = (int)ret;
    *dst = parsed_value;
    return 0;
}

int x264_otoi( const char *str, int def )
{
    int ret;
    return x264_parse_filter_int( str, &ret ) ? def : ret;
}

int x264_otoi_checked( const char *str, int *dst )
{
    return x264_parse_filter_int( str, dst );
}

char *x264_otos( char *str, char *def )
{
    return str ? str : def;
}
