/*****************************************************************************
 * set.c: quantization init
 *****************************************************************************
 * Copyright (C) 2005-2025 x264 project
 *
 * Authors: Loren Merritt <lorenm@u.washington.edu>
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

#include "common.h"

#include <ctype.h>
#include <errno.h>

#define SHIFT(x,s) ((s)<=0 ? (x)<<-(s) : ((x)+(1<<((s)-1)))>>(s))
#define DIV(n,d) (((n) + ((d)>>1)) / (d))
#define X264_QUANT_SCALE_MODES 6
#define X264_QUANT4_SCALE_CLASSES 3
#define X264_QUANT8_SCAN_SIZE 16
#define X264_QUANT8_SCALE_CLASSES 6

static const uint8_t dequant4_scale[][X264_QUANT4_SCALE_CLASSES] =
{
    { 10, 13, 16 },
    { 11, 14, 18 },
    { 13, 16, 20 },
    { 14, 18, 23 },
    { 16, 20, 25 },
    { 18, 23, 29 }
};
X264_STATIC_ASSERT( ARRAY_ELEMS(dequant4_scale) == X264_QUANT_SCALE_MODES, "4x4 dequant scale table size must match QP mod-6 domain" );
X264_STATIC_ASSERT( ARRAY_ELEMS(dequant4_scale[0]) == X264_QUANT4_SCALE_CLASSES, "4x4 dequant scale table width must match coefficient classes" );
static const uint16_t quant4_scale[][X264_QUANT4_SCALE_CLASSES] =
{
    { 13107, 8066, 5243 },
    { 11916, 7490, 4660 },
    { 10082, 6554, 4194 },
    {  9362, 5825, 3647 },
    {  8192, 5243, 3355 },
    {  7282, 4559, 2893 },
};
X264_STATIC_ASSERT( ARRAY_ELEMS(quant4_scale) == X264_QUANT_SCALE_MODES, "4x4 quant scale table size must match QP mod-6 domain" );
X264_STATIC_ASSERT( ARRAY_ELEMS(quant4_scale[0]) == X264_QUANT4_SCALE_CLASSES, "4x4 quant scale table width must match coefficient classes" );

static const uint8_t quant8_scan[] =
{
    0,3,4,3, 3,1,5,1, 4,5,2,5, 3,1,5,1
};
X264_STATIC_ASSERT( ARRAY_ELEMS(quant8_scan) == X264_QUANT8_SCAN_SIZE, "8x8 quant scan table size must match scan input domain" );
static const uint8_t dequant8_scale[][X264_QUANT8_SCALE_CLASSES] =
{
    { 20, 18, 32, 19, 25, 24 },
    { 22, 19, 35, 21, 28, 26 },
    { 26, 23, 42, 24, 33, 31 },
    { 28, 25, 45, 26, 35, 33 },
    { 32, 28, 51, 30, 40, 38 },
    { 36, 32, 58, 34, 46, 43 },
};
X264_STATIC_ASSERT( ARRAY_ELEMS(dequant8_scale) == X264_QUANT_SCALE_MODES, "8x8 dequant scale table size must match QP mod-6 domain" );
X264_STATIC_ASSERT( ARRAY_ELEMS(dequant8_scale[0]) == X264_QUANT8_SCALE_CLASSES, "8x8 dequant scale table width must match coefficient classes" );
static const uint16_t quant8_scale[][X264_QUANT8_SCALE_CLASSES] =
{
    { 13107, 11428, 20972, 12222, 16777, 15481 },
    { 11916, 10826, 19174, 11058, 14980, 14290 },
    { 10082,  8943, 15978,  9675, 12710, 11985 },
    {  9362,  8228, 14913,  8931, 11984, 11259 },
    {  8192,  7346, 13159,  7740, 10486,  9777 },
    {  7282,  6428, 11570,  6830,  9118,  8640 }
};
X264_STATIC_ASSERT( ARRAY_ELEMS(quant8_scale) == X264_QUANT_SCALE_MODES, "8x8 quant scale table size must match QP mod-6 domain" );
X264_STATIC_ASSERT( ARRAY_ELEMS(quant8_scale[0]) == X264_QUANT8_SCALE_CLASSES, "8x8 quant scale table width must match coefficient classes" );

int x264_cqm_init( x264_t *h )
{
    int def_quant4[6][16];
    int def_quant8[6][64];
    int def_dequant4[6][16];
    int def_dequant8[6][64];
    int quant4_mf[4][6][16];
    int quant8_mf[4][6][64];
    int deadzone[4] = { 32 - h->param.analyse.i_luma_deadzone[1],
                        32 - h->param.analyse.i_luma_deadzone[0],
                        32 - 11, 32 - 21 };
    int max_qp_err = -1;
    int max_chroma_qp_err = -1;
    int min_qp_err = QP_MAX+1;
    int num_8x8_lists = h->sps->i_chroma_format_idc == CHROMA_444 ? 4
                      : h->param.analyse.b_transform_8x8 ? 2 : 0; /* Checkasm may segfault if optimized out by --chroma-format */

#define CQM_ALLOC( w, count )\
    for( int i = 0; i < count; i++ )\
    {\
        int size = w*w;\
        int start = w == 8 ? 4 : 0;\
        int j;\
        for( j = 0; j < i; j++ )\
            if( !memcmp( h->sps->scaling_list[i+start], h->sps->scaling_list[j+start], size*sizeof(uint8_t) ) )\
                break;\
        if( j < i )\
        {\
            h->  quant##w##_mf[i] = h->  quant##w##_mf[j];\
            h->dequant##w##_mf[i] = h->dequant##w##_mf[j];\
            h->unquant##w##_mf[i] = h->unquant##w##_mf[j];\
        }\
        else\
        {\
            CHECKED_MALLOC( h->  quant##w##_mf[i], (int64_t)(QP_MAX_SPEC+1)*size*sizeof(udctcoef) );\
            CHECKED_MALLOC( h->dequant##w##_mf[i],  (int64_t)6*size*sizeof(int) );\
            CHECKED_MALLOC( h->unquant##w##_mf[i], (int64_t)(QP_MAX_SPEC+1)*size*sizeof(int) );\
        }\
        for( j = 0; j < i; j++ )\
            if( deadzone[j] == deadzone[i] &&\
                !memcmp( h->sps->scaling_list[i+start], h->sps->scaling_list[j+start], size*sizeof(uint8_t) ) )\
                break;\
        if( j < i )\
        {\
            h->quant##w##_bias[i] = h->quant##w##_bias[j];\
            h->quant##w##_bias0[i] = h->quant##w##_bias0[j];\
        }\
        else\
        {\
            CHECKED_MALLOC( h->quant##w##_bias[i], (int64_t)(QP_MAX_SPEC+1)*size*sizeof(udctcoef) );\
            CHECKED_MALLOC( h->quant##w##_bias0[i], (int64_t)(QP_MAX_SPEC+1)*size*sizeof(udctcoef) );\
        }\
    }

    CQM_ALLOC( 4, 4 )
    CQM_ALLOC( 8, num_8x8_lists )

    for( int q = 0; q < 6; q++ )
    {
        for( int i = 0; i < 16; i++ )
        {
            int j = (i&1) + ((i>>2)&1);
            def_dequant4[q][i] = dequant4_scale[q][j];
            def_quant4[q][i]   =   quant4_scale[q][j];
        }
        for( int i = 0; i < 64; i++ )
        {
            int j = quant8_scan[((i>>1)&12) | (i&3)];
            def_dequant8[q][i] = dequant8_scale[q][j];
            def_quant8[q][i]   =   quant8_scale[q][j];
        }
    }

    for( int q = 0; q < 6; q++ )
    {
        for( int i_list = 0; i_list < 4; i_list++ )
            for( int i = 0; i < 16; i++ )
            {
                h->dequant4_mf[i_list][q][i] = def_dequant4[q][i] * h->sps->scaling_list[i_list][i];
                     quant4_mf[i_list][q][i] = DIV(def_quant4[q][i] * 16, h->sps->scaling_list[i_list][i]);
            }
        for( int i_list = 0; i_list < num_8x8_lists; i_list++ )
            for( int i = 0; i < 64; i++ )
            {
                h->dequant8_mf[i_list][q][i] = def_dequant8[q][i] * h->sps->scaling_list[4+i_list][i];
                     quant8_mf[i_list][q][i] = DIV(def_quant8[q][i] * 16, h->sps->scaling_list[4+i_list][i]);
            }
    }

#define MAX_MF X264_MIN( 0xffff, (1 << (25 - BIT_DEPTH)) - 1 )

    for( int q = 0; q <= QP_MAX_SPEC; q++ )
    {
        int j;
        for( int i_list = 0; i_list < 4; i_list++ )
            for( int i = 0; i < 16; i++ )
            {
                h->unquant4_mf[i_list][q][i] = (1ULL << (q/6 + 15 + 8)) / quant4_mf[i_list][q%6][i];
                j = SHIFT(quant4_mf[i_list][q%6][i], q/6 - 1);
                h->quant4_mf[i_list][q][i] = (uint16_t)j;
                if( !j )
                {
                    min_qp_err = X264_MIN( min_qp_err, q );
                    continue;
                }
                // round to nearest, unless that would cause the deadzone to be negative
                h->quant4_bias[i_list][q][i] = X264_MIN( DIV(deadzone[i_list]<<10, j), (1<<15)/j );
                h->quant4_bias0[i_list][q][i] = (1<<15)/j;
                if( j > MAX_MF && q > max_qp_err && (i_list == CQM_4IY || i_list == CQM_4PY) )
                    max_qp_err = q;
                if( j > MAX_MF && q > max_chroma_qp_err && (i_list == CQM_4IC || i_list == CQM_4PC) )
                    max_chroma_qp_err = q;
            }
        if( h->param.analyse.b_transform_8x8 )
            for( int i_list = 0; i_list < num_8x8_lists; i_list++ )
                for( int i = 0; i < 64; i++ )
                {
                    h->unquant8_mf[i_list][q][i] = (1ULL << (q/6 + 16 + 8)) / quant8_mf[i_list][q%6][i];
                    j = SHIFT(quant8_mf[i_list][q%6][i], q/6);
                    h->quant8_mf[i_list][q][i] = (uint16_t)j;

                    if( !j )
                    {
                        min_qp_err = X264_MIN( min_qp_err, q );
                        continue;
                    }
                    h->quant8_bias[i_list][q][i] = X264_MIN( DIV(deadzone[i_list]<<10, j), (1<<15)/j );
                    h->quant8_bias0[i_list][q][i] = (1<<15)/j;
                    if( j > MAX_MF && q > max_qp_err && (i_list == CQM_8IY || i_list == CQM_8PY) )
                        max_qp_err = q;
                    if( j > MAX_MF && q > max_chroma_qp_err && (i_list == CQM_8IC || i_list == CQM_8PC) )
                        max_chroma_qp_err = q;
                }
    }

    /* Emergency mode denoising. */
    x264_emms();
    CHECKED_MALLOC( h->nr_offset_emergency, (int64_t)sizeof(*h->nr_offset_emergency)*(QP_MAX-QP_MAX_SPEC) );
    for( int q = 0; q < QP_MAX - QP_MAX_SPEC; q++ )
        for( int cat = 0; cat < 3 + CHROMA444; cat++ )
        {
            int dct8x8 = cat&1;
            if( !h->param.analyse.b_transform_8x8 && dct8x8 )
                continue;

            int size = dct8x8 ? 64 : 16;
            udctcoef *nr_offset = h->nr_offset_emergency[q][cat];
            /* Denoise chroma first (due to h264's chroma QP offset), then luma, then DC. */
            int dc_threshold =    (QP_MAX-QP_MAX_SPEC)*2/3;
            int luma_threshold =  (QP_MAX-QP_MAX_SPEC)*2/3;
            int chroma_threshold = 0;

            for( int i = 0; i < size; i++ )
            {
                int max = (1 << (7 + BIT_DEPTH)) - 1;
                /* True "emergency mode": remove all DCT coefficients */
                if( q == QP_MAX - QP_MAX_SPEC - 1 )
                {
                    nr_offset[i] = max;
                    continue;
                }

                int thresh = i == 0 ? dc_threshold : cat >= 2 ? chroma_threshold : luma_threshold;
                if( q < thresh )
                {
                    nr_offset[i] = 0;
                    continue;
                }
                double pos = (double)(q-thresh+1) / (QP_MAX - QP_MAX_SPEC - thresh);

                /* XXX: this math is largely tuned for /dev/random input. */
                double start = dct8x8 ? h->unquant8_mf[CQM_8PY][QP_MAX_SPEC][i]
                                      : h->unquant4_mf[CQM_4PY][QP_MAX_SPEC][i];
                /* Formula chosen as an exponential scale to vaguely mimic the effects
                 * of a higher quantizer. */
                double bias = (pow( 2, pos*(QP_MAX - QP_MAX_SPEC)/10. )*0.003-0.003) * start;
                nr_offset[i] = X264_MIN( bias + 0.5, max );
            }
        }

    if( !h->mb.b_lossless )
    {
        for( int i = 0; i < 3; i++ )
        {
            while( h->chroma_qp_table[SPEC_QP(h->param.rc.i_qp_min[i])] <= max_chroma_qp_err )
                h->param.rc.i_qp_min[i]++;
            if( min_qp_err <= h->param.rc.i_qp_max[i] )
                h->param.rc.i_qp_max[i] = min_qp_err-1;
            if( max_qp_err >= h->param.rc.i_qp_min[i] )
                h->param.rc.i_qp_min[i] = max_qp_err+1;
            /* If long level-codes aren't allowed, we need to allow QP high enough to avoid them. */
            if( !h->param.b_cabac && h->sps->i_profile_idc < PROFILE_HIGH )
                while( h->chroma_qp_table[SPEC_QP(h->param.rc.i_qp_max[i])] <= 12 || h->param.rc.i_qp_max[i] <= 12 )
                    h->param.rc.i_qp_max[i]++;
            if( h->param.rc.i_qp_min[i] > h->param.rc.i_qp_max[i] )
            {
                x264_log( h, X264_LOG_ERROR, "Impossible QP constraints for CQM (min=%d, max=%d)\n",
                          h->param.rc.i_qp_min[i], h->param.rc.i_qp_max[i] );
                return -1;
            }
        }
        h->param.rc.i_qp_min_min = X264_MIN3( h->param.rc.i_qp_min[SLICE_TYPE_I],
                                              h->param.rc.i_qp_min[SLICE_TYPE_P],
                                              h->param.rc.i_qp_min[SLICE_TYPE_B] );
        h->param.rc.i_qp_max_max = X264_MAX3( h->param.rc.i_qp_max[SLICE_TYPE_I],
                                              h->param.rc.i_qp_max[SLICE_TYPE_P],
                                              h->param.rc.i_qp_max[SLICE_TYPE_B] );
    }
    return 0;
fail:
    x264_cqm_delete( h );
    return -1;
}

#define CQM_DELETE( n, max )\
    for( int i = 0; i < (max); i++ )\
    {\
        int j;\
        for( j = 0; j < i; j++ )\
            if( h->quant##n##_mf[i] == h->quant##n##_mf[j] )\
                break;\
        if( j == i )\
        {\
            x264_free( h->  quant##n##_mf[i] );\
            x264_free( h->dequant##n##_mf[i] );\
            x264_free( h->unquant##n##_mf[i] );\
        }\
        for( j = 0; j < i; j++ )\
            if( h->quant##n##_bias[i] == h->quant##n##_bias[j] )\
                break;\
        if( j == i )\
        {\
            x264_free( h->quant##n##_bias[i] );\
            x264_free( h->quant##n##_bias0[i] );\
        }\
    }

void x264_cqm_delete( x264_t *h )
{
    CQM_DELETE( 4, 4 );
    CQM_DELETE( 8, CHROMA444 ? 4 : 2 );
    x264_free( h->nr_offset_emergency );
}

static int cqm_is_delimiter( int c )
{
    return c == ',' || isspace( (unsigned char)c );
}

static int cqm_is_name_char( int c )
{
    return isalnum( (unsigned char)c ) || c == '_';
}

static const char *cqm_find_jmlist( const char *buf, const char *name )
{
    size_t len = strlen( name );
    const char *p = buf;

    while( (p = strstr( p, name )) )
    {
        const char *end = p + len;
        if( (p == buf || !cqm_is_name_char( p[-1] )) &&
            ((*end == 'U' || *end == 'V') ? !cqm_is_name_char( end[1] ) : !cqm_is_name_char( *end )) )
            return p;
        p++;
    }

    return NULL;
}

static const char *cqm_find_next_jmlist( const char *buf )
{
    static const char * const names[] =
    {
        "INTRA4X4_LUMA",
        "INTER4X4_LUMA",
        "INTRA4X4_CHROMA",
        "INTER4X4_CHROMA",
        "INTRA8X8_LUMA",
        "INTER8X8_LUMA",
        "INTRA8X8_CHROMA",
        "INTER8X8_CHROMA",
        NULL
    };
    const char *next = NULL;

    for( int i = 0; names[i]; i++ )
    {
        const char *p = cqm_find_jmlist( buf, names[i] );
        if( p && (!next || p < next) )
            next = p;
    }

    return next;
}

static const char *cqm_next_value_start( const char *p, int first )
{
    if( !*p )
        return NULL;
    if( first )
        while( cqm_is_delimiter( *p ) || *p == '=' )
            p++;
    else
    {
        if( !cqm_is_delimiter( *p ) )
            return p;
        while( cqm_is_delimiter( *p ) )
            p++;
    }

    return *p ? p : NULL;
}

static int cqm_parse_int( const char *p, char **end, int *dst )
{
    long value;

    if( !(*p >= '0' && *p <= '9') && *p != '-' )
        return -1;

    errno = 0;
    value = strtol( p, end, 10 );
    if( *end == p || errno == ERANGE || value < INT_MIN || value > INT_MAX )
        return -1;

    *dst = (int)value;
    return 0;
}

static int cqm_parse_jmlist( x264_t *h, const char *buf, const char *name,
                             uint8_t *cqm, const uint8_t *jvt, int length )
{
    int i;
    uint8_t parsed[X264_CQM_8_SIZE];

    if( length <= 0 || length > (int)sizeof(parsed) )
        return -1;

    const char *p = cqm_find_jmlist( buf, name );
    if( !p )
    {
        memset( parsed, 16, length );
        memcpy( cqm, parsed, length );
        return 0;
    }

    p += strlen( name );
    if( *p == 'U' || *p == 'V' )
        p++;

    const char *nextvar = cqm_find_next_jmlist( p );

    for( i = 0; i < length; i++ )
    {
        char *end;
        int coef = -1;
        p = cqm_next_value_start( p, i == 0 );
        if( !p || (nextvar && p >= nextvar) )
            break;
        if( cqm_parse_int( p, &end, &coef ) || (*end && !cqm_is_delimiter( *end )) )
        {
            x264_log( h, X264_LOG_ERROR, "bad coefficient in list '%s'\n", name );
            return -1;
        }
        if( i == 0 && coef == 0 )
        {
            memcpy( parsed, jvt, length );
            memcpy( cqm, parsed, length );
            return 0;
        }
        if( coef < 1 || coef > 255 )
        {
            x264_log( h, X264_LOG_ERROR, "bad coefficient in list '%s'\n", name );
            return -1;
        }
        parsed[i] = coef;
        p = end;
    }

    if( i != length || (nextvar && p && p > nextvar) )
    {
        x264_log( h, X264_LOG_ERROR, "not enough coefficients in list '%s'\n", name );
        return -1;
    }

    memcpy( cqm, parsed, length );
    return 0;
}

int x264_cqm_parse_file( x264_t *h, const char *filename )
{
    char *p;
    int b_error = 0;
    uint8_t cqm_4iy[X264_CQM_4_SIZE];
    uint8_t cqm_4py[X264_CQM_4_SIZE];
    uint8_t cqm_4ic[X264_CQM_4_SIZE];
    uint8_t cqm_4pc[X264_CQM_4_SIZE];
    uint8_t cqm_8iy[X264_CQM_8_SIZE];
    uint8_t cqm_8py[X264_CQM_8_SIZE];
    uint8_t cqm_8ic[X264_CQM_8_SIZE];
    uint8_t cqm_8pc[X264_CQM_8_SIZE];

    char *buf = x264_slurp_file( filename );
    if( !buf )
    {
        x264_log( h, X264_LOG_ERROR, "can't open file '%s'\n", filename );
        return -1;
    }

    while( (p = strchr( buf, '#' )) != NULL )
        memset( p, ' ', strcspn( p, "\n" ) );

    b_error |= cqm_parse_jmlist( h, buf, "INTRA4X4_LUMA",   cqm_4iy, x264_cqm_jvt4i, X264_CQM_4_SIZE );
    b_error |= cqm_parse_jmlist( h, buf, "INTER4X4_LUMA",   cqm_4py, x264_cqm_jvt4p, X264_CQM_4_SIZE );
    b_error |= cqm_parse_jmlist( h, buf, "INTRA4X4_CHROMA", cqm_4ic, x264_cqm_jvt4i, X264_CQM_4_SIZE );
    b_error |= cqm_parse_jmlist( h, buf, "INTER4X4_CHROMA", cqm_4pc, x264_cqm_jvt4p, X264_CQM_4_SIZE );
    b_error |= cqm_parse_jmlist( h, buf, "INTRA8X8_LUMA",   cqm_8iy, x264_cqm_jvt8i, X264_CQM_8_SIZE );
    b_error |= cqm_parse_jmlist( h, buf, "INTER8X8_LUMA",   cqm_8py, x264_cqm_jvt8p, X264_CQM_8_SIZE );
    if( CHROMA444 )
    {
        b_error |= cqm_parse_jmlist( h, buf, "INTRA8X8_CHROMA", cqm_8ic, x264_cqm_jvt8i, X264_CQM_8_SIZE );
        b_error |= cqm_parse_jmlist( h, buf, "INTER8X8_CHROMA", cqm_8pc, x264_cqm_jvt8p, X264_CQM_8_SIZE );
    }

    x264_free( buf );
    if( b_error )
        return b_error;

    h->param.i_cqm_preset = X264_CQM_CUSTOM;
    memcpy( h->param.cqm_4iy, cqm_4iy, sizeof(h->param.cqm_4iy) );
    memcpy( h->param.cqm_4py, cqm_4py, sizeof(h->param.cqm_4py) );
    memcpy( h->param.cqm_4ic, cqm_4ic, sizeof(h->param.cqm_4ic) );
    memcpy( h->param.cqm_4pc, cqm_4pc, sizeof(h->param.cqm_4pc) );
    memcpy( h->param.cqm_8iy, cqm_8iy, sizeof(h->param.cqm_8iy) );
    memcpy( h->param.cqm_8py, cqm_8py, sizeof(h->param.cqm_8py) );
    if( CHROMA444 )
    {
        memcpy( h->param.cqm_8ic, cqm_8ic, sizeof(h->param.cqm_8ic) );
        memcpy( h->param.cqm_8pc, cqm_8pc, sizeof(h->param.cqm_8pc) );
    }
    return 0;
}
