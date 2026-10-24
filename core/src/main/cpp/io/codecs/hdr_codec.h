#ifndef MONTAGE_IO_CODECS_HDR_CODEC_H
#define MONTAGE_IO_CODECS_HDR_CODEC_H

// Radiance HDR（.hdr/.pic）读取（#?RADIANCE/#?RGBE；FORMAT=32-bit_rle_rgbe；
// 新式自适应 RLE + 旧式标准 RLE + 裸流；RGBE→线性→sRGB8）。只读（07 §M25 范围）。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

bool hdrDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_HDR_CODEC_H
