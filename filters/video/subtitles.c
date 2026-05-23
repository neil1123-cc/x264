/*****************************************************************************
* subtitles.c: subtitles render filter using vsfilter
*****************************************************************************
* Copyright (C) 2003-2014 Zhou Zongyi <zhouzy_wuxi@hotmail.com>
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
*****************************************************************************/

#include <windows.h>
#include <float.h>
#include <limits.h>
#include "x264cli.h"
#include "subtitles.h"
#include "video.h"

csri_open_file_t csri_open_file;
csri_add_file_t csri_add_file;
csri_request_fmt_t csri_request_fmt;
csri_render_t csri_render;
csri_close_t csri_close;

//csri_openflag flags={};
enum { MAX_SUBTITLE_FILES = 16 };

static char *subfilename[MAX_SUBTITLE_FILES];
int subtotal = 0;
static HMODULE hVSFilter = 0;

int add_sub(char *filename)
{
	if( filename && subtotal < MAX_SUBTITLE_FILES )
	{
		subfilename[subtotal++] = filename;
		return 1;
	}
	return 0;
}

static const char* get_csri_fmt_name(unsigned int fmt)
{
	switch(fmt)
	{
	case CSRI_F_RGBA:
		return "RGBA";
	case CSRI_F_ARGB:
		return "ARGB";
	case CSRI_F_BGRA:
		return "BGRA";
	case CSRI_F_ABGR:
		return "ABGR";
	case CSRI_F_RGB_:
	case CSRI_F__RGB:
	case CSRI_F_RGB:
		return "RGB24";
	case CSRI_F_BGR_:
	case CSRI_F__BGR:
	case CSRI_F_BGR:
		return "BGR24";
//	case CSRI_F_AYUV:
//		return "AYUV";
//	case CSRI_F_YUVA:
//		return "YUVA";
//	case CSRI_F_YVUA:
//		return "YVUA";
	case CSRI_F_YUY2:
		return "YUY2";
//	case CSRI_F_YV12A:
//		return "YV12A";
	case CSRI_F_YV12:
		return "YV12";
	case CSRI_F_NV12:
		return "NV12";
	case CSRI_F_NV21:
		return "NV21";
	}
	return "Unknown";
}

static int subtitles_load_csri(void)
{
	if (hVSFilter)
		return 0;

#if ARCH_X86_64
	if (NULL == (hVSFilter = LoadLibraryA("VSFilter64.dll")))
	{
		x264_cli_log("subtitles", X264_LOG_ERROR, "failed to load VSFilter64.dll\n");
		return -1;
	}
#else
	if (NULL == (hVSFilter = LoadLibraryA("VSFilter.dll")))
	{
		x264_cli_log("subtitles", X264_LOG_ERROR, "failed to load VSFilter.dll\n");
		return -1;
	}
#endif
	csri_open_file = (csri_open_file_t)GetProcAddress(hVSFilter,"csri_open_file");
	csri_close = (csri_close_t)GetProcAddress(hVSFilter,"csri_close");
	csri_request_fmt = (csri_request_fmt_t)GetProcAddress(hVSFilter,"csri_request_fmt");
	csri_render = (csri_render_t)GetProcAddress(hVSFilter,"csri_render");
	csri_add_file = (csri_add_file_t)GetProcAddress(hVSFilter,"csri_add_file");
	if( !csri_open_file || !csri_close || !csri_request_fmt || !csri_render )
	{
		x264_cli_log("subtitles", X264_LOG_ERROR, "VSFilter missing required CSRI interface\n");
		csri_open_file = NULL;
		csri_close = NULL;
		csri_request_fmt = NULL;
		csri_render = NULL;
		csri_add_file = NULL;
		FreeLibrary(hVSFilter);
		hVSFilter = 0;
		return -1;
	}

	return 0;
}

void* subtitles_new_renderer(const csri_fmt *fmt, uint32_t sarw, uint32_t sarh)
{
	int i;
	csri_openflag flag;
	void *subrenderinst;

	if (!fmt || !subtotal || !subfilename[0] || subtitles_load_csri())
		return 0;
	if (sarw && sarh && sarw != sarh) // non-square par
	{
		flag.name = "PAR";
		flag.data.dval = (double)sarw / sarh;
		flag.next = NULL;
	}
	else
		flag.name = NULL;

	if (NULL == (subrenderinst = csri_open_file((void*)"vsfilter", subfilename[0], flag.name?&flag:NULL)))
	{
		x264_cli_log("subtitles", X264_LOG_ERROR, "failed to create subtitles renderer\n");
		return 0;
	}
	if (csri_request_fmt(subrenderinst, fmt))
	{
		x264_cli_log("subtitles", X264_LOG_ERROR, "csri does not support %s input\n", get_csri_fmt_name(fmt->pixfmt));
		csri_close(subrenderinst);
		return 0;
	}
	x264_cli_log("subtitles", X264_LOG_INFO, "loaded subtitles \"%s\"\n", subfilename[0]);
	if (csri_add_file)
		for (i=1; i<subtotal; i++)
		{
			if (csri_add_file(subrenderinst, subfilename[i], flag.name?&flag:NULL))
				x264_cli_log("subtitles", X264_LOG_INFO, "loaded subtitles \"%s\"\n", subfilename[i]);
			else
				x264_cli_log("subtitles", X264_LOG_WARNING, "failed to load subtitles \"%s\"\n", subfilename[i]);
		}
	else
		x264_cli_log("subtitles", X264_LOG_WARNING, "no csri_add_file interface, fail to render subtitles\n");
	return subrenderinst;
}

#define NAME "subtitles"
#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, NAME, __VA_ARGS__ )

cli_vid_filter_t subtitles_filter;

typedef struct
{
	hnd_t prev_hnd;
	cli_vid_filter_t prev_filter;
	void *subrenderinst;
	int csp;
	int width;
	int height;
	unsigned int fmt;
	double scale_factor;
	int vfr;
} subtitles_hnd_t;

static int subtitles_abs_stride( int stride )
{
	if( stride == INT_MIN )
		return -1;
	return stride < 0 ? -stride : stride;
}

static int subtitles_frame_is_invalid( subtitles_hnd_t *h, cli_pic_t *pic )
{
	if( !h || !pic || (pic->img.csp & X264_CSP_MASK) != h->csp ||
	    pic->img.width != h->width || pic->img.height != h->height ||
	    pic->img.planes != 3 )
		return 1;

	int luma_stride = subtitles_abs_stride( pic->img.stride[0] );
	int chroma_stride1 = subtitles_abs_stride( pic->img.stride[1] );
	int chroma_stride2 = subtitles_abs_stride( pic->img.stride[2] );
	int chroma_width = h->width / 2;
	if( luma_stride < h->width || chroma_stride1 < chroma_width || chroma_stride2 < chroma_width ||
	    !pic->img.plane[0] || !pic->img.plane[1] || !pic->img.plane[2] )
		return 1;

	return 0;
}

static int init( hnd_t *handle, cli_vid_filter_t *filter, video_info_t *info, x264_param_t *param, char *opt_string )
{
	if (!subtotal)
		return 0;

	subtitles_hnd_t *h;
	csri_fmt fmt;
	if( !handle || !*handle || !filter || !info ||
	    !filter->get_frame || !filter->release_frame || !filter->free )
		return -1;
	if (!(h = calloc(1, sizeof(subtitles_hnd_t))))
		return -1;
	fmt.width = info->width;
	fmt.height = info->height;
	h->csp = info->csp & X264_CSP_MASK;
	switch(h->csp)
	{
	case X264_CSP_I420:
	case X264_CSP_YV12:
		fmt.pixfmt = CSRI_F_YV12;
		break;
	case X264_CSP_NV12:
		fmt.pixfmt = CSRI_F_NV12;
		break;
	case X264_CSP_BGR:
		fmt.pixfmt = CSRI_F_BGR;
		break;
	case X264_CSP_BGRA:
		fmt.pixfmt = CSRI_F_BGRA;
		break;
	case X264_CSP_RGB:
		fmt.pixfmt = CSRI_F_RGB;
		break;
	default:
		x264_cli_log( NAME, X264_LOG_ERROR, "unsupported colorspace\n");
		fmt.pixfmt = -1;
	}
	if (fmt.pixfmt != CSRI_F_YV12) /* workaround: currently only YV12 works fine */
	{
		x264_cli_log( NAME, X264_LOG_ERROR, "unsupported colorspace\n");
		fmt.pixfmt = -1;
	}
	double scale_factor;
	if( info->vfr )
	{
		if( info->timebase_num <= 0 || info->timebase_den <= 0 ||
		    info->timebase_convert_multiplier != info->timebase_convert_multiplier )
		{
			free(h);
			return -1;
		}
		scale_factor = info->timebase_convert_multiplier * (double)info->timebase_num /
		               (double)info->timebase_den;
	}
	else
	{
		if( info->fps_num <= 0 || info->fps_den <= 0 )
		{
			free(h);
			return -1;
		}
		scale_factor = (double)info->fps_den / (double)info->fps_num;
	}
	if( scale_factor != scale_factor || scale_factor <= 0.0 || scale_factor > DBL_MAX )
	{
		free(h);
		return -1;
	}
	if (fmt.pixfmt == -1 || !(h->subrenderinst = subtitles_new_renderer(&fmt, info->sar_width, info->sar_height)))
	{
		free(h);
		return -1;
	}
	h->fmt = fmt.pixfmt;
	h->width = info->width;
	h->height = info->height;
	h->scale_factor = scale_factor;
	h->vfr = info->vfr;

	h->prev_filter = *filter;
	h->prev_hnd = *handle;
	*handle = h;
	*filter = subtitles_filter;
	return 0;
}

static int get_frame( hnd_t handle, cli_pic_t *output, int frame )
{
	subtitles_hnd_t *h = handle;
	csri_frame fr = {0};
	if( !h || !output || !h->subrenderinst || !csri_render || !h->prev_hnd ||
	    !h->prev_filter.get_frame || !h->prev_filter.release_frame || frame < 0 )
		return -1;
	if( h->prev_filter.get_frame( h->prev_hnd, output, frame ) )
		return -1;
	if( subtitles_frame_is_invalid( h, output ) )
	{
		h->prev_filter.release_frame( h->prev_hnd, output, frame );
		return -1;
	}
	fr.planes[0] = output->img.plane[0];
	fr.strides[0] = output->img.stride[0];
	switch(h->csp)
	{
	case X264_CSP_I420:
		fr.planes[1] = output->img.plane[2];
		fr.planes[2] = output->img.plane[1];
		goto L_YV12;
	case X264_CSP_YV12:
		fr.planes[1] = output->img.plane[1];
		fr.planes[2] = output->img.plane[2];
		L_YV12:
		fr.strides[1] = output->img.stride[1];
		fr.strides[2] = output->img.stride[2];
		break;
	case X264_CSP_NV12:
		fr.planes[1] = output->img.plane[1];
		fr.planes[2] = 0;
		fr.strides[1] = output->img.stride[1];
		break;
	default:
		h->prev_filter.release_frame( h->prev_hnd, output, frame );
		return -1;
	}
	if( !fr.planes[0] || !fr.planes[1] || !fr.strides[0] || !fr.strides[1] ||
	    (h->csp != X264_CSP_NV12 && !fr.planes[2]) )
	{
		h->prev_filter.release_frame( h->prev_hnd, output, frame );
		return -1;
	}
	fr.pixfmt = h->fmt;
	double render_time = (h->vfr ? output->pts : frame) * h->scale_factor;
	if( !isfinite( render_time ) )
	{
		h->prev_filter.release_frame( h->prev_hnd, output, frame );
		return -1;
	}
	csri_render( h->subrenderinst, &fr, render_time );
	return 0;
}

static int release_frame( hnd_t handle, cli_pic_t *pic, int frame )
{
	subtitles_hnd_t *h = handle;
	/* NO filter should ever have a dependent release based on the plane pointers,
	 * so avoid unnecessary unshifting */
	if( !h || !pic || !h->prev_hnd || !h->prev_filter.release_frame )
		return -1;
	return h->prev_filter.release_frame( h->prev_hnd, pic, frame );
}

static void free_filter( hnd_t handle )
{
	subtitles_hnd_t *h = handle;
	if( !h )
		return;
	if( h->subrenderinst && csri_close )
		csri_close( h->subrenderinst );
	if( h->prev_hnd && h->prev_filter.free )
		h->prev_filter.free( h->prev_hnd );
	free( h );
}

static void help( int longhelp )
{
    printf( "      "NAME":\n" );
    if( longhelp )
    {
        printf( "            renders subtitles using VSFilter\n" );
#if ARCH_X86_64
        printf( "            need VSFilter64.dll\n" );
#else
        printf( "            need VSFilter.dll\n" );
#endif
    }
    printf( "\n"
            "      --sub <string>          Load subtitles file (used with video filter \"subtitles\")\n");
    if( longhelp )
        printf( "                              can be called more than once to load multiple subtitles\n" );
}

cli_vid_filter_t subtitles_filter = { NAME, help, init, get_frame, release_frame, free_filter, NULL };
