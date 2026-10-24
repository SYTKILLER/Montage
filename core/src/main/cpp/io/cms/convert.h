#ifndef MONTAGE_IO_CMS_CONVERT_H
#define MONTAGE_IO_CMS_CONVERT_H

// M22 色彩管理内核 · 文档颜色模型间转换。
// CMYK↔RGB 经 ICC 色彩管理模块：内置涂布 CMYK profile + sRGB（相对色度 + 黑点补偿，
// Photoshop 默认），CMYK 像素的显示与着色与色彩受管编辑器一致。文档内嵌 CMYK profile
// 在其会话空间激活期间（CmykScope）替换内置 profile；否则用内置。Lab 用精确 D50 公式；
// 灰视作 sGray（灰 v = sRGB (v,v,v)）。
// 翻译自 photocraft crates/color/src/convert.rs（MIT OR Apache-2.0）。

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace montage {
namespace cms {

class Transform;

// 文档自身的 CMYK profile，与 sRGB 双向链接（相对色度 + BPC，同内置默认）。
// 激活于线程（CmykScope）时，cmykToRgb/rgbToCmyk（合成器读 CMYK 层、CMYK 填充色）
// 用它而非内置涂布 CMYK。
class CmykSpace {
  public:
    // 文档内嵌 profile 字节的空间：无字节、非 CMYK profile、或即内置涂布 CMYK
    //（默认路径）时为空。按内容哈希缓存。
    static std::shared_ptr<const CmykSpace> forProfile(const std::vector<uint8_t>* icc);

    // CMYK → sRGB（相对色度 + BPC）。
    std::array<float, 3> toSrgb(const std::array<float, 4>& c) const;
    // sRGB → CMYK。
    std::array<float, 4> fromSrgb(const std::array<float, 3>& rgb) const;

    uint64_t id = 0;  // profile 内容哈希（转换像素的缓存键）

  private:
    std::shared_ptr<Transform> toSrgb_;
    std::shared_ptr<Transform> fromSrgb_;
};

// 会话级 CMYK 空间激活守卫（RAII；析构恢复外层——嵌套安全）。
class CmykScope {
  public:
    explicit CmykScope(const std::shared_ptr<const CmykSpace>& space);
    ~CmykScope();
    CmykScope(const CmykScope&) = delete;
    CmykScope& operator=(const CmykScope&) = delete;

  private:
    std::shared_ptr<const CmykSpace> prev_;
};

// 当前线程激活的 CMYK 空间（见 CmykScope）。
std::shared_ptr<const CmykSpace> activeCmykSpace();

// sRGB 传递函数：编码 → 线性。
inline float srgbToLinear(float v) {
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
// sRGB 传递函数：线性 → 编码。
inline float linearToSrgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// 无 profile 的朴素 CMYK → RGB：`(1−c)(1−k)`。参考与测试保留。
std::array<float, 3> cmykToRgbNaive(const std::array<float, 4>& c);

// 全 GCR 的朴素 RGB → CMYK。
std::array<float, 4> rgbToCmykNaive(const std::array<float, 3>& rgb);

// 色彩受管 CMYK → sRGB（激活的文档 CMYK profile，否则内置涂布 CMYK；相对色度 + BPC）。
std::array<float, 3> cmykToRgb(const std::array<float, 4>& c);

// 色彩受管 sRGB → CMYK（同上）。
std::array<float, 4> rgbToCmyk(const std::array<float, 3>& rgb);

// D50 白点（ICC PCS）。
inline constexpr std::array<float, 3> kD50F32 = {0.96422f, 1.0f, 0.82521f};

// 线性 sRGB → D50 XYZ（Bradford 适配，同 ICC sRGB profile）。行和 = kD50F32。
inline constexpr float kSrgbToXyzD50[3][3] = {
    {0.436074f, 0.385064f, 0.143080f},
    {0.222504f, 0.716878f, 0.060618f},
    {0.013932f, 0.097104f, 0.714173f},
};

// D50 XYZ → 线性 sRGB（kSrgbToXyzD50 的逆）。
inline constexpr float kXyzD50ToSrgb[3][3] = {
    {3.133856f, -1.616867f, -0.490615f},
    {-0.978768f, 1.916142f, 0.033454f},
    {0.071945f, -0.228991f, 1.405243f},
};

// CIE L*a*b*（D50）→ 编码 sRGB（Bradford 适配到 D65）。
std::array<float, 3> labToSrgb(const std::array<float, 3>& lab);

// 编码 sRGB → CIE L*a*b*（D50）。
std::array<float, 3> srgbToLab(const std::array<float, 3>& rgb);

// Rec.601 亮度（Photoshop 灰度转换默认）。
inline float rgbToGray(const std::array<float, 3>& rgb) {
    return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
}

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_CONVERT_H
