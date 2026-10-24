#ifndef MONTAGE_IO_DNG_DECODE_H
#define MONTAGE_IO_DNG_DECODE_H

// M23a RAW 显影门面（04 §D4.4 / 07 §3.15 M23）：解码与显影内核在 io/raw/
// （photocraft raw crate C++ 翻译：DNG 全量/CR2/NEF/ARW/RW2/ORF/RAF +
// demosaic 三算法+X-Trans + 显影链 ProPhoto），本层负责参数语义（M15e 面板）
// 与发布（ProPhoto → sRGB → TileGrid）。
// 能力边界（不支持给明确错误）：CR3、PEF、JPEG XL 瓦片、有损 JPEG 瓦片、
// Nikon "lossy after split"、Fuji 压缩、Panasonic RawFormat 4。
// 注：显影参数全部自持，不依赖系统 RawDescriptor。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/camera_raw.h"
#include "engine/document.h"

namespace montage {
namespace io {

// 内容探测：相机 raw（DNG/CR2/NEF/ARW/RW2/ORF/RAF…，廉价头与 IFD 检查）。
bool looksLikeCameraRaw(const std::vector<uint8_t>& bytes);

// M23c：内嵌预览 JPEG → 交错 RGB8 的解码钩子（由 image_import 初始化时注入
// 系统解码器实现；宿主测试不注入 = fit_look 参考不可得，保持中性回退）。
using PreviewDecodeFn = bool (*)(const uint8_t* jpeg, size_t len, uint32_t& w, uint32_t& h,
                                 std::vector<uint8_t>& rgb);
void setPreviewDecodeHook(PreviewDecodeFn fn);

// M15e 显影参数（相对默认显影的偏移；显影参数面板）
struct DevelopParams {
    float temp = 0.0f;   // -100..100 色温偏移（正=暖/偏黄，负=冷/偏蓝；调制 R/B 增益 ±30%）
    float tint = 0.0f;   // -100..100 色调偏移（正=品红，负=绿；调制 G 增益）
    float ev = 0.0f;     // -2..+2 曝光（线性增益 2^ev，高光照常截断）
    float gamma = 1.0f;  // 0.5..2.0 伽马（sRGB 编码后再取 1/γ 次幂；>1 提亮中间调）
    // M23b：Camera Raw 9 级链（settingsJson 非空时启用；photocraft camera_raw 语义，
    // JSON = CameraRaw 字段 camelCase 串——basic/曲线/HSL/分级/细节/效果全量）。
    std::string cameraJson;
};

// 显影 → 直 RGBA TileGrid（引擎瓦片约定）。失败给明确错误。
bool developDngToGrid(const std::vector<uint8_t>& bytes,
                      const DevelopParams& params,
                      std::shared_ptr<const TileGrid>& out, uint32_t& outW, uint32_t& outH,
                      std::string& err);

// 默认参数显影（导入路径语义不变）
inline bool developDngToGrid(const std::vector<uint8_t>& bytes,
                             std::shared_ptr<const TileGrid>& out, uint32_t& outW, uint32_t& outH,
                             std::string& err) {
    return developDngToGrid(bytes, DevelopParams{}, out, outW, outH, err);
}

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_DNG_DECODE_H
