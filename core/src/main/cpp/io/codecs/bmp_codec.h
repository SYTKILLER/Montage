#ifndef MONTAGE_IO_CODECS_BMP_CODEC_H
#define MONTAGE_IO_CODECS_BMP_CODEC_H

// BMP 编解码（Windows BMP/DIB：BITMAPCORE/INFO/V4/V5 头，1/4/8/16/24/32bpp，
// BI_RGB/RLE4/RLE8/BITFIELDS）。写出 = 32bpp V4（含 alpha）或 24bpp。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

bool bmpDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);
bool bmpEncode(const uint8_t* rgba, uint32_t w, uint32_t h, bool withAlpha,
               std::vector<uint8_t>& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_BMP_CODEC_H
