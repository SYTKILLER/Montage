#ifndef MONTAGE_ENGINE_LENS_OPS_H
#define MONTAGE_ENGINE_LENS_OPS_H

// M20d 镜头校正（photocraft crates/algo/src/lens.rs C++ 翻译，MIT OR Apache-2.0）。
// Brown–Conrady 径向 k1/k2 + 切向 p1/p2、横向色差（红蓝相对绿径向缩放）、暗角
// （1+v1 r²+v2 r⁴+v3 r⁶ 除出 + Midpoint smoothstep 曝光坡道）、垂直/水平透视 keystone
// 单应逆、角度/缩放；逆映射逐像素双三次预乘采样，1px 软边 + 三种边缘模式。
// Montage v1：RGB(A) 8 位稠密图（CMYK/Lab/位深泛化留 M22）。

#include "engine/inpaint_ops.h"  // RectI

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace montage {
using inpaint::RectI;

namespace lens {

struct Img {
    std::vector<float> px;  // w*h*4 RGBA
    int w = 0;
    int h = 0;
};

enum class EdgeMode : int32_t { Extension = 0, Transparency = 1, Color = 2 };

// 镜头 profile（畸变/暗角/横向 CA 系数）
struct Profile {
    std::string name;
    double k1 = 0.0;
    double k2 = 0.0;
    double p1 = 0.0;
    double p2 = 0.0;
    double vignette[3] = {0.0, 0.0, 0.0};  // 1 + v1 r² + v2 r⁴ + v3 r⁶
    double caRed = 0.0;
    double caBlue = 0.0;
};

// 35mm 等效焦距通用 profile（对数焦距插值：广角桶形畸变+强暗角，长焦轻枕形）
Profile genericProfile(double focal35mm);

// PS 对话框单位的校正参数
struct Correction {
    bool hasProfile = false;
    Profile profile;
    bool correctDistortion = true;
    bool correctVignette = true;
    bool correctCa = true;
    double distortion = 0.0;        // Remove Distortion −100..100
    double redCyan = 0.0;           // −100..100
    double blueYellow = 0.0;        // −100..100
    double vignetteAmount = 0.0;    // −100..100（正=提亮角部）
    double vignetteMidpoint = 50.0; // 0..100
    double vertical = 0.0;          // −100..100
    double horizontal = 0.0;        // −100..100
    double angle = 0.0;             // 度（正=顺时针）
    double scale = 100.0;           // 50..150%
    EdgeMode edge = EdgeMode::Transparency;
    double edgeColor[4] = {0.0, 0.0, 0.0, 1.0};
};

// 输出像素采样位置：绿/alpha 在 p，红在 pr，蓝在 pb；颜色乘 gain
struct Sample {
    double p[2] = {0.0, 0.0};
    double pr[2] = {0.0, 0.0};
    double pb[2] = {0.0, 0.0};
    float gain = 1.0f;
};

// LensCorrection 的逆映射
class LensMap {
   public:
    LensMap() = default;
    LensMap(const Correction& lc, const RectI& frame);
    // 输出 px → 采样处（keystone 送往无穷时 false）
    bool sample(double x, double y, Sample& out) const;

   private:
    double c_[2] = {0.0, 0.0};
    double r_ = 1.0;
    double k1_ = 0.0, k2_ = 0.0, p1_ = 0.0, p2_ = 0.0;
    double caR_ = 0.0, caB_ = 0.0;
    double vigProfile_[3] = {0.0, 0.0, 0.0};
    double vigAmount_ = 0.0;
    double vigMid_ = 0.5;
    double hinv_[9] = {1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0};
    double sinA_ = 0.0, cosA_ = 1.0;
    double scale_ = 1.0;
};

// 校正后全帧仍被覆盖的最小 scale%（Auto Scale）
double autoScale(const Correction& lc, const RectI& frame);

// 让 a→b 线水平/垂直（就近）的角度（度，Straighten 工具）
double straightenAngle(const double a[2], const double b[2]);

// 应用校正（frame 内；edge 决定未覆盖处填充）
Img correct(const Img& src, const RectI& frame, const Correction& lc);

}  // namespace lens

}  // namespace montage

#endif  // MONTAGE_ENGINE_LENS_OPS_H
