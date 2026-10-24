#ifndef MONTAGE_IO_CODECS_GIF_CODEC_H
#define MONTAGE_IO_CODECS_GIF_CODEC_H

// GIF 编解码（GIF87a/89a：LZW、GCT/LCT、interlace、GCE 透明）。v1 读首帧/写单帧。
// 量化 = codec_util medianCut + Floyd–Steinberg（可关）。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

bool gifDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);

// dither=false 关闭 FS 抖动；maxColors ≤ 256（透明占用一个索引）
bool gifEncode(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors, bool dither,
               std::vector<uint8_t>& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_GIF_CODEC_H
