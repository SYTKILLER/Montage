#ifndef MONTAGE_IO_PNG_DECODE_H
#define MONTAGE_IO_PNG_DECODE_H

// 自研 PNG 解码（M4c）：工程包资产必须确定性读取（直 RGBA、无字节序/预乘猜谜——
// 系统解码器在这两条上均不可信，见 M4b/M4c 实证）。支持 8-bit、无隔行、
// colorType 0/2/4/6（灰度/RGB/灰+A/RGBA）；色板(3)/16-bit/隔行明确报错。

#include <cstdint>
#include <memory>
#include <string>

#include "engine/document.h"

namespace montage {
namespace io {

// 解码 PNG 字节为文档瓦片网格（直 alpha RGBA8888）；失败返回 nullptr 并带 err。
std::shared_ptr<const TileGrid> decodePngToGrid(const uint8_t* data, size_t size, std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_PNG_DECODE_H
