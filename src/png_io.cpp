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
