#ifndef MONTAGE_IO_CODECS_TGA_CODEC_H
#define MONTAGE_IO_CODECS_TGA_CODEC_H

// TGA 编解码（Truevision TARGA v2：type 1/2/3/9/10/11，15/16/24/32bpp，
// RLE + 裸流，colormap，origin 翻转，footer）。写出 = RLE 24/32/8。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

bool tgaDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);
// withAlpha=false 写 24bpp；true 写 32bpp（含 8 位属性=alpha）
bool tgaEncode(const uint8_t* rgba, uint32_t w, uint32_t h, bool withAlpha,
               std::vector<uint8_t>& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_TGA_CODEC_H
