#ifndef MONTAGE_IO_CODECS_CODEC_UTIL_H
#define MONTAGE_IO_CODECS_CODEC_UTIL_H

// M25 平面格式编解码公共工具（07 §M25：能力表模式——格式×读×写×深度单一事实来源）。
// 全部解码产物 = CodecImage（RGBA8 直 alpha，引擎瓦片约定）；编码输入同构。
// 不触碰 Engine/Document（纯字节域），宿主机可直测。

#include <cstdint>
#include <string>
#include <vector>

namespace montage {
namespace io {
namespace codecs {

// 解码产物：RGBA8 直 alpha（行优先，w*h*4）
struct CodecImage {
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba;
};

// 能力表条目（单一事实来源；getCodecCapabilities 桥接与 TS 打开/导出对话框由此驱动）
struct CodecCap {
    const char* format;   // 规范名（exportImage/openFileFromFd 分发键）
    bool read;
    bool write;
    const char* depths;   // 可表示位深摘要（读=输入域，写=输出域）
    const char* exts;     // 分号分隔扩展名（小写，含点）
    const char* note;     // 边界说明（UI 悬浮/日志用）
};

// 解码分层产物（TIFF ImageSourceData 等）：记录为 bottom→top 序（组装序与 PSD 同语义）
struct CodecLayerRec {
    int left = 0;
    int top = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> rgba;  // 预乘由来源定：PSD 语义为预乘（psd_reader 直出），
                                // TIFF 层复用 psd_reader 直出，导入端统一 unpremultiply
    std::string name;
    bool isGroup = false;
    double opacity = 1.0;
    std::string blendKey = "norm";
    bool visible = true;
};

// ---------- 色彩/数值工具 ----------

// half（IEEE 754 binary16）→ float
float halfToFloat(uint16_t h);

// 线性 [0,1] → sRGB 编码 [0,255]（四舍五入）
uint8_t linearToSrgb8(float v);

// RGBE（Radiance）→ 线性 RGB（ldexp 语义，e+128 生成）
void rgbeToLinear(uint8_t r, uint8_t g, uint8_t b, uint8_t e, float out[3]);

// ---------- 量化（GIF 写出） ----------

// 中位切分量化：输入 RGBA（跳过 a==0 像素），maxColors ≤ 256。
// paletteOut = RGB 三元组（≤ maxColors*3）；indexOut = 每像素调色板号（透明像素索引
// 由调用方处理——quantizeImage 提供透明索引语义）。
void medianCutPalette(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors,
                      std::vector<uint8_t>& paletteOut, std::vector<uint8_t>& indexOut);

// 带 Floyd–Steinberg 抖动的量化（dither=false 时取最近色）。
// needTransparency=true 且图中存在 alpha<128 像素时：透明像素映射到 paletteOut 追加的
// 专用透明索引（GIF89a GCE transparent flag 用），返回值 = 透明索引（无透明像素返回 -1）。
int quantizeImage(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors, bool dither,
                  std::vector<uint8_t>& paletteOut, std::vector<uint8_t>& indexOut);

// ---------- 简单缓冲工具 ----------

// 最近邻重采样 RGBA（BMP/TGA 行对齐不涉及，EXR dataWindow 裁剪用不到——保留给未来）
void copyRegion(const uint8_t* src, uint32_t srcW, uint32_t srcH,
                uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_CODEC_UTIL_H
