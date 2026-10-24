#ifndef MONTAGE_IO_CODECS_CODEC_TABLE_H
#define MONTAGE_IO_CODECS_CODEC_TABLE_H

// M25 能力表 + 格式嗅探 + 编解码统一入口（07 §M25：格式×读×写×深度单一事实来源）。
// bridge getCodecCapabilities / openFileFromFd 分发 / exportImage 分发全部由此驱动。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

// 能力表（单一事实来源；顺序 = UI 呈现顺序）
const std::vector<CodecCap>& capabilities();

// 内容嗅探：魔数 → 格式名（"tiff"/"bigtiff"/"exr"/"hdr"/"gif"/"qoi"/"bmp"/"tga"）。
// TGA 无可靠魔数：footer（TRUEVISION-XFILE）存在才命中；fileName 扩展名兜底
// （.tga/.vda/.icb/.pic——仅当魔数嗅探全落空时使用）。
// 命中返回 true 并写 fmt；未命中返回 false（走系统解码器）。
bool sniff(const uint8_t* data, size_t len, const std::string& fileName, std::string& fmt);

// 扩展名 → 格式名（"" = 未知；不含点，大小写不敏感）
std::string formatFromExt(const std::string& ext);

// 平面图解码（合成图；多帧取第一帧/主 IFD）。fmt = sniff/formatFromExt 结果。
bool decode(const uint8_t* data, size_t len, const std::string& fmt, CodecImage& out,
            std::string& err);

// 分层解码（v1 仅 TIFF ImageSourceData 8-bit Layr；返回 bottom→top 层记录 + 文档尺寸）。
// 无分层时返回 false 且 err="no layers"（调用方回退 decode）。
bool decodeLayers(const uint8_t* data, size_t len, const std::string& fmt, uint32_t& w, uint32_t& h,
                  std::vector<CodecLayerRec>& layers, std::vector<std::string>& notes,
                  std::string& err);

// 编码（rgba 直 alpha；quality 保留参数，v1 各格式忽略或用于位深开关）。
bool encode(const std::string& fmt, const uint8_t* rgba, uint32_t w, uint32_t h, int quality,
            std::vector<uint8_t>& out, std::string& err);

// 格式可写检查（能力表驱动；exportImage 前置校验）
bool canExport(const std::string& fmt);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_CODEC_TABLE_H
