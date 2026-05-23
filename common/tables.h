/*****************************************************************************
 * tables.h: const tables
 *****************************************************************************
 * Copyright (C) 2003-2025 x264 project
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Loren Merritt <lorenm@u.washington.edu>
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

#ifndef X264_TABLES_H
#define X264_TABLES_H

typedef struct
{
    uint8_t i_bits;
    uint8_t i_size;
} vlc_t;

X264_API extern const x264_level_t x264_levels[];

#define X264_EXP2_LUT_SIZE 64
#define X264_LOG2_LUT_SIZE 128
#define X264_LOG2_LZ_LUT_SIZE 32
extern const uint8_t x264_exp2_lut[X264_EXP2_LUT_SIZE];
extern const float   x264_log2_lut[X264_LOG2_LUT_SIZE];
extern const float   x264_log2_lz_lut[X264_LOG2_LZ_LUT_SIZE];

#define QP_MAX_MAX (51+6*2+18)
#define X264_TRELLIS_LAMBDA2_MODELS 2
extern const uint16_t x264_lambda_tab[QP_MAX_MAX+1];
extern const int      x264_lambda2_tab[QP_MAX_MAX+1];
extern const int      x264_trellis_lambda2_tab[X264_TRELLIS_LAMBDA2_MODELS][QP_MAX_MAX+1];
#define MAX_CHROMA_LAMBDA_OFFSET 36
extern const uint16_t x264_chroma_lambda2_offset_tab[MAX_CHROMA_LAMBDA_OFFSET+1];

#define X264_HPEL_REF_COUNT 16
extern const uint8_t x264_hpel_ref0[X264_HPEL_REF_COUNT];
extern const uint8_t x264_hpel_ref1[X264_HPEL_REF_COUNT];

#define X264_CQM_4_SIZE 16
#define X264_CQM_8_SIZE 64
#define X264_CQM_JVT_COUNT 8
extern const uint8_t x264_cqm_jvt4i[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_jvt4p[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_jvt8i[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_jvt8p[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_flat16[X264_CQM_8_SIZE];
extern const uint8_t * const x264_cqm_jvt[X264_CQM_JVT_COUNT];
extern const uint8_t x264_cqm_avci50_4ic[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_avci50_p_8iy[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_avci50_1080i_8iy[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_avci100_720p_4ic[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_avci100_720p_8iy[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_avci100_1080_4ic[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_avci100_1080i_8iy[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_avci100_1080p_8iy[X264_CQM_8_SIZE];
extern const uint8_t x264_cqm_avci300_2160p_4iy[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_avci300_2160p_4ic[X264_CQM_4_SIZE];
extern const uint8_t x264_cqm_avci300_2160p_8iy[X264_CQM_8_SIZE];

#define X264_DECIMATE_4_SIZE 16
#define X264_DECIMATE_8_SIZE 64
extern const uint8_t x264_decimate_table4[X264_DECIMATE_4_SIZE];
extern const uint8_t x264_decimate_table8[X264_DECIMATE_8_SIZE];

#define X264_DCT4_WEIGHT_SIZE 16
#define X264_DCT8_WEIGHT_SIZE 64
extern const uint32_t x264_dct4_weight_tab[X264_DCT4_WEIGHT_SIZE];
extern const uint32_t x264_dct8_weight_tab[X264_DCT8_WEIGHT_SIZE];
extern const uint32_t x264_dct4_weight2_tab[X264_DCT4_WEIGHT_SIZE];
extern const uint32_t x264_dct8_weight2_tab[X264_DCT8_WEIGHT_SIZE];

#define X264_CABAC_CONTEXTS 1024
#define X264_CABAC_STATE_COUNT 128
#define X264_CABAC_LPS_RANGE_COUNT 64
#define X264_CABAC_LPS_RANGE_SIZE 4
#define X264_CABAC_INIT_PB_MODELS 3
#define X264_LAST_COEFF_FLAG_OFFSET_8X8_COUNT 63
#define X264_COEFF_FLAG_OFFSET_CHROMA_422_DC_COUNT 7
#define X264_COUNT_CAT_M1_COUNT 14

extern const int8_t   x264_cabac_context_init_I[X264_CABAC_CONTEXTS][2];
extern const int8_t   x264_cabac_context_init_PB[X264_CABAC_INIT_PB_MODELS][X264_CABAC_CONTEXTS][2];
extern const uint8_t  x264_cabac_range_lps[X264_CABAC_LPS_RANGE_COUNT][X264_CABAC_LPS_RANGE_SIZE];
extern const uint8_t  x264_cabac_transition[X264_CABAC_STATE_COUNT][2];
extern const uint8_t  x264_cabac_renorm_shift[X264_CABAC_LPS_RANGE_COUNT];
extern const uint16_t x264_cabac_entropy[X264_CABAC_STATE_COUNT];

#define X264_COEFF_FLAG_OFFSET_INTERLACED_MODES 2
#define X264_COEFF_FLAG_OFFSET_8X8_SIZE 64
#define X264_COEFF_FLAG_OFFSET_CAT_COUNT 16
extern const uint8_t  x264_significant_coeff_flag_offset_8x8[X264_COEFF_FLAG_OFFSET_INTERLACED_MODES][X264_COEFF_FLAG_OFFSET_8X8_SIZE];
extern const uint8_t  x264_last_coeff_flag_offset_8x8[X264_LAST_COEFF_FLAG_OFFSET_8X8_COUNT];
extern const uint8_t  x264_coeff_flag_offset_chroma_422_dc[X264_COEFF_FLAG_OFFSET_CHROMA_422_DC_COUNT];
extern const uint16_t x264_significant_coeff_flag_offset[X264_COEFF_FLAG_OFFSET_INTERLACED_MODES][X264_COEFF_FLAG_OFFSET_CAT_COUNT];
extern const uint16_t x264_last_coeff_flag_offset[X264_COEFF_FLAG_OFFSET_INTERLACED_MODES][X264_COEFF_FLAG_OFFSET_CAT_COUNT];
extern const uint16_t x264_coeff_abs_level_m1_offset[X264_COEFF_FLAG_OFFSET_CAT_COUNT];
extern const uint8_t  x264_count_cat_m1[X264_COUNT_CAT_M1_COUNT];

#define X264_COEFF0_TOKEN_COUNT 6
#define X264_COEFF_TOKEN_TABLES 6
#define X264_COEFF_TOKEN_TOTALS 16
#define X264_COEFF_TOKEN_TRAILING_COUNT 4
#define X264_TOTAL_ZEROS_COUNT 15
#define X264_TOTAL_ZEROS_SIZE 16
#define X264_TOTAL_ZEROS_2X2_DC_COUNT 3
#define X264_TOTAL_ZEROS_2X2_DC_SIZE 4
#define X264_TOTAL_ZEROS_2X4_DC_COUNT 7
#define X264_TOTAL_ZEROS_2X4_DC_SIZE 8
#define X264_RUN_BEFORE_COUNT 7
#define X264_RUN_BEFORE_SIZE 16
extern const vlc_t x264_coeff0_token[X264_COEFF0_TOKEN_COUNT];
extern const vlc_t x264_coeff_token[X264_COEFF_TOKEN_TABLES][X264_COEFF_TOKEN_TOTALS][X264_COEFF_TOKEN_TRAILING_COUNT];
extern const vlc_t x264_total_zeros[X264_TOTAL_ZEROS_COUNT][X264_TOTAL_ZEROS_SIZE];
extern const vlc_t x264_total_zeros_2x2_dc[X264_TOTAL_ZEROS_2X2_DC_COUNT][X264_TOTAL_ZEROS_2X2_DC_SIZE];
extern const vlc_t x264_total_zeros_2x4_dc[X264_TOTAL_ZEROS_2X4_DC_COUNT][X264_TOTAL_ZEROS_2X4_DC_SIZE];
extern const vlc_t x264_run_before_init[X264_RUN_BEFORE_COUNT][X264_RUN_BEFORE_SIZE];

extern uint8_t x264_zero[1024];

#endif
