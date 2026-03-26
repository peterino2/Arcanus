#ifndef ARCANUS_FONTCACHE_H
#define ARCANUS_FONTCACHE_H

#include <stdint.h>

#define FONTCACHE_MAGIC 0x31434641u
#define FONTCACHE_VERSION 4u
#define FONTCACHE_FIRST_CHAR 32u
#define FONTCACHE_CHAR_COUNT 95u
#define FONTCACHE_ATLAS_WIDTH 1024u
#define FONTCACHE_ATLAS_HEIGHT 1024u
#define FONTCACHE_ATLAS_BASE_PX 64.0f
#define FONTCACHE_SDF_PADDING 5u
#define FONTCACHE_SDF_ON_EDGE_VALUE 180u
#define FONTCACHE_SDF_PIXEL_DIST_SCALE 36.0f
#define FONTCACHE_ATLAS_GAP 1u
#define FONTCACHE_FIXED_SHIFT 8
#define FONTCACHE_FIXED_SCALE (1 << FONTCACHE_FIXED_SHIFT)
#define FONTCACHE_BITMAP_CODEC_NONE 0u
#define FONTCACHE_BITMAP_CODEC_ZERO_RLE 1u

#pragma pack(push, 1)
typedef struct FontCacheHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t atlas_width;
    uint32_t atlas_height;
    uint32_t first_char;
    uint32_t char_count;
    uint32_t glyph_count;
    uint32_t kern_pair_count;
    uint32_t bitmap_codec;
    uint32_t bitmap_data_size;
    uint32_t sdf_padding;
    uint32_t sdf_on_edge_value;
    uint32_t atlas_gap;
    float atlas_base_px;
    float sdf_pixel_dist_scale;
    float font_scale;
    float font_ascent;
    float font_line_advance;
} FontCacheHeader;

typedef struct FontCacheGlyph {
    uint16_t atlas_x;
    uint16_t atlas_y;
    uint16_t width;
    uint16_t height;
    int16_t xoff_8_8;
    int16_t yoff_8_8;
    int16_t advance_8_8;
    uint32_t bitmap_offset;
    uint32_t bitmap_size;
} FontCacheGlyph;

typedef struct FontCacheKerningPair {
    uint8_t left_char;
    uint8_t right_char;
    int16_t amount_8_8;
} FontCacheKerningPair;
#pragma pack(pop)

#endif
