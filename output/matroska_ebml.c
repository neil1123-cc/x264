/*****************************************************************************
 * matroska_ebml.c: matroska muxer utilities
 *****************************************************************************
 * Copyright (C) 2005-2025 x264 project
 *
 * Authors: Mike Matsnev <mike@haali.su>
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
#include "matroska_ebml.h"
#include <float.h>

#define CLSIZE 1048576
#define CHECK(x)\
do {\
    if( (x) < 0 )\
        return -1;\
} while( 0 )

struct mk_context
{
    struct mk_context *next, **prev, *parent;
    mk_writer *owner;
    unsigned id;

    void *data;
    unsigned d_cur, d_max;
};

typedef struct mk_context mk_context;

struct mk_writer
{
    FILE *fp;

    unsigned duration_ptr;

    mk_context *root, *cluster, *frame;
    mk_context *freelist;
    mk_context *actlist;

    unsigned track_count;
    int64_t def_duration[MK_MAX_TRACKS];
    int64_t timescale;
    int64_t cluster_tc_scaled;
    int64_t frame_tc;
    int64_t max_frame_tc[MK_MAX_TRACKS];

    int8_t wrote_header, in_frame, keyframe, skippable;
};

static mk_context *mk_create_context( mk_writer *w, mk_context *parent, unsigned id )
{
    mk_context *c;

    if( w->freelist )
    {
        c = w->freelist;
        w->freelist = w->freelist->next;
    }
    else
    {
        int64_t context_alloc_size = sizeof(mk_context);
        c = calloc( 1, context_alloc_size );
        if( !c )
            return NULL;
    }

    c->parent = parent;
    c->owner = w;
    c->id = id;

    if( c->owner->actlist )
        c->owner->actlist->prev = &c->next;
    c->next = c->owner->actlist;
    c->prev = &c->owner->actlist;
    c->owner->actlist = c;

    return c;
}

static int mk_append_context_data( mk_context *c, const void *data, unsigned size )
{
    if( !c )
        return -1;
    if( size && !data )
        return -1;
    if( size > UINT_MAX - c->d_cur )
        return -1;
    unsigned ns = c->d_cur + size;

    if( ns > c->d_max )
    {
        unsigned dn = c->d_max ? c->d_max : 16;
        while( ns > dn )
        {
            if( dn > UINT_MAX / 2 )
            {
                dn = ns;
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
    }

    if( size )
        memcpy( (uint8_t*)c->data + c->d_cur, data, size );

    c->d_cur = ns;

    return 0;
}

static int mk_write_id( mk_context *c, unsigned id )
{
    uint8_t c_id[4] = { (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id };

    if( c_id[0] )
        return mk_append_context_data( c, c_id, 4 );
    if( c_id[1] )
        return mk_append_context_data( c, c_id+1, 3 );
    if( c_id[2] )
        return mk_append_context_data( c, c_id+2, 2 );
    return mk_append_context_data( c, c_id+3, 1 );
}

static int mk_write_size( mk_context *c, unsigned size )
{
    uint8_t c_size[5] = { 0x08, (uint8_t)(size >> 24), (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size };

    if( size < 0x7f )
    {
        c_size[4] |= 0x80;
        return mk_append_context_data( c, c_size+4, 1 );
    }
    if( size < 0x3fff )
    {
        c_size[3] |= 0x40;
        return mk_append_context_data( c, c_size+3, 2 );
    }
    if( size < 0x1fffff )
    {
        c_size[2] |= 0x20;
        return mk_append_context_data( c, c_size+2, 3 );
    }
    if( size < 0x0fffffff )
    {
        c_size[1] |= 0x10;
        return mk_append_context_data( c, c_size+1, 4 );
    }
    return mk_append_context_data( c, c_size, 5 );
}

static int mk_flush_context_id( mk_context *c )
{
    uint8_t ff = 0xff;

    if( !c->id )
        return 0;

    CHECK( mk_write_id( c->parent, c->id ) );
    CHECK( mk_append_context_data( c->parent, &ff, 1 ) );

    c->id = 0;

    return 0;
}

static int mk_flush_context_data( mk_context *c )
{
    if( !c->d_cur )
        return 0;

    if( c->parent )
        CHECK( mk_append_context_data( c->parent, c->data, c->d_cur ) );
    else if( fwrite( c->data, c->d_cur, 1, c->owner->fp ) != 1 )
        return -1;

    c->d_cur = 0;

    return 0;
}

static int mk_close_context( mk_context *c, unsigned *off )
{
    if( c->id )
    {
        CHECK( mk_write_id( c->parent, c->id ) );
        CHECK( mk_write_size( c->parent, c->d_cur ) );
    }

    if( c->parent && off )
    {
        if( *off > UINT_MAX - c->parent->d_cur )
            return -1;
        *off += c->parent->d_cur;
    }

    CHECK( mk_flush_context_data( c ) );

    if( c->next )
        c->next->prev = c->prev;
    *(c->prev) = c->next;
    c->next = c->owner->freelist;
    c->owner->freelist = c;

    return 0;
}

static void mk_destroy_contexts( mk_writer *w )
{
    mk_context *next;

    for( mk_context *cur = w->freelist; cur; cur = next )
    {
        next = cur->next;
        free( cur->data );
        free( cur );
    }

    for( mk_context *cur = w->actlist; cur; cur = next )
    {
        next = cur->next;
        free( cur->data );
        free( cur );
    }

    w->freelist = w->actlist = w->root = NULL;
}

static int mk_write_string( mk_context *c, unsigned id, const char *str )
{
    if( !str )
        return -1;
    size_t len = strlen( str );
    if( len > UINT_MAX )
        return -1;

    CHECK( mk_write_id( c, id ) );
    CHECK( mk_write_size( c, (unsigned)len ) );
    CHECK( mk_append_context_data( c, str, (unsigned)len ) );
    return 0;
}

static int mk_write_bin( mk_context *c, unsigned id, const void *data, unsigned size )
{
    CHECK( mk_write_id( c, id ) );
    CHECK( mk_write_size( c, size ) );
    CHECK( mk_append_context_data( c, data, size ) );
    return 0;
}

static int mk_write_uint( mk_context *c, unsigned id, uint64_t ui )
{
    uint8_t c_ui[8] = { (uint8_t)(ui >> 56), (uint8_t)(ui >> 48), (uint8_t)(ui >> 40), (uint8_t)(ui >> 32),
                        (uint8_t)(ui >> 24), (uint8_t)(ui >> 16), (uint8_t)(ui >> 8), (uint8_t)ui };
    unsigned i = 0;

    CHECK( mk_write_id( c, id ) );
    while( i < 7 && !c_ui[i] )
        ++i;
    CHECK( mk_write_size( c, 8 - i ) );
    CHECK( mk_append_context_data( c, c_ui+i, 8 - i ) );
    return 0;
}

static int mk_write_float_raw( mk_context *c, float f )
{
    union
    {
        float f;
        uint32_t u;
    } u;
    uint8_t c_f[4];

    u.f = f;
    c_f[0] = (uint8_t)(u.u >> 24);
    c_f[1] = (uint8_t)(u.u >> 16);
    c_f[2] = (uint8_t)(u.u >> 8);
    c_f[3] = (uint8_t)u.u;

    return mk_append_context_data( c, c_f, 4 );
}

static int mk_write_float( mk_context *c, unsigned id, float f )
{
    CHECK( mk_write_id( c, id ) );
    CHECK( mk_write_size( c, 4 ) );
    CHECK( mk_write_float_raw( c, f ) );
    return 0;
}

mk_writer *mk_create_writer( const char *filename )
{
    if( !filename )
        return NULL;

    int64_t writer_alloc_size = sizeof(mk_writer);
    mk_writer *w = calloc( 1, writer_alloc_size );
    if( !w )
        return NULL;

    w->root = mk_create_context( w, NULL, 0 );
    if( !w->root )
    {
        free( w );
        return NULL;
    }

    if( !strcmp( filename, "-" ) )
        w->fp = stdout;
    else
        w->fp = x264_fopen( filename, "wb" );
    if( !w->fp )
    {
        mk_destroy_contexts( w );
        free( w );
        return NULL;
    }

    w->timescale = 1000000;

    return w;
}

static int mk_write_track( mk_writer *w, mk_context *c, mk_track_t track )
{
    mk_context  *ti, *t;

    if( track.id == 0 || track.id >= MK_MAX_TRACKS || !track.codec_id )
        return -1;
    if( track.codec_private_size && !track.codec_private )
        return -1;

    if( !(ti = mk_create_context( w, c, 0xae )) ) // TrackEntry
        return -1;
    CHECK( mk_write_uint( ti, 0xd7, track.id ) ); // TrackNumber
    CHECK( mk_write_uint( ti, 0x73c5, track.id ) ); // TrackUID
    CHECK( mk_write_uint( ti, 0x83, track.type ) ); // TrackType
    CHECK( mk_write_uint( ti, 0x9c, track.lacing != MK_LACING_NONE ) ); // FlagLacing
    CHECK( mk_write_string( ti, 0x86, track.codec_id ) ); // codec_id
    if( track.codec_private_size )
        CHECK( mk_write_bin( ti, 0x63a2, track.codec_private, track.codec_private_size ) ); // codec_private
    if( track.default_frame_duration < 0 )
        return -1;
    if( track.default_frame_duration )
    {
        CHECK( mk_write_uint( ti, 0x23e383, (uint64_t)track.default_frame_duration ) ); // DefaultDuration
        w->def_duration[track.id] = track.default_frame_duration;
    }

    switch( track.type )
    {
        case MK_TRACK_VIDEO:
            if( track.info.v.display_size_units < 0 )
                return -1;
            if( !(t = mk_create_context( w, ti, 0xe0 ) ) ) // Video
                return -1;
            CHECK( mk_write_uint( t, 0xb0, track.info.v.width ) );
            CHECK( mk_write_uint( t, 0xba, track.info.v.height ) );
            CHECK( mk_write_uint( t, 0x54b2, (uint64_t)track.info.v.display_size_units ) );
            CHECK( mk_write_uint( t, 0x54b0, track.info.v.display_width ) );
            CHECK( mk_write_uint( t, 0x54ba, track.info.v.display_height ) );
            if( track.info.v.stereo_mode >= 0 )
                CHECK( mk_write_uint( t, 0x53b8, (uint64_t)track.info.v.stereo_mode ) );
            CHECK( mk_close_context( t, 0 ) );
            break;
        case MK_TRACK_AUDIO:
            if( !(t = mk_create_context( w, ti, 0xe1 ) ) ) // Audio
                return -1;
            CHECK( mk_write_float( t, 0xb5, (float)track.info.a.samplerate ) );
            if( track.info.a.output_samplerate && ( track.info.a.samplerate != track.info.a.output_samplerate ) )
                CHECK( mk_write_float( t, 0x78b5, (float)track.info.a.output_samplerate ) );
            CHECK( mk_write_uint( t, 0x9f, track.info.a.channels ) );
            if( track.info.a.bit_depth )
                CHECK( mk_write_uint( t, 0x6264, track.info.a.bit_depth ) );
            CHECK( mk_close_context( t, 0 ) );
            break;
        default:
            break;
    }

    CHECK( mk_close_context( ti, 0 ) );

    return 0;
}

int mk_write_header( mk_writer *w, const char *writing_app, int64_t timescale,
                     mk_track_t *tracks, int track_count )
{
    mk_context  *c;
    int i,j;

    if( !w || !writing_app || !tracks )
        return -1;
    if( w->wrote_header )
        return -1;
    if( timescale <= 0 || track_count <= 0 || track_count >= MK_MAX_TRACKS )
        return -1;

    w->timescale = timescale;
    w->track_count = (unsigned)track_count;
    for( i=0; i<MK_MAX_TRACKS; i++ )
        w->def_duration[i] = w->max_frame_tc[i] = 0;

    if( !(c = mk_create_context( w, w->root, 0x1a45dfa3 )) ) // EBML
		return -1;

	j=-1;
	for( i=1; i<=track_count; i++ )
		if (tracks[i].type==MK_TRACK_VIDEO) j=i;
	
    CHECK( mk_write_uint( c, 0x4286, 1 ) ); // EBMLVersion
    CHECK( mk_write_uint( c, 0x42f7, 1 ) ); // EBMLReadVersion
    CHECK( mk_write_uint( c, 0x42f2, 4 ) ); // EBMLMaxIDLength
    CHECK( mk_write_uint( c, 0x42f3, 8 ) ); // EBMLMaxSizeLength
    CHECK( mk_write_string( c, 0x4282, "matroska") ); // DocType
	if (j==-1) CHECK( mk_write_uint( c, 0x4287, 2 ) ); // DocTypeVersion
	else CHECK( mk_write_uint( c, 0x4287, tracks[j].info.v.stereo_mode >= 0 ? 3 : 2 ) ); // DocTypeVersion
    CHECK( mk_write_uint( c, 0x4287, 2 ) ); // DocTypeVersion
    CHECK( mk_write_uint( c, 0x4285, 2 ) ); // DocTypeReadVersion
    CHECK( mk_close_context( c, 0 ) );

    if( !(c = mk_create_context( w, w->root, 0x18538067 )) ) // Segment
        return -1;
    CHECK( mk_flush_context_id( c ) );
    CHECK( mk_close_context( c, 0 ) );

    if( !(c = mk_create_context( w, w->root, 0x1549a966 )) ) // SegmentInfo
        return -1;
    CHECK( mk_write_string( c, 0x4d80, "Haali Matroska Writer b0" ) ); // MuxingApp
    CHECK( mk_write_string( c, 0x5741, writing_app ) ); // WritingApp
    CHECK( mk_write_uint( c, 0x2ad7b1, (uint64_t)w->timescale ) ); // TimecodeScale
    CHECK( mk_write_float( c, 0x4489, 0) ); // Duration
    if( c->d_cur < 4 )
        return -1;
    w->duration_ptr = c->d_cur - 4;
    CHECK( mk_close_context( c, &w->duration_ptr ) );

    if( !(c = mk_create_context( w, w->root, 0x1654ae6b )) ) // Tracks
        return -1;
 

    for( i=1; i<=track_count; i++ )
        CHECK( mk_write_track( w, c, tracks[i] ) );
    CHECK( mk_close_context( c, 0 ) );

    CHECK( mk_flush_context_data( w->root ) );

    w->wrote_header = 1;

    return 0;
}

static int mk_close_cluster( mk_writer *w )
{
    if( w->cluster == NULL )
        return 0;
    CHECK( mk_close_context( w->cluster, 0 ) );
    w->cluster = NULL;
    CHECK( mk_flush_context_data( w->root ) );
    return 0;
}

static int mk_flush_frame( mk_writer *w, uint32_t track_id )
{
    int64_t delta;
    unsigned fsize;
    uint8_t c_delta_flags[3];

    if( !w->in_frame )
        return 0;
    if( track_id == 0 || track_id > w->track_count || track_id >= MK_MAX_TRACKS || w->timescale <= 0 || w->frame_tc < 0 )
        return -1;

    delta = w->frame_tc/w->timescale - w->cluster_tc_scaled;
    if( delta > 32767ll || delta < -32768ll )
        CHECK( mk_close_cluster( w ) );

    if( !w->cluster )
    {
        w->cluster_tc_scaled = w->frame_tc / w->timescale;
        w->cluster = mk_create_context( w, w->root, 0x1f43b675 ); // Cluster
        if( !w->cluster )
            return -1;

        CHECK( mk_write_uint( w->cluster, 0xe7, (uint64_t)w->cluster_tc_scaled ) ); // Timecode

        delta = 0;
    }

    fsize = w->frame ? w->frame->d_cur : 0;
    if( fsize > UINT_MAX - 4 )
        return -1;

    CHECK( mk_write_id( w->cluster, 0xa3 ) ); // SimpleBlock
    CHECK( mk_write_size( w->cluster, fsize + 4 ) ); // Size
	CHECK( mk_write_size( w->cluster, track_id ) ); // TrackNumber

    uint16_t delta_bits = (uint16_t)(int16_t)delta;
    c_delta_flags[0] = (uint8_t)(delta_bits >> 8);
    c_delta_flags[1] = (uint8_t)delta_bits;
    c_delta_flags[2] = (uint8_t)(((unsigned)w->keyframe << 7) | (unsigned)w->skippable);
    CHECK( mk_append_context_data( w->cluster, c_delta_flags, 3 ) ); // Timecode, Flags
    if( w->frame )
    {
        CHECK( mk_append_context_data( w->cluster, w->frame->data, w->frame->d_cur ) ); // Data
        w->frame->d_cur = 0;
    }

    w->in_frame = 0;

    if( w->cluster->d_cur > CLSIZE )
        CHECK( mk_close_cluster( w ) );

    if( w->max_frame_tc[track_id] < w->frame_tc )
        w->max_frame_tc[track_id] = w->frame_tc;

    return 0;
}

int mk_end_frame( mk_writer *w, uint32_t track_id )
{
    if( !w )
        return -1;
    if( mk_flush_frame( w, track_id ) < 0 )
        return -1;

    w->in_frame  = 0;

    return 0;
}

int mk_start_frame( mk_writer *w )
{
    if( !w || w->in_frame )
        return -1;
    w->in_frame  = 1;
    w->frame_tc  = -1;
    w->keyframe  = 0;
    w->skippable = 0;

    return 0;
}

int mk_set_frame_flags( mk_writer *w, int64_t timestamp, int keyframe, int skippable, uint32_t track_id )
{
    if( !w || !w->in_frame )
        return -1;
    if( timestamp < 0 || track_id == 0 || track_id > w->track_count || track_id >= MK_MAX_TRACKS )
        return -1;

    w->frame_tc  = timestamp;
    w->keyframe  = keyframe  != 0;
    w->skippable = skippable != 0;

    return 0;
}

int mk_add_frame_data( mk_writer *w, const void *data, unsigned size )
{
    if( !w || !w->in_frame )
        return -1;
    if( size && !data )
        return -1;

    if( !w->frame )
    {
        if( !(w->frame = mk_create_context( w, NULL, 0 )) )
            return -1;
    }

    return mk_append_context_data( w->frame, data, size );
}

int mk_close( mk_writer *w, int64_t *last_delta )
{
    int ret = 0;
    if( !w )
        return -1;
    if( !w->fp )
    {
        mk_destroy_contexts( w );
        free( w );
        return -1;
    }
    if( w->in_frame )
        ret = -1;
    if( mk_close_cluster( w ) < 0 )
        ret = -1;
    if( w->wrote_header && x264_is_regular_file( w->fp ) )
    {
        if( !last_delta || w->track_count >= MK_MAX_TRACKS )
            ret = -1;
        else
        {
            int64_t total_duration = INT64_MAX;
            for( uint32_t i = 1; i <= w->track_count; i++ )
            {
                int64_t last_frametime = last_delta[i] ? last_delta[i] : w->def_duration[i];
                if( last_frametime < 0 || w->max_frame_tc[i] < 0 ||
                    last_frametime > INT64_MAX - w->max_frame_tc[i] )
                {
                    ret = -1;
                    continue;
                }
                int64_t track_duration = w->max_frame_tc[i] + last_frametime;
                total_duration = X264_MIN( track_duration, total_duration );
            }
            double duration = w->timescale > 0 && total_duration != INT64_MAX
                            ? (double)total_duration / (double)w->timescale : -1.0;
            if( duration < 0.0 || duration > FLT_MAX || w->duration_ptr > LONG_MAX ||
                fseek( w->fp, (long)w->duration_ptr, SEEK_SET ) ||
                mk_write_float_raw( w->root, (float)duration ) < 0 ||
                mk_flush_context_data( w->root ) < 0 )
                ret = -1;
        }
    }
    mk_destroy_contexts( w );
    if( w->fp == stdout ? fflush( w->fp ) : fclose( w->fp ) )
        ret = -1;
    free( w );
    return ret;
}
