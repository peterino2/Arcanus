#define _CRT_SECURE_NO_WARNINGS

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../fontcache.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "vendor/stb_truetype.h"

static bool LoadFile(const char *path, std::vector<unsigned char> *out_data)
{
    FILE *file = std::fopen(path, "rb");
    long size_long = 0;
    std::vector<unsigned char> data;

    if (file == nullptr) {
        std::cerr << "failed to open " << path << '\n';
        return false;
    }

    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        std::cerr << "failed to seek " << path << '\n';
        return false;
    }

    size_long = std::ftell(file);
    if (size_long <= 0) {
        std::fclose(file);
        std::cerr << "invalid size for " << path << '\n';
        return false;
    }

    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        std::cerr << "failed to rewind " << path << '\n';
        return false;
    }

    data.resize(static_cast<std::size_t>(size_long));
    if (std::fread(data.data(), 1, data.size(), file) != data.size()) {
        std::fclose(file);
        std::cerr << "failed to read " << path << '\n';
        return false;
    }

    std::fclose(file);
    *out_data = std::move(data);
    return true;
}

static bool WriteFile(const char *path, const void *data, std::size_t size)
{
    FILE *file = std::fopen(path, "wb");

    if (file == nullptr) {
        std::cerr << "failed to open output " << path << '\n';
        return false;
    }

    if (std::fwrite(data, 1, size, file) != size) {
        std::fclose(file);
        std::cerr << "failed to write " << path << '\n';
        return false;
    }

    std::fclose(file);
    return true;
}

static void CollectFonts(const char *pattern, std::vector<std::string> *out_paths)
{
    WIN32_FIND_DATAA find_data = {};
    HANDLE find_handle = FindFirstFileA(pattern, &find_data);

    if (find_handle == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            out_paths->push_back(find_data.cFileName);
        }
    } while (FindNextFileA(find_handle, &find_data) != 0);

    FindClose(find_handle);
}

static std::string ReplaceExtension(const std::string &path, const char *extension)
{
    std::string result = path;
    std::size_t dot = result.find_last_of('.');

    if (dot == std::string::npos) {
        result += extension;
    } else {
        result.erase(dot);
        result += extension;
    }

    return result;
}

static bool FloatToFixed8_8(float value, std::int16_t *out_value)
{
    int scaled = value >= 0.0f
        ? (int)(value * (float)FONTCACHE_FIXED_SCALE + 0.5f)
        : (int)(value * (float)FONTCACHE_FIXED_SCALE - 0.5f);

    if (scaled < -32768 || scaled > 32767) {
        return false;
    }

    *out_value = (std::int16_t)scaled;
    return true;
}

static void EncodeGlyphBitmap(const unsigned char *src, std::size_t size, std::vector<unsigned char> *out_data)
{
    std::size_t i = 0;

    while (i < size) {
        if (src[i] == 0) {
            std::size_t run = 1;

            while (i + run < size && src[i + run] == 0 && run < 128) {
                ++run;
            }

            out_data->push_back((unsigned char)(0x80u | (unsigned char)(run - 1)));
            i += run;
            continue;
        }

        {
            std::size_t run = 1;

            while (i + run < size && src[i + run] != 0 && run < 128) {
                ++run;
            }

            out_data->push_back((unsigned char)(run - 1));
            out_data->insert(out_data->end(), src + i, src + i + run);
            i += run;
        }
    }
}

static bool BuildFontCache(const std::string &font_path)
{
    std::vector<unsigned char> font_file;
    std::vector<unsigned char> bitmap_data;
    std::vector<FontCacheGlyph> glyphs;
    std::vector<FontCacheKerningPair> kern_pairs;
    std::vector<unsigned char> cache_file;
    stbtt_fontinfo font_info = {};
    int ascent = 0;
    int descent = 0;
    int line_gap = 0;
    float font_scale = 0.0f;
    float font_ascent = 0.0f;
    float font_line_advance = 0.0f;
    int pen_x = 0;
    int pen_y = 0;
    int row_h = 0;
    const std::string cache_path = ReplaceExtension(font_path, ".fontcache");

    if (!LoadFile(font_path.c_str(), &font_file)) {
        return false;
    }

    if (stbtt_InitFont(&font_info, font_file.data(), stbtt_GetFontOffsetForIndex(font_file.data(), 0)) == 0) {
        std::cerr << "stbtt_InitFont failed for " << font_path << '\n';
        return false;
    }

    glyphs.assign(FONTCACHE_CHAR_COUNT, FontCacheGlyph{});
    font_scale = stbtt_ScaleForPixelHeight(&font_info, FONTCACHE_ATLAS_BASE_PX);
    stbtt_GetFontVMetrics(&font_info, &ascent, &descent, &line_gap);
    font_ascent = (float)ascent * font_scale;
    font_line_advance = (float)(ascent - descent + line_gap) * font_scale;

    for (unsigned int left = FONTCACHE_FIRST_CHAR; left < FONTCACHE_FIRST_CHAR + FONTCACHE_CHAR_COUNT; ++left) {
        for (unsigned int right = FONTCACHE_FIRST_CHAR; right < FONTCACHE_FIRST_CHAR + FONTCACHE_CHAR_COUNT; ++right) {
            std::int16_t amount_8_8 = 0;
            float amount = (float)stbtt_GetCodepointKernAdvance(&font_info, (int)left, (int)right) * font_scale;

            if (!FloatToFixed8_8(amount, &amount_8_8)) {
                std::cerr << "kerning overflow while processing " << font_path << '\n';
                return false;
            }

            if (amount_8_8 != 0) {
                FontCacheKerningPair pair = {};

                pair.left_char = (std::uint8_t)left;
                pair.right_char = (std::uint8_t)right;
                pair.amount_8_8 = amount_8_8;
                kern_pairs.push_back(pair);
            }
        }
    }

    for (unsigned int ch = FONTCACHE_FIRST_CHAR; ch < FONTCACHE_FIRST_CHAR + FONTCACHE_CHAR_COUNT; ++ch) {
        FontCacheGlyph &glyph = glyphs[ch - FONTCACHE_FIRST_CHAR];
        unsigned char *bitmap = nullptr;
        int width = 0;
        int height = 0;
        int xoff = 0;
        int yoff = 0;
        int advance = 0;
        int lsb = 0;
        std::int16_t xoff_8_8 = 0;
        std::int16_t yoff_8_8 = 0;
        std::int16_t advance_8_8 = 0;

        stbtt_GetCodepointHMetrics(&font_info, (int)ch, &advance, &lsb);
        (void)lsb;

        bitmap = stbtt_GetCodepointSDF(
            &font_info,
            font_scale,
            (int)ch,
            (int)FONTCACHE_SDF_PADDING,
            (unsigned char)FONTCACHE_SDF_ON_EDGE_VALUE,
            FONTCACHE_SDF_PIXEL_DIST_SCALE,
            &width,
            &height,
            &xoff,
            &yoff);

        if (!FloatToFixed8_8((float)xoff, &xoff_8_8) ||
                !FloatToFixed8_8((float)yoff, &yoff_8_8) ||
                !FloatToFixed8_8((float)advance * font_scale, &advance_8_8)) {
            if (bitmap != nullptr) {
                stbtt_FreeSDF(bitmap, nullptr);
            }
            std::cerr << "glyph metric overflow while processing " << font_path << '\n';
            return false;
        }

        glyph.xoff_8_8 = xoff_8_8;
        glyph.yoff_8_8 = yoff_8_8;
        glyph.advance_8_8 = advance_8_8;

        if (bitmap == nullptr || width <= 0 || height <= 0) {
            if (bitmap != nullptr) {
                stbtt_FreeSDF(bitmap, nullptr);
            }
            continue;
        }

        if (pen_x + width > (int)FONTCACHE_ATLAS_WIDTH) {
            pen_x = 0;
            pen_y += row_h + (int)FONTCACHE_ATLAS_GAP;
            row_h = 0;
        }

        if (pen_y + height > (int)FONTCACHE_ATLAS_HEIGHT) {
            stbtt_FreeSDF(bitmap, nullptr);
            std::cerr << "atlas overflow while processing " << font_path << '\n';
            return false;
        }

        if ((std::size_t)width > 0xFFFFu || (std::size_t)height > 0xFFFFu ||
                (std::size_t)pen_x > 0xFFFFu || (std::size_t)pen_y > 0xFFFFu) {
            stbtt_FreeSDF(bitmap, nullptr);
            std::cerr << "glyph bounds overflow while processing " << font_path << '\n';
            return false;
        }

        glyph.atlas_x = (std::uint16_t)pen_x;
        glyph.atlas_y = (std::uint16_t)pen_y;
        glyph.width = (std::uint16_t)width;
        glyph.height = (std::uint16_t)height;
        glyph.bitmap_offset = (std::uint32_t)bitmap_data.size();
        EncodeGlyphBitmap(bitmap, (std::size_t)width * (std::size_t)height, &bitmap_data);
        glyph.bitmap_size = (std::uint32_t)(bitmap_data.size() - (std::size_t)glyph.bitmap_offset);

        pen_x += width + (int)FONTCACHE_ATLAS_GAP;
        row_h = std::max(row_h, height);
        stbtt_FreeSDF(bitmap, nullptr);
    }

    if (bitmap_data.size() > 0xFFFFFFFFu || kern_pairs.size() > 0xFFFFFFFFu) {
        std::cerr << "cache data is too large for " << font_path << '\n';
        return false;
    }

    {
        FontCacheHeader header = {};
        std::size_t glyph_bytes = glyphs.size() * sizeof(FontCacheGlyph);
        std::size_t kern_pair_bytes = kern_pairs.size() * sizeof(FontCacheKerningPair);
        std::size_t total_size = sizeof(FontCacheHeader) + glyph_bytes + kern_pair_bytes + bitmap_data.size();

        header.magic = FONTCACHE_MAGIC;
        header.version = FONTCACHE_VERSION;
        header.atlas_width = FONTCACHE_ATLAS_WIDTH;
        header.atlas_height = FONTCACHE_ATLAS_HEIGHT;
        header.first_char = FONTCACHE_FIRST_CHAR;
        header.char_count = FONTCACHE_CHAR_COUNT;
        header.glyph_count = (std::uint32_t)glyphs.size();
        header.kern_pair_count = (std::uint32_t)kern_pairs.size();
        header.bitmap_codec = FONTCACHE_BITMAP_CODEC_ZERO_RLE;
        header.bitmap_data_size = (std::uint32_t)bitmap_data.size();
        header.sdf_padding = FONTCACHE_SDF_PADDING;
        header.sdf_on_edge_value = FONTCACHE_SDF_ON_EDGE_VALUE;
        header.atlas_gap = FONTCACHE_ATLAS_GAP;
        header.atlas_base_px = FONTCACHE_ATLAS_BASE_PX;
        header.sdf_pixel_dist_scale = FONTCACHE_SDF_PIXEL_DIST_SCALE;
        header.font_scale = font_scale;
        header.font_ascent = font_ascent;
        header.font_line_advance = font_line_advance;

        cache_file.resize(total_size);
        std::memcpy(cache_file.data(), &header, sizeof(header));
        std::memcpy(cache_file.data() + sizeof(header), glyphs.data(), glyph_bytes);
        std::memcpy(cache_file.data() + sizeof(header) + glyph_bytes, kern_pairs.data(), kern_pair_bytes);
        std::memcpy(cache_file.data() + sizeof(header) + glyph_bytes + kern_pair_bytes, bitmap_data.data(), bitmap_data.size());
    }

    if (!WriteFile(cache_path.c_str(), cache_file.data(), cache_file.size())) {
        return false;
    }

    std::cout << "generated " << cache_path << '\n';
    return true;
}

int main(void)
{
    std::vector<std::string> fonts;
    bool ok = true;

    CollectFonts("*.ttf", &fonts);
    std::sort(fonts.begin(), fonts.end());
    fonts.erase(std::unique(fonts.begin(), fonts.end()), fonts.end());

    if (fonts.empty()) {
        std::cerr << "no .ttf files found in current directory\n";
        return 1;
    }

    for (const std::string &font_path : fonts) {
        if (!BuildFontCache(font_path)) {
            ok = false;
        }
    }

    return ok ? 0 : 1;
}
