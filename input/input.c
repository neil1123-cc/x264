/*****************************************************************************
 * input.c: common input functions
 *****************************************************************************
 * Copyright (C) 2010-2025 x264 project
 *
 * Authors: Steven Walters <kemuri9@gmail.com>
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

#include "input.h"
#include <limits.h>

#ifdef _WIN32
#include <io.h>
#elif HAVE_MMAP
#include <sys/mman.h>
#include <unistd.h>
#endif

const x264_cli_csp_t x264_cli_csps[] = {
    [X264_CSP_I400] = { "i400", 1, { 1 },         { 1 },         1, 1 },
    [X264_CSP_I420] = { "i420", 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2 },
    [X264_CSP_I422] = { "i422", 3, { 1, .5, .5 }, { 1,  1,  1 }, 2, 1 },
    [X264_CSP_I444] = { "i444", 3, { 1,  1,  1 }, { 1,  1,  1 }, 1, 1 },
    [X264_CSP_YV12] = { "yv12", 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2 },
    [X264_CSP_YV16] = { "yv16", 3, { 1, .5, .5 }, { 1,  1,  1 }, 2, 1 },
    [X264_CSP_YV24] = { "yv24", 3, { 1,  1,  1 }, { 1,  1,  1 }, 1, 1 },
    [X264_CSP_NV12] = { "nv12", 2, { 1,  1 },     { 1, .5 },     2, 2 },
    [X264_CSP_NV21] = { "nv21", 2, { 1,  1 },     { 1, .5 },     2, 2 },
    [X264_CSP_NV16] = { "nv16", 2, { 1,  1 },     { 1,  1 },     2, 1 },
    [X264_CSP_YUYV] = { "yuyv", 1, { 2 },         { 1 },         2, 1 },
    [X264_CSP_UYVY] = { "uyvy", 1, { 2 },         { 1 },         2, 1 },
    [X264_CSP_BGR]  = { "bgr",  1, { 3 },         { 1 },         1, 1 },
    [X264_CSP_BGRA] = { "bgra", 1, { 4 },         { 1 },         1, 1 },
    [X264_CSP_RGB]  = { "rgb",  1, { 3 },         { 1 },         1, 1 },
};

int x264_cli_csp_is_invalid( int csp )
{
    int csp_mask = csp & X264_CSP_MASK;
    return csp_mask <= X264_CSP_NONE || csp_mask >= X264_CSP_CLI_MAX ||
           csp_mask == X264_CSP_V210 || csp & X264_CSP_OTHER;
}

int x264_cli_csp_depth_factor( int csp )
{
    if( x264_cli_csp_is_invalid( csp ) )
        return 0;
    return (csp & X264_CSP_HIGH_DEPTH) ? 2 : 1;
}

int64_t x264_cli_pic_plane_size( int csp, int width, int height, int plane )
{
    int csp_mask = csp & X264_CSP_MASK;
    if( x264_cli_csp_is_invalid( csp ) || width <= 0 || height <= 0 ||
        plane < 0 || plane >= x264_cli_csps[csp_mask].planes )
        return 0;
    int64_t size = (int64_t)width * height;
    size *= x264_cli_csps[csp_mask].width[plane] * x264_cli_csps[csp_mask].height[plane];
    size *= x264_cli_csp_depth_factor( csp );
    return size;
}

int64_t x264_cli_pic_size( int csp, int width, int height )
{
    if( x264_cli_csp_is_invalid( csp ) )
        return 0;
    int64_t size = 0;
    int csp_mask = csp & X264_CSP_MASK;
    for( int i = 0; i < x264_cli_csps[csp_mask].planes; i++ )
    {
        int64_t plane_size = x264_cli_pic_plane_size( csp, width, height, i );
        if( plane_size <= 0 || size > INT64_MAX - plane_size )
            return 0;
        size += plane_size;
    }
    return size;
}

static int cli_pic_plane_alloc_size( int csp, int csp_mask, int width, int height,
                                     int plane, int align, int *stride, int64_t *size )
{
    if( !stride || !size )
        return -1;
    int depth = x264_cli_csp_depth_factor( csp );
    int64_t plane_width = (int64_t)width * x264_cli_csps[csp_mask].width[plane];
    int64_t plane_height = (int64_t)height * x264_cli_csps[csp_mask].height[plane];
    if( depth <= 0 || align <= 0 || plane_width <= 0 || plane_height <= 0 ||
        plane_width > INT_MAX / depth )
        return -1;
    int64_t byte_width = plane_width * depth;
    if( byte_width > INT_MAX - (align - 1) )
        return -1;
    int aligned_stride = ALIGN( (int)byte_width, align );
    if( aligned_stride < byte_width || plane_height > INT64_MAX / aligned_stride )
        return -1;
    *stride = aligned_stride;
    *size = plane_height * aligned_stride;
    return 0;
}

static int cli_pic_init_internal( cli_pic_t *pic, int csp, int width, int height, int align, int alloc )
{
    if( !pic )
        return -1;
    memset( pic, 0, sizeof(cli_pic_t) );
    int csp_mask = csp & X264_CSP_MASK;
    if( x264_cli_csp_is_invalid( csp ) )
        pic->img.planes = 0;
    else
        pic->img.planes = x264_cli_csps[csp_mask].planes;
    pic->img.csp    = csp;
    pic->img.width  = width;
    pic->img.height = height;
    for( int i = 0; i < pic->img.planes; i++ )
    {
        int stride;
        int64_t size;
        if( cli_pic_plane_alloc_size( csp, csp_mask, width, height, i, align, &stride, &size ) )
            return -1;
        pic->img.stride[i] = stride;

        if( alloc )
        {
            pic->img.plane[i] = x264_malloc( size );
            if( !pic->img.plane[i] )
            {
                x264_cli_pic_clean( pic );
                return -1;
            }
        }
    }

    return 0;
}

int x264_cli_pic_alloc( cli_pic_t *pic, int csp, int width, int height )
{
    return cli_pic_init_internal( pic, csp, width, height, 1, 1 );
}

int x264_cli_pic_alloc_aligned( cli_pic_t *pic, int csp, int width, int height )
{
    return cli_pic_init_internal( pic, csp, width, height, NATIVE_ALIGN, 1 );
}

int x264_cli_pic_init_noalloc( cli_pic_t *pic, int csp, int width, int height )
{
    return cli_pic_init_internal( pic, csp, width, height, 1, 0 );
}

void x264_cli_pic_clean( cli_pic_t *pic )
{
    for( int i = 0; i < pic->img.planes; i++ )
        x264_free( pic->img.plane[i] );
    memset( pic, 0, sizeof(cli_pic_t) );
}

const x264_cli_csp_t *x264_cli_get_csp( int csp )
{
    if( x264_cli_csp_is_invalid( csp ) )
        return NULL;
    return x264_cli_csps + (csp&X264_CSP_MASK);
}

/* Functions for handling memory-mapped input frames */
int x264_cli_mmap_init( cli_mmap_t *h, FILE *fh )
{
#if defined(_WIN32) || HAVE_MMAP
    if( !h || !fh )
        return -1;
    memset( h, 0, sizeof(cli_mmap_t) );
#if HAVE_MMAP && !defined(_WIN32)
    h->fd = -1;
#endif
    int fd = fileno( fh );
    if( fd < 0 )
        return -1;
    x264_struct_stat file_stat;
    if( x264_fstat( fd, &file_stat ) || file_stat.st_size < 0 )
        return -1;
    h->file_size = file_stat.st_size;
#ifdef _WIN32
    HANDLE osfhandle = (HANDLE)_get_osfhandle( fd );
    if( osfhandle == INVALID_HANDLE_VALUE )
        return -1;
    SYSTEM_INFO si;
    GetSystemInfo( &si );
    if( !si.dwPageSize || !si.dwAllocationGranularity ||
        si.dwPageSize > INT_MAX || si.dwAllocationGranularity > INT_MAX )
        return -1;
    h->page_mask = (int)si.dwPageSize - 1;
    h->align_mask = (int)si.dwAllocationGranularity - 1;
    h->prefetch_virtual_memory = (void*)GetProcAddress( GetModuleHandleW( L"kernel32.dll" ), "PrefetchVirtualMemory" );
    h->process_handle = GetCurrentProcess();
    h->map_handle = CreateFileMappingW( osfhandle, NULL, PAGE_READONLY, 0, 0, NULL );
    return !h->map_handle;
#elif HAVE_MMAP && defined(_SC_PAGESIZE)
    long page_size = sysconf( _SC_PAGESIZE );
    if( page_size <= 0 || page_size > INT_MAX )
        return -1;
    h->align_mask = (int)page_size - 1;
    h->fd = fd;
    return 0;
#endif
#endif
    return -1;
}

/* Third-party filters such as swscale can overread the input buffer which may result
 * in segfaults. We have to pad the buffer size as a workaround to avoid that. */
#define MMAP_PADDING 64

void *x264_cli_mmap( cli_mmap_t *h, int64_t offset, int64_t size )
{
#if defined(_WIN32) || HAVE_MMAP
    uint8_t *base;
    if( !h || offset < 0 || size <= 0 || h->file_size < 0 || h->align_mask < 0 )
        return NULL;
#ifdef _WIN32
    if( !h->map_handle || h->page_mask < 0 )
        return NULL;
#else
    if( h->fd < 0 )
        return NULL;
#endif
    uint64_t file_size = h->file_size;
    uint64_t offset_u = offset;
    uint64_t size_u = size;
    if( offset_u > file_size || size_u > file_size - offset_u )
        return NULL;
    int align = offset_u & h->align_mask;
    uint64_t map_offset = offset_u - align;
    uint64_t map_size_u = size_u + align;
    if( map_size_u > SIZE_MAX - MMAP_PADDING )
        return NULL;
    size_t map_size = (size_t)map_size_u;
#ifdef _WIN32
    /* If the padding crosses a page boundary we need to increase the mapping size. */
    size_t padded_size = ((0 - map_size) & h->page_mask) < MMAP_PADDING ? map_size + MMAP_PADDING : map_size;
    if( UINT64_MAX - map_offset < padded_size )
        return NULL;
    if( map_offset + padded_size > file_size )
    {
        /* It's not possible to do the POSIX mmap() remapping trick on Windows, so if the padding crosses a
         * page boundary past the end of the file we have to copy the entire frame into a padded buffer. */
        if( (base = MapViewOfFile( h->map_handle, FILE_MAP_READ, (DWORD)(map_offset >> 32), (DWORD)map_offset, map_size )) )
        {
            uint8_t *buf = NULL;
            HANDLE anon_map = CreateFileMappingW( INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)((uint64_t)padded_size >> 32), (DWORD)padded_size, NULL );
            if( anon_map )
            {
                if( (buf = MapViewOfFile( anon_map, FILE_MAP_WRITE, 0, 0, 0 )) )
                {
                    buf += align;
                    memcpy( buf, base + align, map_size - align );
                }
                CloseHandle( anon_map );
            }
            UnmapViewOfFile( base );
            return buf;
        }
    }
    else if( (base = MapViewOfFile( h->map_handle, FILE_MAP_READ, (DWORD)(map_offset >> 32), (DWORD)map_offset, padded_size )) )
    {
        /* PrefetchVirtualMemory() is only available on Windows 8 and newer. */
        if( h->prefetch_virtual_memory )
        {
            struct { void *addr; size_t size; } mem_range = { base, map_size };
            h->prefetch_virtual_memory( h->process_handle, 1, &mem_range, 0 );
        }
        return base + align;
    }
#else
    size_t padded_size = map_size + MMAP_PADDING;
    if( (base = mmap( NULL, padded_size, PROT_READ, MAP_PRIVATE, h->fd, map_offset )) != MAP_FAILED )
    {
        /* Ask the OS to readahead pages. This improves performance whereas
         * forcing page faults by manually accessing every page does not.
         * Some systems have implemented madvise() but not posix_madvise()
         * and vice versa, so check both to see if either is available. */
#ifdef MADV_WILLNEED
        madvise( base, map_size, MADV_WILLNEED );
#elif defined(POSIX_MADV_WILLNEED)
        posix_madvise( base, map_size, POSIX_MADV_WILLNEED );
#endif
        /* Remap the file mapping of any padding that crosses a page boundary past the end of
         * the file into a copy of the last valid page to prevent reads from invalid memory. */
        size_t aligned_size = (padded_size - 1) & ~(size_t)h->align_mask;
        if( UINT64_MAX - map_offset < aligned_size )
        {
            munmap( base, padded_size );
            return NULL;
        }
        if( map_offset + aligned_size >= file_size )
        {
            uint64_t last_page = (map_offset + map_size - 1) & ~(uint64_t)h->align_mask;
            if( mmap( base + aligned_size, padded_size - aligned_size, PROT_READ, MAP_PRIVATE|MAP_FIXED, h->fd, last_page ) == MAP_FAILED )
            {
                munmap( base, padded_size );
                return NULL;
            }
        }

        return base + align;
    }
#endif
#endif
    return NULL;
}

int x264_cli_munmap( cli_mmap_t *h, void *addr, int64_t size )
{
#if defined(_WIN32) || HAVE_MMAP
    if( !h || !addr || size < 0 || h->align_mask < 0 )
        return -1;
    uintptr_t align = (uintptr_t)addr & (uintptr_t)h->align_mask;
    void *base = (void*)((uintptr_t)addr - align);
#ifdef _WIN32
    return !UnmapViewOfFile( base );
#else
    if( align > SIZE_MAX - MMAP_PADDING ||
        (uint64_t)size > SIZE_MAX - MMAP_PADDING - align )
        return -1;
    return munmap( base, (size_t)size + MMAP_PADDING + align );
#endif
#endif
    return -1;
}

void x264_cli_mmap_close( cli_mmap_t *h )
{
#ifdef _WIN32
    if( h && h->map_handle )
    {
        CloseHandle( h->map_handle );
        h->map_handle = NULL;
    }
#elif HAVE_MMAP
    if( h )
        h->fd = -1;
#endif
}
