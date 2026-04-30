/*****************************************************************************
 * gop.h: GOP output muxer
 *****************************************************************************
 * MIT License
 *
 * Copyright (c) 2018-2019 Xinyue Lu
 * Adapted for x264 by x264 project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *****************************************************************************
 * The MIT License applies to this file only.
 *****************************************************************************/

#ifndef X264_GOP_H
#define X264_GOP_H

#include "output.h"

/* GOP output context */
typedef struct
{
    FILE *gop_file;          /* Main .gop index file */
    FILE *data_file;         /* Current GOP data file */
    char *filename_prefix;   /* Base filename without extension */
    char *dir_prefix;        /* Directory path */
    int i_numframe;          /* Frame counter */
    int b_fail;              /* Failure flag */
} gop_hnd_t;

extern const cli_output_t gop_output;

#endif
