#ifndef MONTAGE_IO_IMAGE_EXPORT_H
#define MONTAGE_IO_IMAGE_EXPORT_H

// M7.5 导出管线（06 规划 §4）：CPU 扁平化内核 + PNG 直写。
// 扁平化与 GL 管线逐公式同构（blend 24 模式 = gen_shaders.py BODIES 直译；
// 剪贴 = alpha × 基 alpha（kClipFrag）；调整层 = 256 LUT 全帧（kAdjustFrag + buildAdjustmentLut）。
// accum 为 8-bit 直 alpha（每层一次舍入，B-1 对拍容差登记）。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace io {

// 锁外扁平化输入（docMutex 内构建快照，避免计算持锁）。
struct FlatLayer {
    BlendMode blend = BlendMode::Normal;
    double opacity = 1.0;
    bool clipping = false;
    bool visible = true;
    bool isAdjustment = false;
    LayerAdjustment adjustment;                        // isAdjustment 时有效（值拷贝）
    std::shared_ptr<const TileGrid> pixels;            // effectivePixels（蒙版已合成）；调整层为 null
    std::shared_ptr<const TileGrid> clipBasePixels;    // 剪贴基 effectivePixels（非剪贴层为 null）
    double originX = 0.0;
    double originY = 0.0;
    double clipBaseOriginX = 0.0;                      // 剪贴基自身落点（网格局部→文档坐标）
    double clipBaseOriginY = 0.0;
};

struct FlattenJob {
    uint32_t docWidth = 0;
    uint32_t docHeight = 0;
    std::vector<FlatLayer> layers;  // bottom → top，仅可见层
};

// docMutex 内构建扁平化快照（region = 文档坐标矩形；全文档传 0/0/0/0 → 自动取全图）
FlattenJob BuildFlattenJob(const Document& doc, int regionX, int regionY, int regionW, int regionH);

struct FlattenResult {
    bool ok = false;
    std::string error;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // width*height*4，直 alpha
};

// CPU 扁平化（锁外执行；job 来自 BuildFlattenJob）
FlattenResult FlattenVisible(const FlattenJob& job, int regionX, int regionY, int regionW, int regionH);

// RGBA 流式 PNG 直写 fd（filter 0 + 多 IDAT；成功返回总字节数）
bool WritePngToFd(int fd, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                  uint64_t* bytesOut, std::string* err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_IMAGE_EXPORT_H
