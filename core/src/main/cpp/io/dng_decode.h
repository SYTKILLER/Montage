#ifndef MONTAGE_IO_DNG_DECODE_H
#define MONTAGE_IO_DNG_DECODE_H

// M15d DNG RAW 显影链 v1（04 §D4.4）：自研 TIFF/DNG 容器解析 + 显影核。
// 能力边界：未压缩（Compression=1）16-bit CFA（PhotometricInterpretation=32803）、
// 单/多 strip、CFA 2×2（RGGB/GBRG/GRBG/BGGR）、黑电平（标量或 2×2 取均值）、
// 白电平、AsShotNeutral 白平衡、双线性去马赛克、sRGB 伽马。
// 不支持（给明确错误）：JPEG/deflate 压缩、tiled、12-bit 打包、非线性码片。
// 注：规划原文的 createImageRawData 路线改为自研解析——显影参数全部自持，不依赖
// 系统 RawDescriptor（v1 简化登记）。

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace io {

// 内容探测：TIFF 魔数 + DNGVersion(50706) 标签存在
bool looksLikeDng(const std::vector<uint8_t>& bytes);

// 显影 → 直 RGBA TileGrid（引擎瓦片约定）。失败给明确错误。
bool developDngToGrid(const std::vector<uint8_t>& bytes,
                      std::shared_ptr<const TileGrid>& out, uint32_t& outW, uint32_t& outH,
                      std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_DNG_DECODE_H
