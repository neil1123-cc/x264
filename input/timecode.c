/*****************************************************************************
 * timecode.c: timecode file input
 *****************************************************************************
 * Copyright (C) 2010-2025 x264 project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
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
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "timecode", __VA_ARGS__ )

typedef struct
{
    cli_input_t input;
    hnd_t p_handle;
    int auto_timebase_num;
    int auto_timebase_den;
    uint64_t timebase_num;
    uint64_t timebase_den;
    int stored_pts_num;
    int64_t *pts;
    double assume_fps;
    double last_timecode;
} timecode_hnd_t;

static inline double sigexp10( double value, double *exponent )
{
    /* This function separates significand and exp10 from double floating point. */
    *exponent = pow( 10.0, floor( log10( value ) ) );
    return value / *exponent;
}

#define DOUBLE_EPSILON 5e-6
#define MKV_TIMEBASE_DEN 1000000000

static int timecode_is_positive_finite( double value )
{
    return value == value && value > 0.0 && value <= DBL_MAX;
}

static int timecode_seek_to_pos( FILE *file, int64_t file_pos )
{
    return file_pos < 0 || file_pos > LONG_MAX || fseek( file, (long)file_pos, SEEK_SET );
}

static const char *timecode_skip_space( const char *arg )
{
    while( isspace( (unsigned char)*arg ) )
        arg++;
    return arg;
}

static int timecode_at_line_end( const char *arg )
{
    arg = timecode_skip_space( arg );
    return !*arg || *arg == '#';
}

static int timecode_parse_int_end( const char *arg, char **end, int *dst )
{
    const char *p;
    long value;

    if( !arg || !end || !dst )
        return -1;

    p = arg;
    if( *p == '-' )
        p++;
    if( *p < '0' || *p > '9' )
        return -1;

    errno = 0;
    value = strtol( arg, end, 10 );
    if( *end == arg || errno == ERANGE || value < INT_MIN || value > INT_MAX )
        return -1;

    *dst = (int)value;
    return 0;
}

static int timecode_match_word( const char **arg, const char *word )
{
    size_t len = strlen( word );
    const char *p = *arg;

    if( strncasecmp( p, word, len ) || (p[len] && !isspace( (unsigned char)p[len] )) )
        return -1;

    *arg = timecode_skip_space( p + len );
    return 0;
}

static int timecode_parse_header_version( const char *arg, int *version )
{
    char *end;
    int parsed_version;
    const char *p;

    if( !arg || !version )
        return -1;

    p = timecode_skip_space( arg );
    if( *p++ != '#' )
        return -1;
    p = timecode_skip_space( p );
    if( timecode_match_word( &p, "timecode" ) && timecode_match_word( &p, "timestamp" ) )
        return -1;
    if( timecode_match_word( &p, "format" ) || *p++ != 'v' ||
        timecode_parse_int_end( p, &end, &parsed_version ) || !timecode_at_line_end( end ) )
        return -1;

    *version = parsed_version;
    return 0;
}

static int timecode_parse_tdecimate_last_frame( const char *arg, int *last_frame )
{
    char *end;
    int mode, parsed_last_frame;
    const char *p;

    if( !arg || !last_frame )
        return -1;

    p = timecode_skip_space( arg );
    if( *p++ != '#' )
        return 1;
    p = timecode_skip_space( p );
    if( timecode_match_word( &p, "TDecimate" ) )
        return 1;
    if( timecode_match_word( &p, "Mode" ) )
        return 1;
    if( timecode_parse_int_end( p, &end, &mode ) )
        return 1;
    p = timecode_skip_space( end );
    if( mode != 3 )
        return 1;
    if( *p++ != ':' )
        return 1;
    p = timecode_skip_space( p );
    if( timecode_match_word( &p, "Last" ) )
        return 1;
    if( timecode_match_word( &p, "Frame" ) || *p++ != '=' )
        return -1;
    p = timecode_skip_space( p );
    if( timecode_parse_int_end( p, &end, &parsed_last_frame ) || !timecode_at_line_end( end ) )
        return -1;

    *last_frame = parsed_last_frame;
    return 0;
}

static int timecode_parse_u64_end( const char *arg, char **end, uint64_t *dst )
{
    unsigned long long value;

    if( !arg || !end || !dst )
        return -1;

    if( *arg < '0' || *arg > '9' )
        return -1;

    errno = 0;
    value = strtoull( arg, end, 10 );
    if( *end == arg || errno == ERANGE )
        return -1;

    *dst = (uint64_t)value;
    return 0;
}

static int timecode_parse_double_end( const char *arg, char **end, double *dst )
{
    const char *p;
    double value;

    if( !arg || !end || !dst )
        return -1;

    p = arg;
    if( *p == '-' )
        p++;
    if( !( (*p >= '0' && *p <= '9') || *p == '.' ) )
        return -1;

    errno = 0;
    value = strtod( arg, end );
    if( *end == arg || errno == ERANGE )
        return -1;

    *dst = value;
    return 0;
}

static int timecode_parse_double_line( const char *arg, double *dst )
{
    char *end;
    double value;

    if( !dst )
        return -1;

    if( timecode_parse_double_end( arg, &end, &value ) || !timecode_at_line_end( end ) )
        return -1;

    *dst = value;
    return 0;
}

static int timecode_parse_v1_range( const char *arg, int *start, int *end_frame, double *fps )
{
    char *end;
    const char *p = arg;
    int parsed_start, parsed_end_frame;
    double parsed_fps;

    if( !start || !end_frame || !fps )
        return -1;

    if( timecode_parse_int_end( p, &end, &parsed_start ) )
        return -1;
    p = timecode_skip_space( end );
    if( *p++ != ',' )
        return -1;
    p = timecode_skip_space( p );
    if( timecode_parse_int_end( p, &end, &parsed_end_frame ) )
        return -1;
    p = timecode_skip_space( end );
    if( *p++ != ',' )
        return -1;
    p = timecode_skip_space( p );
    if( timecode_parse_double_end( p, &end, &parsed_fps ) || !timecode_at_line_end( end ) )
        return -1;

    *start = parsed_start;
    *end_frame = parsed_end_frame;
    *fps = parsed_fps;
    return 0;
}

static int timecode_parse_timebase( const char *arg, uint64_t *num, uint64_t *den, int *has_den )
{
    char *end;
    uint64_t parsed_num, parsed_den = 0;
    int parsed_has_den;

    if( !num || !den || !has_den )
        return -1;

    if( timecode_parse_u64_end( arg, &end, &parsed_num ) )
        return -1;

    if( *end == '/' )
    {
        parsed_has_den = 1;
        if( timecode_parse_u64_end( end + 1, &end, &parsed_den ) || !timecode_at_line_end( end ) )
            return -1;
    }
    else if( timecode_at_line_end( end ) )
    {
        parsed_has_den = 0;
    }
    else
        return -1;

    if( !parsed_num || (parsed_has_den && !parsed_den) ||
        parsed_num > UINT32_MAX || parsed_den > UINT32_MAX )
        return -1;

    *num = parsed_num;
    *den = parsed_den;
    *has_den = parsed_has_den;
    return 0;
}

static int timecode_to_u64( uint64_t *dst, double value )
{
    long double value_ld = value;
    if( value != value || value_ld < 0.0L || value_ld > (long double)UINT64_MAX )
        return -1;
    *dst = (uint64_t)value;
    return 0;
}

static int timecode_round_to_u64( uint64_t *dst, double value )
{
    return timecode_to_u64( dst, round( value ) );
}

static int timecode_update_auto_den( timecode_hnd_t *h, uint64_t fps_num )
{
    if( !fps_num )
        return 0;
    if( !h->timebase_den )
    {
        h->timebase_den = fps_num;
        if( h->timebase_den > UINT32_MAX )
            h->auto_timebase_den = 0;
        return 0;
    }

    uint64_t divisor = gcd( h->timebase_den, fps_num );
    uint64_t reduced = h->timebase_den / divisor;
    if( reduced > UINT64_MAX / fps_num || reduced * fps_num > UINT32_MAX )
    {
        h->auto_timebase_den = 0;
        return 0;
    }
    h->timebase_den = reduced * fps_num;
    return 0;
}

static int timecode_fps_num_from_sig( uint64_t *fps_num, uint64_t fps_den, double fps_sig, double exponent )
{
    uint64_t rounded;
    if( !timecode_is_positive_finite( fps_sig ) || !timecode_is_positive_finite( exponent ) ||
        timecode_round_to_u64( &rounded, (double)fps_den * fps_sig ) )
        return -1;
    long double value = (long double)rounded * (long double)exponent;
    if( value != value || value <= 0.0L || value > (long double)UINT32_MAX )
        return -1;
    *fps_num = (uint64_t)value;
    return 0;
}

static int timecode_mkv_fps_den( uint64_t *fps_den, double fps_sig, double exponent )
{
    uint64_t rounded;
    if( !timecode_is_positive_finite( fps_sig ) || !timecode_is_positive_finite( exponent ) ||
        timecode_round_to_u64( &rounded, (double)MKV_TIMEBASE_DEN / fps_sig ) )
        return -1;
    long double value = (long double)rounded / (long double)exponent;
    if( value != value || value <= 0.0L || value > (long double)UINT64_MAX )
        return -1;
    *fps_den = (uint64_t)value;
    return 0;
}

static int timecode_seconds_to_pts( int64_t *pts, double timecode, const timecode_hnd_t *h )
{
    if( !timecode_is_positive_finite( timecode ) && timecode != 0.0 )
        return -1;
    if( !h->timebase_num || !h->timebase_den )
        return -1;
    long double value = (long double)timecode * (long double)h->timebase_den /
                        (long double)h->timebase_num + 0.5L;
    if( value != value || value < 0.0L || value > (long double)INT64_MAX )
        return -1;
    *pts = (int64_t)value;
    return 0;
}

static double correct_fps( double fps, timecode_hnd_t *h )
{
    uint64_t i = 1;
    uint64_t fps_num, fps_den;
    double exponent;
    FAIL_IF_ERROR( !timecode_is_positive_finite( fps ) || !h->timebase_num,
                   "tcfile fps correction failed.\n"
                   "                  Specify an appropriate timebase manually or remake tcfile.\n" );
    double fps_sig = sigexp10( fps, &exponent );
    while( 1 )
    {
        FAIL_IF_ERROR( i > UINT64_MAX / h->timebase_num, "tcfile fps correction failed.\n"
                       "                  Specify an appropriate timebase manually or remake tcfile.\n" );
        fps_den = i * h->timebase_num;
        FAIL_IF_ERROR( timecode_fps_num_from_sig( &fps_num, fps_den, fps_sig, exponent ),
                       "tcfile fps correction failed.\n"
                       "                  Specify an appropriate timebase manually or remake tcfile.\n" );
        if( fabs( ((double)fps_num / (double)fps_den) / exponent - fps_sig ) < DOUBLE_EPSILON )
            break;
        ++i;
    }
    if( h->auto_timebase_den )
        timecode_update_auto_den( h, fps_num );
    return (double)fps_num / (double)fps_den;
}

static int try_mkv_timebase_den( double *fpss, timecode_hnd_t *h, int loop_num )
{
    h->timebase_num = 0;
    h->timebase_den = MKV_TIMEBASE_DEN;
    for( int num = 0; num < loop_num; num++ )
    {
        uint64_t fps_den;
        double exponent;
        FAIL_IF_ERROR( !timecode_is_positive_finite( fpss[num] ), "automatic timebase generation failed.\n"
                       "                  Specify timebase manually.\n" );
        double fps_sig = sigexp10( fpss[num], &exponent );
        FAIL_IF_ERROR( timecode_mkv_fps_den( &fps_den, fps_sig, exponent ),
                       "automatic timebase generation failed.\n"
                       "                  Specify timebase manually.\n" );
        h->timebase_num = fps_den && h->timebase_num ? gcd( h->timebase_num, fps_den ) : fps_den;
        FAIL_IF_ERROR( h->timebase_num > UINT32_MAX || !h->timebase_num, "automatic timebase generation failed.\n"
                       "                  Specify timebase manually.\n" );
    }
    return 0;
}

static int parse_tcfile( FILE *tcfile_in, timecode_hnd_t *h, video_info_t *info )
{
    char buff[256];
    int ret, tcfv, num, seq_num, timecodes_num;
    double *timecodes = NULL;
    double *fpss = NULL;

    ret = fgets( buff, sizeof(buff), tcfile_in ) != NULL &&
          !timecode_parse_header_version( buff, &tcfv );
    FAIL_IF_ERROR( !ret || (tcfv != 1 && tcfv != 2), "unsupported timecode format\n" );
#define NO_TIMECODE_LINE (buff[0] == '#' || buff[0] == '\n' || buff[0] == '\r')
    if( tcfv == 1 )
    {
        int64_t file_pos;
        double assume_fps, seq_fps;
        int start, end = -1;
        int prev_start = -1, prev_end = -1;

        h->assume_fps = 0;
        for( num = 2; fgets( buff, sizeof(buff), tcfile_in ) != NULL; num++ )
        {
            if( NO_TIMECODE_LINE )
                continue;
            char *p = buff;
            while( *p && isspace( (unsigned char)*p ) )
                p++;
            if( !strncasecmp( p, "assume", 6 ) )
            {
                p += 6;
                p = (char *)timecode_skip_space( p );
            }
            else
                p = buff;
            FAIL_IF_ERROR( timecode_parse_double_line( p, &h->assume_fps ),
                           "tcfile parsing error: assumed fps not found\n" );
            break;
        }
        FAIL_IF_ERROR( !timecode_is_positive_finite( h->assume_fps ), "invalid assumed fps %.6f\n", h->assume_fps );

        file_pos = ftell( tcfile_in );
        FAIL_IF_ERROR( file_pos < 0, "tcfile seek failed\n" );
        h->stored_pts_num = 0;
        for( seq_num = 0; fgets( buff, sizeof(buff), tcfile_in ) != NULL; num++ )
        {
            if( NO_TIMECODE_LINE )
            {
                ret = timecode_parse_tdecimate_last_frame( buff, &end );
                FAIL_IF_ERROR( ret < 0, "invalid tcfile frame count\n" );
                if( !ret )
                {
                    FAIL_IF_ERROR( end < 0 || end == INT_MAX, "invalid tcfile frame count\n" );
                    h->stored_pts_num = end + 1;
                }
                continue;
            }
            ret = timecode_parse_v1_range( buff, &start, &end, &seq_fps ) ? 0 : 3;
            if( ret == EOF )
                continue;
            FAIL_IF_ERROR( ret != 3, "invalid input tcfile\n" );
            FAIL_IF_ERROR( start > end || start <= prev_start || end <= prev_end || !timecode_is_positive_finite( seq_fps ),
                           "invalid input tcfile at line %d: %s\n", num, buff );
            prev_start = start;
            prev_end = end;
            if( h->auto_timebase_den || h->auto_timebase_num )
                ++seq_num;
        }
        if( !h->stored_pts_num )
        {
            FAIL_IF_ERROR( end > INT_MAX - 2, "invalid tcfile frame count\n" );
            h->stored_pts_num = end + 2;
        }
        timecodes_num = h->stored_pts_num;
        FAIL_IF_ERROR( timecodes_num <= 0 || (uint64_t)timecodes_num > SIZE_MAX / sizeof(double), "too many timecodes\n" );
        FAIL_IF_ERROR( timecode_seek_to_pos( tcfile_in, file_pos ), "tcfile seek failed\n" );

        timecodes = malloc( (size_t)timecodes_num * sizeof(double) );
        if( !timecodes )
            return -1;
        if( h->auto_timebase_den || h->auto_timebase_num )
        {
            FAIL_IF_ERROR( seq_num == INT_MAX || (uint64_t)(seq_num + 1) > SIZE_MAX / sizeof(double), "too many tcfile fps entries\n" );
            fpss = malloc( ((size_t)seq_num + 1) * sizeof(double) );
            if( !fpss )
                goto fail;
        }

        assume_fps = correct_fps( h->assume_fps, h );
        if( assume_fps < 0 )
            goto fail;
        timecodes[0] = 0;
        for( num = seq_num = 0; num < timecodes_num - 1 && fgets( buff, sizeof(buff), tcfile_in ) != NULL; )
        {
            if( NO_TIMECODE_LINE )
                continue;
            ret = timecode_parse_v1_range( buff, &start, &end, &seq_fps ) ? 0 : 3;
            if( ret == EOF )
                continue;
            if( ret != 3 )
            {
                start = end = timecodes_num - 1;
                seq_fps = assume_fps;
            }
            for( ; num < start && num < timecodes_num - 1; num++ )
                timecodes[num + 1] = timecodes[num] + 1.0 / assume_fps;
            if( num < timecodes_num - 1 )
            {
                if( h->auto_timebase_den || h->auto_timebase_num )
                    fpss[seq_num++] = seq_fps;
                seq_fps = correct_fps( seq_fps, h );
                if( seq_fps < 0 )
                    goto fail;
                for( num = start; num <= end && num < timecodes_num - 1; num++ )
                    timecodes[num + 1] = timecodes[num] + 1.0 / seq_fps;
            }
        }
        for( ; num < timecodes_num - 1; num++ )
            timecodes[num + 1] = timecodes[num] + 1.0 / assume_fps;
        if( h->auto_timebase_den || h->auto_timebase_num )
            fpss[seq_num] = h->assume_fps;

        if( h->auto_timebase_num && !h->auto_timebase_den )
        {
            double exponent;
            double assume_fps_sig, seq_fps_sig;
            if( try_mkv_timebase_den( fpss, h, seq_num + 1 ) < 0 )
                goto fail;
            if( timecode_seek_to_pos( tcfile_in, file_pos ) )
                goto fail;
            assume_fps_sig = sigexp10( h->assume_fps, &exponent );
            uint64_t assume_fps_den;
            if( timecode_mkv_fps_den( &assume_fps_den, assume_fps_sig, exponent ) )
                goto fail;
            assume_fps = (double)MKV_TIMEBASE_DEN / (double)assume_fps_den;
            for( num = 0; num < timecodes_num - 1 && fgets( buff, sizeof(buff), tcfile_in ) != NULL; )
            {
                if( NO_TIMECODE_LINE )
                    continue;
                ret = timecode_parse_v1_range( buff, &start, &end, &seq_fps ) ? 0 : 3;
                if( ret == EOF )
                    continue;
                if( ret != 3 )
                {
                    start = end = timecodes_num - 1;
                    seq_fps = assume_fps;
                }
                seq_fps_sig = sigexp10( seq_fps, &exponent );
                uint64_t seq_fps_den;
                if( timecode_mkv_fps_den( &seq_fps_den, seq_fps_sig, exponent ) )
                    goto fail;
                seq_fps = (double)MKV_TIMEBASE_DEN / (double)seq_fps_den;
                for( ; num < start && num < timecodes_num - 1; num++ )
                    timecodes[num + 1] = timecodes[num] + 1.0 / assume_fps;
                for( num = start; num <= end && num < timecodes_num - 1; num++ )
                    timecodes[num + 1] = timecodes[num] + 1.0 / seq_fps;
            }
            for( ; num < timecodes_num - 1; num++ )
                timecodes[num + 1] = timecodes[num] + 1.0 / assume_fps;
        }
        if( fpss )
        {
            free( fpss );
            fpss = NULL;
        }

        h->assume_fps = assume_fps;
        h->last_timecode = timecodes[timecodes_num - 1];
    }
    else    /* tcfv == 2 */
    {
        int64_t file_pos = ftell( tcfile_in );
        FAIL_IF_ERROR( file_pos < 0, "tcfile seek failed\n" );

        h->stored_pts_num = 0;
        while( fgets( buff, sizeof(buff), tcfile_in ) != NULL )
        {
            if( NO_TIMECODE_LINE )
            {
                if( !h->stored_pts_num )
                    file_pos = ftell( tcfile_in );
                continue;
            }
            FAIL_IF_ERROR( h->stored_pts_num == INT_MAX, "too many timecodes\n" );
            h->stored_pts_num++;
        }
        timecodes_num = h->stored_pts_num;
        FAIL_IF_ERROR( !timecodes_num, "input tcfile doesn't have any timecodes!\n" );
        FAIL_IF_ERROR( (uint64_t)timecodes_num > SIZE_MAX / sizeof(double), "too many timecodes\n" );
        FAIL_IF_ERROR( timecode_seek_to_pos( tcfile_in, file_pos ), "tcfile seek failed\n" );

        timecodes = malloc( (size_t)timecodes_num * sizeof(double) );
        if( !timecodes )
            return -1;

        num = 0;
        if( fgets( buff, sizeof(buff), tcfile_in ) != NULL )
        {
            FAIL_IF_ERROR( timecode_parse_double_line( buff, &timecodes[0] ),
                           "invalid input tcfile for frame 0\n" );
            FAIL_IF_ERROR( !timecode_is_positive_finite( timecodes[0] ) && timecodes[0] != 0.0,
                           "invalid input tcfile for frame 0\n" );
            timecodes[0] *= 1e-3;         /* Timecode format v2 is expressed in milliseconds. */
            for( num = 1; num < timecodes_num && fgets( buff, sizeof(buff), tcfile_in ) != NULL; )
            {
                if( NO_TIMECODE_LINE )
                    continue;
                FAIL_IF_ERROR( timecode_parse_double_line( buff, &timecodes[num] ) ||
                               !timecode_is_positive_finite( timecodes[num] ),
                               "invalid input tcfile for frame %d\n", num );
                timecodes[num] *= 1e-3;         /* Timecode format v2 is expressed in milliseconds. */
                FAIL_IF_ERROR( timecodes[num] <= timecodes[num - 1],
                               "invalid input tcfile for frame %d\n", num );
                ++num;
            }
        }
        FAIL_IF_ERROR( num < timecodes_num, "failed to read input tcfile for frame %d", num );

        if( timecodes_num == 1 )
            h->timebase_den = info->fps_num;
        else if( h->auto_timebase_den )
        {
            FAIL_IF_ERROR( timecodes_num <= 1, "too few timecodes for automatic timebase generation\n" );
            size_t fpss_num = (size_t)timecodes_num - 1;
            FAIL_IF_ERROR( fpss_num > SIZE_MAX / sizeof(double), "too many tcfile fps entries\n" );
            fpss = malloc( fpss_num * sizeof(double) );
            if( !fpss )
                goto fail;
            for( size_t fpss_idx = 0; fpss_idx < fpss_num; fpss_idx++ )
            {
                fpss[fpss_idx] = 1.0 / (timecodes[fpss_idx + 1] - timecodes[fpss_idx]);
                if( h->auto_timebase_den )
                {
                    uint64_t i = 1;
                    uint64_t fps_num, fps_den;
                    double exponent;
                    double fps_sig = sigexp10( fpss[fpss_idx], &exponent );
                    while( 1 )
                    {
                        FAIL_IF_ERROR( !h->timebase_num || i > UINT64_MAX / h->timebase_num,
                                       "automatic timebase generation failed.\n"
                                       "                  Specify an appropriate timebase manually.\n" );
                        fps_den = i * h->timebase_num;
                        if( timecode_fps_num_from_sig( &fps_num, fps_den, fps_sig, exponent ) )
                        {
                            h->auto_timebase_den = 0;
                            break;
                        }
                        if( fabs( ((double)fps_num / (double)fps_den) / exponent - fps_sig ) < DOUBLE_EPSILON )
                            break;
                        ++i;
                    }
                    if( !h->auto_timebase_den )
                        continue;
                    timecode_update_auto_den( h, fps_num );
                    if( !h->auto_timebase_den )
                        continue;
                }
            }
            if( h->auto_timebase_num && !h->auto_timebase_den )
            {
                FAIL_IF_ERROR( fpss_num > INT_MAX, "too many tcfile fps entries\n" );
                if( try_mkv_timebase_den( fpss, h, (int)fpss_num ) < 0 )
                    goto fail;
            }
            free( fpss );
            fpss = NULL;
        }

        if( timecodes_num > 1 )
            h->assume_fps = 1.0 / (timecodes[timecodes_num - 1] - timecodes[timecodes_num - 2]);
        else
            h->assume_fps = (double)info->fps_num / info->fps_den;
        h->last_timecode = timecodes[timecodes_num - 1];
    }
#undef NO_TIMECODE_LINE
    if( h->auto_timebase_den || h->auto_timebase_num )
    {
        FAIL_IF_ERROR( !h->timebase_num || !h->timebase_den, "automatic timebase generation failed.\n"
                       "                  Specify an appropriate timebase manually.\n" );
        uint64_t i = gcd( h->timebase_num, h->timebase_den );
        h->timebase_num /= i;
        h->timebase_den /= i;
        x264_cli_log( "timecode", X264_LOG_INFO, "automatic timebase generation %"PRIu64"/%"PRIu64"\n", h->timebase_num, h->timebase_den );
    }
    FAIL_IF_ERROR( !h->timebase_num || !h->timebase_den ||
                   h->timebase_num > UINT32_MAX || h->timebase_den > UINT32_MAX,
                   "automatic timebase generation failed.\n"
                   "                  Specify an appropriate timebase manually.\n" );

    FAIL_IF_ERROR( (uint64_t)h->stored_pts_num > SIZE_MAX / sizeof(int64_t), "too many timecodes\n" );
    h->pts = malloc( (size_t)h->stored_pts_num * sizeof(int64_t) );
    if( !h->pts )
        goto fail;
    for( num = 0; num < h->stored_pts_num; num++ )
    {
        if( timecode_seconds_to_pts( &h->pts[num], timecodes[num], h ) )
            goto fail;
        FAIL_IF_ERROR( num > 0 && h->pts[num] <= h->pts[num - 1], "invalid timebase or timecode for frame %d\n", num );
    }

    free( timecodes );
    return 0;

fail:
    if( timecodes )
        free( timecodes );
    if( fpss )
        free( fpss );
    return -1;
}

#undef DOUBLE_EPSILON
#undef MKV_TIMEBASE_DEN

static int open_file( char *psz_filename, hnd_t *p_handle, video_info_t *info, cli_input_opt_t *opt )
{
    int ret = 0;
    FILE *tcfile_in = NULL;
    timecode_hnd_t *h = NULL;

    if( !psz_filename || !p_handle || !*p_handle || !info || !opt )
        return -1;

    h = calloc( 1, sizeof(timecode_hnd_t) );
    if( !h )
    {
        x264_cli_log( "timecode", X264_LOG_ERROR, "malloc failed\n" );
        return -1;
    }
    h->input = cli_input;
    h->p_handle = *p_handle;
    if( opt->timebase )
    {
        int has_den = 0;
        if( timecode_parse_timebase( opt->timebase, &h->timebase_num, &h->timebase_den, &has_den ) )
        {
            x264_cli_log( "timecode", X264_LOG_ERROR, "invalid argument: timebase = %s\n", opt->timebase );
            goto fail;
        }
        ret = has_den ? 2 : 1;
        if( !has_den )
            h->timebase_den = 0; /* set later by auto timebase generation */
    }
    h->auto_timebase_num = !ret;
    h->auto_timebase_den = ret < 2;
    if( h->auto_timebase_num )
    {
        if( !info->fps_den )
        {
            x264_cli_log( "timecode", X264_LOG_ERROR, "invalid input fps\n" );
            goto fail;
        }
        h->timebase_num = info->fps_den; /* can be changed later by auto timebase generation */
    }
    if( h->auto_timebase_den )
        h->timebase_den = 0;             /* set later by auto timebase generation */

    tcfile_in = x264_fopen( psz_filename, "rb" );
    if( !tcfile_in )
    {
        x264_cli_log( "timecode", X264_LOG_ERROR, "can't open `%s'\n", psz_filename );
        goto fail;
    }
    if( !x264_is_regular_file( tcfile_in ) )
    {
        x264_cli_log( "timecode", X264_LOG_ERROR, "tcfile input incompatible with non-regular file `%s'\n", psz_filename );
        goto fail;
    }

    if( parse_tcfile( tcfile_in, h, info ) < 0 )
        goto fail;
    if( fclose( tcfile_in ) )
    {
        tcfile_in = NULL;
        x264_cli_log( "timecode", X264_LOG_ERROR, "failed to close timecode file `%s'\n", psz_filename );
        goto fail;
    }
    tcfile_in = NULL;

    info->timebase_num = (uint32_t)h->timebase_num;
    info->timebase_den = (uint32_t)h->timebase_den;
    info->vfr = 1;

    *p_handle = h;
    return 0;

fail:
    if( tcfile_in )
    {
        if( fclose( tcfile_in ) )
            x264_cli_log( "timecode", X264_LOG_ERROR, "failed to close timecode file `%s'\n", psz_filename );
    }
    if( h )
    {
        free( h->pts );
        free( h );
    }
    return -1;
}

static int get_frame_pts( timecode_hnd_t *h, int frame, int real_frame, double *last_timecode, int *drop_pts, int64_t *pts )
{
    if( frame < 0 )
        return -1;
    if( h->pts && frame < h->stored_pts_num )
    {
        *pts = h->pts[frame];
        return 0;
    }
    else
    {
        if( h->pts && real_frame )
            *drop_pts = 1;
        if( !timecode_is_positive_finite( h->assume_fps ) ||
            (*last_timecode != 0.0 && !timecode_is_positive_finite( *last_timecode )) )
            return -1;
        double timecode = *last_timecode + 1.0 / h->assume_fps;
        if( !timecode_is_positive_finite( timecode ) )
            return -1;
        if( real_frame )
            *last_timecode = timecode;
        return timecode_seconds_to_pts( pts, timecode, h );
    }
}

static int read_frame( cli_pic_t *pic, hnd_t handle, int frame )
{
    timecode_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle || !h->input.read_frame ||
        frame < 0 || frame == INT_MAX )
        return -1;
    if( h->input.read_frame( pic, h->p_handle, frame ) )
        return -1;

    int64_t pts;
    int64_t next_pts;
    double last_timecode = h->last_timecode;
    int drop_pts = 0;
    if( get_frame_pts( h, frame, 1, &last_timecode, &drop_pts, &pts ) ||
        get_frame_pts( h, frame + 1, 0, &last_timecode, &drop_pts, &next_pts ) ||
        next_pts <= pts )
    {
        if( h->input.release_frame )
            h->input.release_frame( pic, h->p_handle );
        return -1;
    }
    if( drop_pts )
    {
        x264_cli_log( "timecode", X264_LOG_INFO, "input timecode file missing data for frame %d and later\n"
                      "                 assuming constant fps %.6f\n", frame, h->assume_fps );
        free( h->pts );
        h->pts = NULL;
    }
    h->last_timecode = last_timecode;
    pic->pts = pts;
    pic->duration = next_pts - pts;

    return 0;
}

static int release_frame( cli_pic_t *pic, hnd_t handle )
{
    timecode_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle )
        return -1;
    if( h->input.release_frame )
        return h->input.release_frame( pic, h->p_handle );
    return 0;
}

static int picture_alloc( cli_pic_t *pic, hnd_t handle, int csp, int width, int height )
{
    timecode_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle || !h->input.picture_alloc )
        return -1;
    return h->input.picture_alloc( pic, h->p_handle, csp, width, height );
}

static void picture_clean( cli_pic_t *pic, hnd_t handle )
{
    timecode_hnd_t *h = handle;
    if( !pic || !h || !h->p_handle || !h->input.picture_clean )
        return;
    h->input.picture_clean( pic, h->p_handle );
}

static int close_file( hnd_t handle )
{
    timecode_hnd_t *h = handle;
    int ret = 0;
    if( !h )
        return 0;
    if( h->pts )
        free( h->pts );
    if( h->p_handle && h->input.close_file )
        ret = h->input.close_file( h->p_handle );
    free( h );
    return ret;
}

const cli_input_t timecode_input = { open_file, picture_alloc, read_frame, release_frame, picture_clean, close_file };
