#ifndef MONTAGE_IO_CMS_GAMUT_H
#define MONTAGE_IO_CMS_GAMUT_H

// M22 色彩管理内核 · 对打样（输出）profile 的色域检查：PS「视图 › 色域警告」。
// 翻译自 photocraft crates/cms/src/gamut.rs（MIT OR Apache-2.0）。

#include <optional>
#include <string>

#include "io/cms/pipeline.h"
#include "io/cms/profile.h"

namespace montage {
namespace cms {

// 判定域外的默认 ΔE76 阈值。
inline constexpr float kGamutDefaultThreshold = 4.0f;

// src 颜色对 proof 的预计算域外测试。
// 颜色送入打样设备（相对色度、裁剪）再回 Lab，移动量超过阈值（ΔE76）即域外。
// ΔE 在源设备空间上制表（3 通道 33³ 网格、4 通道 17⁴，其余精确）。
class GamutCheck {
  public:
    GamutCheck() = default;
    static std::optional<GamutCheck> make(const Profile& src, const Profile& proof, float threshold,
                                          std::string& err);

    size_t inputs() const { return inputs_; }

    // 颜色与其打样往返的 ΔE76。
    float deltaE(const float* px) const;

    bool outOfGamut(const float* px) const { return deltaE(px) > threshold_; }

    float threshold() const { return threshold_; }

  private:
    size_t inputs_ = 0;
    std::optional<Clut> grid_;
    Pipeline direct_{};
    Pipeline via_{};
    float threshold_ = kGamutDefaultThreshold;
};

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_GAMUT_H
