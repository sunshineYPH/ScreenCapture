#include "png_io.h"

#include <X11/Xutil.h>
#include <cstdint>
#include <vector>
#include <zlib.h>

namespace {

void write_png_chunk(std::vector<uint8_t>& png, const char* type, const uint8_t* data, uint32_t length) {
    // Reserve space upfront to avoid reallocations
    size_t total_size = 4 + 4 + length + 4;
    png.reserve(png.size() + total_size);

    // Length (big-endian)
    png.push_back((length >> 24) & 0xFF);
    png.push_back((length >> 16) & 0xFF);
    png.push_back((length >> 8) & 0xFF);
    png.push_back(length & 0xFF);

    // Type
    png.push_back(type[0]);
    png.push_back(type[1]);
    png.push_back(type[2]);
    png.push_back(type[3]);

    // Data - batch copy
    if (data && length > 0) {
        png.insert(png.end(), data, data + length);
    }

    // CRC32 (over type + data) - compute directly without extra allocation
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const uint8_t*>(type), 4);
    if (data && length > 0) {
        crc = crc32(crc, data, length);
    }

    png.push_back((crc >> 24) & 0xFF);
    png.push_back((crc >> 16) & 0xFF);
    png.push_back((crc >> 8) & 0xFF);
    png.push_back(crc & 0xFF);
}

} // namespace

std::vector<unsigned char> encodePNG(XImage* img, int width, int height) {
    std::vector<uint8_t> png;

    // PNG signature
    uint8_t signature[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    for (int i = 0; i < 8; i++) {
        png.push_back(signature[i]);
    }

    // IHDR chunk
    uint8_t ihdr[13];
    ihdr[0] = (width >> 24) & 0xFF;
    ihdr[1] = (width >> 16) & 0xFF;
    ihdr[2] = (width >> 8) & 0xFF;
    ihdr[3] = width & 0xFF;
    ihdr[4] = (height >> 24) & 0xFF;
    ihdr[5] = (height >> 16) & 0xFF;
    ihdr[6] = (height >> 8) & 0xFF;
    ihdr[7] = height & 0xFF;
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 2;  // color type: RGB
    ihdr[10] = 0; // compression method
    ihdr[11] = 0; // filter method
    ihdr[12] = 0; // interlace method
    write_png_chunk(png, "IHDR", ihdr, 13);

    // Prepare raw image data with filter bytes
    // Each row: filter_byte (0 = None) + RGB data
    size_t row_size = width * 3;
    size_t raw_size = height * (1 + row_size);
    std::vector<uint8_t> raw_data(raw_size);

    for (int y = 0; y < height; y++) {
        size_t offset = y * (1 + row_size);
        raw_data[offset] = 0; // filter type: None

        for (int x = 0; x < width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            raw_data[offset + 1 + x * 3] = (pixel >> 16) & 0xFF;     // R
            raw_data[offset + 1 + x * 3 + 1] = (pixel >> 8) & 0xFF;  // G
            raw_data[offset + 1 + x * 3 + 2] = pixel & 0xFF;         // B
        }
    }

    // IDAT chunk with zlib compression
    std::vector<uint8_t> idat_data;

    // Allocate output buffer (reserve extra for zlib overhead)
    size_t comp_buf_size = raw_size + 1024;
    std::vector<uint8_t> comp_buf(comp_buf_size);

    // Use compress2() for simple zlib compression
    uLongf dest_len = comp_buf_size;
    if (compress2(comp_buf.data(), &dest_len, raw_data.data(), raw_size, Z_DEFAULT_COMPRESSION) == Z_OK) {
        // compress2 already includes zlib header and Adler32 checksum
        idat_data.assign(comp_buf.begin(), comp_buf.begin() + dest_len);
    } else {
        // Fallback to uncompressed if zlib fails
        idat_data.push_back(0x78);
        idat_data.push_back(0x01);
        idat_data.insert(idat_data.end(), raw_data.begin(), raw_data.end());
        uint32_t adler = adler32(1L, raw_data.data(), raw_size);
        idat_data.push_back((adler >> 24) & 0xFF);
        idat_data.push_back((adler >> 16) & 0xFF);
        idat_data.push_back((adler >> 8) & 0xFF);
        idat_data.push_back(adler & 0xFF);
    }

    write_png_chunk(png, "IDAT", idat_data.data(), idat_data.size());

    // IEND chunk
    write_png_chunk(png, "IEND", nullptr, 0);

    return png;
}

// ---------- PNG decoder (libpng) ----------
//
// The hand-rolled encoder above is fine for the project's main path
// (writing the screenshot). But reading PNG icon assets is a different
// problem — we need to handle all 8-bit color types (gray, gray+alpha,
// RGB, RGBA, palette) and the 5 PNG filter modes. Re-deriving all of
// that would be hundreds of lines and easy to break on interlaced or
// 16-bit files. libpng is on every Linux desktop, so we use it for
// reading only.

#include <png.h>
#include <cstdio>
#include <cstdlib>

DecodedPNG decodePNGFile(const std::string& path) {
    DecodedPNG out;

    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return out;

    png_structp png = png_create_read_struct(
        PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) { std::fclose(fp); return out; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return out;
    }

    // libpng's longjmp-on-error requires this setup. We use it to
    // return cleanly instead of unwinding through C++ destructors.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return out;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int w = static_cast<int>(png_get_image_width(png, info));
    int h = static_cast<int>(png_get_image_height(png, info));
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth  = png_get_bit_depth(png, info);

    // Normalize the format to 8-bit RGBA so the rest of the pipeline
    // can assume a single layout. Handles palette images, 16-bit
    // channels, and grayscale/gray-alpha sources.
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png);
    }
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);

    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    // libpng wants row pointers into caller-owned memory. We supply
    // pointers into the output vector; the bytes-per-row must match
    // what we told libpng in update_info (RGBA = w*4).
    std::vector<png_bytep> rows(h);
    for (int y = 0; y < h; y++) {
        rows[y] = out.rgba.data() + static_cast<size_t>(y) * w * 4;
    }
    png_read_image(png, rows.data());

    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return out;
}
