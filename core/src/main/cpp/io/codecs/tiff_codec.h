#ifndef MONTAGE_IO_CODECS_TIFF_CODEC_H
#define MONTAGE_IO_CODECS_TIFF_CODEC_H

// TIFF/BigTIFF 编解码（07 §M25：读 Photoshop 私有 tag；分层写出延后池）。
// 读：经典/BigTIFF、双字节序、条带/瓦片、无压缩/LZW/PackBits/Deflate、
//     1/2/4/8/16/32f bit、灰度/RGB(A)/调色板、水平差值预测器、planar、
//     ImageSourceData(37724) 8-bit Layr 分层（合成 PSD 容器喂 psd_reader）。
// 写：经典 TIFF（8/16bit RGB(A)/灰度，Deflate/无压缩）——>4GB BigTIFF 不需要（100MP 上限）。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"
#include "io/psd/psd_reader.h"

namespace montage {
namespace io {
namespace codecs {

bool tiffDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);

// bitDepth: 8 | 16；compression: "none" | "deflate" | "lzw"；alpha 存在且 <255 任一 → RGBA
bool tiffEncode(const uint8_t* rgba, uint32_t w, uint32_t h, int bitDepth, bool withAlpha,
                const std::string& compression, std::vector<uint8_t>& out, std::string& err);

// 是否携带 Photoshop 分层 tag（37724 含 Layr/Lr16/Lr32 块）
bool tiffHasLayerData(const uint8_t* data, size_t len);

// 分层解码（bottom→top；rgba 为预乘——psd_reader 语义，导入端 unpremultiply）。
// pdocOut 非 null 时输出完整 PsdDocument（psd_import::assemblePsdLayers 组装复用：
// 组嵌套/蒙版/混合键全走 PSD 通路）。
// 16/32-bit 分层（Lr16/Lr32）与小端（MIB8）v1 拒绝 → notes 提示、调用方回退合成图。
bool tiffDecodeLayers(const uint8_t* data, size_t len, uint32_t& w, uint32_t& h,
                      std::vector<CodecLayerRec>& layers, std::vector<std::string>& notes,
                      std::string& err, psd::PsdDocument* pdocOut = nullptr);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_TIFF_CODEC_H
