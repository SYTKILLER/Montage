#ifndef MONTAGE_IO_CODECS_EXR_CODEC_H
#define MONTAGE_IO_CODECS_EXR_CODEC_H

// OpenEXR 读取（扫描线单部件：NONE/RLE/ZIPS/ZIP 压缩；half/float 通道 → 线性 → sRGB8）。
// tiled/multipart/deep/PIZ/PXR24/B44/DWAA 族明确报错（07 §M25：EXR 读）。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

bool exrDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_EXR_CODEC_H
