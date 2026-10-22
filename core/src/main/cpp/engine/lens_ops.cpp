// M20d 镜头校正 —— photocraft crates/algo/src/lens.rs C++ 翻译（MIT OR Apache-2.0）。

#include "engine/lens_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {

namespace {
std::array<float, 4> catmullRomF(float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return {-0.5f * t3 + t2 - 0.5f * t, 1.5f * t3 - 2.5f * t2 + 1.0f,
            -1.5f * t3 + 2.0f * t2 + 0.5f * t, 0.5f * t3 - 0.5f * t2};
}
}  // namespace

namespace lens {

Profile genericProfile(double focal35mm) {
    struct Row {
        double f, k1, k2, v1, v2;
    };
    constexpr Row kTable[7] = {
        {12.0, -0.16, 0.04, -0.60, 0.15},  {16.0, -0.11, 0.025, -0.50, 0.12},
        {24.0, -0.06, 0.012, -0.38, 0.08}, {35.0, -0.025, 0.004, -0.28, 0.05},
        {50.0, 0.0, 0.0, -0.22, 0.04},     {85.0, 0.012, 0.0, -0.16, 0.02},
        {200.0, 0.02, 0.0, -0.10, 0.01},
    };
    const double f = std::clamp(focal35mm, kTable[0].f, kTable[6].f);
    int k = 5;
    for (int i = 0; i < 6; ++i) {
        if (f <= kTable[i + 1].f) {
            k = i;
            break;
        }
    }
    const Row& a = kTable[k];
    const Row& b = kTable[k + 1];
    const double t = (std::log(f) - std::log(a.f)) / (std::log(b.f) - std::log(a.f));
    auto l = [&](double x, double y) { return x + (y - x) * t; };
    Profile p;
    p.name = "Generic Lens";
    p.k1 = l(a.k1, b.k1);
    p.k2 = l(a.k2, b.k2);
    p.vignette[0] = l(a.v1, b.v1);
    p.vignette[1] = l(a.v2, b.v2);
    p.vignette[2] = 0.0;
    return p;
}

LensMap::LensMap(const Correction& lc, const RectI& frame) {
    c_[0] = (static_cast<double>(frame.x0) + frame.x1) / 2.0;
    c_[1] = (static_cast<double>(frame.y0) + frame.y1) / 2.0;
    r_ = std::max(std::hypot(static_cast<double>(frame.width()), static_cast<double>(frame.height())), 2.0) / 2.0;
    const Profile& prof = lc.hasProfile ? lc.profile : Profile{};
    double pk1 = 0.0, pk2 = 0.0, pp1 = 0.0, pp2 = 0.0;
    if (lc.correctDistortion) {
        pk1 = prof.k1;
        pk2 = prof.k2;
        pp1 = prof.p1;
        pp2 = prof.p2;
    }
    const double g = -lc.vertical / 100.0 * 0.5;
    const double h = -lc.horizontal / 100.0 * 0.5;
    // [[1,0,0],[0,1,0],[h,g,1]] 的逆 = [[1,0,0],[0,1,0],[-h,-g,1]]
    hinv_[0] = 1.0; hinv_[1] = 0.0; hinv_[2] = 0.0;
    hinv_[3] = 0.0; hinv_[4] = 1.0; hinv_[5] = 0.0;
    hinv_[6] = -h;  hinv_[7] = -g;  hinv_[8] = 1.0;
    const double aRad = lc.angle * 3.14159265358979323846 / 180.0;
    k1_ = pk1 - lc.distortion / 100.0 * 0.25;
    k2_ = pk2;
    p1_ = pp1;
    p2_ = pp2;
    caR_ = (lc.correctCa ? prof.caRed : 0.0) - lc.redCyan * 4e-5;
    caB_ = (lc.correctCa ? prof.caBlue : 0.0) - lc.blueYellow * 4e-5;
    if (lc.correctVignette) {
        vigProfile_[0] = prof.vignette[0];
        vigProfile_[1] = prof.vignette[1];
        vigProfile_[2] = prof.vignette[2];
    }
    vigAmount_ = lc.vignetteAmount / 100.0;
    vigMid_ = std::clamp(lc.vignetteMidpoint, 0.0, 100.0) / 100.0;
    sinA_ = std::sin(aRad);
    cosA_ = std::cos(aRad);
    scale_ = std::clamp(lc.scale / 100.0, 0.05, 20.0);
}

bool LensMap::sample(double x, double y, Sample& out) const {
    // 撤销缩放与旋转（绕中心）
    const double dx = (x - c_[0]) / scale_;
    const double dy = (y - c_[1]) / scale_;
    const double rx = cosA_ * dx + sinA_ * dy;
    const double ry = -sinA_ * dx + cosA_ * dy;
    // 撤销 keystone
    const double nx = rx / r_;
    const double ny = ry / r_;
    const double w = hinv_[6] * nx + hinv_[7] * ny + hinv_[8];
    if (w <= 0.05) {
        return false;
    }
    const double ux = nx / w;
    const double uy = ny / w;
    // Brown–Conrady 畸变
    const double r2 = ux * ux + uy * uy;
    const double radial = 1.0 + k1_ * r2 + k2_ * r2 * r2;
    const double sx = ux * radial + 2.0 * p1_ * ux * uy + p2_ * (r2 + 2.0 * ux * ux);
    const double sy = uy * radial + p1_ * (r2 + 2.0 * uy * uy) + 2.0 * p2_ * ux * uy;
    auto toPx = [&](double k, double out2[2]) {
        out2[0] = c_[0] + sx * k * r_;
        out2[1] = c_[1] + sy * k * r_;
    };
    // 源半径上的暗角
    const double rs2 = sx * sx + sy * sy;
    const double falloff = std::max(1.0 + vigProfile_[0] * rs2 + vigProfile_[1] * rs2 * rs2 +
                                        vigProfile_[2] * rs2 * rs2 * rs2,
                                    0.05);
    const double start = (1.0 - vigMid_) * 0.9;
    const double t = std::clamp((std::sqrt(rs2) - start) / (1.0 - start + 1e-9), 0.0, 1.0);
    const double ramp = t * t * (3.0 - 2.0 * t);
    out.gain = static_cast<float>(std::pow(2.0, vigAmount_ * 1.5 * ramp) / falloff);
    toPx(1.0, out.p);
    toPx(1.0 + caR_, out.pr);
    toPx(1.0 + caB_, out.pb);
    return true;
}

double autoScale(const Correction& lc, const RectI& frame) {
    auto covers = [&](double scalePct) -> bool {
        Correction c2 = lc;
        c2.scale = scalePct;
        const LensMap m(c2, frame);
        const double x0 = frame.x0, y0 = frame.y0, x1 = frame.x1, y1 = frame.y1;
        for (int i = 0; i <= 40; ++i) {
            const double t = static_cast<double>(i) / 40.0;
            const double pts[4][2] = {{x0 + t * (x1 - x0), y0},
                                      {x0 + t * (x1 - x0), y1},
                                      {x0, y0 + t * (y1 - y0)},
                                      {x1, y0 + t * (y1 - y0)}};
            for (const auto& pt : pts) {
                Sample s;
                if (!m.sample(pt[0], pt[1], s)) {
                    return false;
                }
                if (!(s.p[0] >= x0 - 0.5 && s.p[0] <= x1 + 0.5 && s.p[1] >= y0 - 0.5 &&
                      s.p[1] <= y1 + 0.5)) {
                    return false;
                }
            }
        }
        return true;
    };
    if (covers(lc.scale)) {
        return lc.scale;
    }
    double lo = lc.scale;
    double hi = lc.scale;
    while (!covers(hi) && hi < 400.0) {
        hi *= 1.25;
    }
    for (int i = 0; i < 30; ++i) {
        const double mid = (lo + hi) / 2.0;
        if (covers(mid)) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return hi;
}

double straightenAngle(const double a[2], const double b[2]) {
    const double deg = std::atan2(b[1] - a[1], b[0] - a[0]) * 180.0 / 3.14159265358979323846;
    const double target = std::round(deg / 90.0) * 90.0;
    return -(deg - target);
}

namespace {
// 逆映射重采样（photocraft remap 的 RGBA8 稠密版）：frame 外钳=边缘延展采样，
// 映射出帧的像素吃 edge 模式；1px 软边。
Img remapImg(const Img& src, const RectI& frame, const RectI& outArea, const Correction& lc,
             const LensMap& map) {
    Img out;
    if (outArea.empty() || frame.empty()) {
        return out;
    }
    out.w = outArea.width();
    out.h = outArea.height();
    out.px.assign(static_cast<size_t>(out.w) * out.h * 4u, 0.0f);
    // 预乘源
    Img pre = src;
    if (frame.x1 <= pre.w && frame.y1 <= pre.h) {
        for (size_t i = 0; i < pre.px.size(); i += 4u) {
            const float a = pre.px[i + 3u];
            pre.px[i] *= a;
            pre.px[i + 1u] *= a;
            pre.px[i + 2u] *= a;
        }
    }
    const double fx0 = frame.x0, fy0 = frame.y0, fx1 = frame.x1, fy1 = frame.y1;
    auto bicubic = [&](int c, double px, double py) -> float {
        const double x = std::clamp(px - 0.5, fx0, fx1 - 1.0);
        const double y = std::clamp(py - 0.5, fy0, fy1 - 1.0);
        const double xi = std::floor(x);
        const double yi = std::floor(y);
        const std::array<float, 4> wx = catmullRomF(static_cast<float>(x - xi));
        const std::array<float, 4> wy = catmullRomF(static_cast<float>(y - yi));
        float acc = 0.0f;
        for (int j = 0; j < 4; ++j) {
            const int yy = std::clamp(static_cast<int>(yi) + j - 1, frame.y0, frame.y1 - 1);
            for (int i = 0; i < 4; ++i) {
                const int xx = std::clamp(static_cast<int>(xi) + i - 1, frame.x0, frame.x1 - 1);
                acc += wx[i] * wy[j] * pre.px[(static_cast<size_t>(yy) * pre.w + xx) * 4u + c];
            }
        }
        return acc;
    };
    for (int y = 0; y < out.h; ++y) {
        for (int x = 0; x < out.w; ++x) {
            float* o = &out.px[(static_cast<size_t>(y) * out.w + x) * 4u];
            Sample s;
            const bool ok = map.sample(outArea.x0 + x + 0.5, outArea.y0 + y + 0.5, s);
            if (!ok) {
                // Transparency / 出无穷：边缘色
                o[0] = lc.edgeColor[0];
                o[1] = lc.edgeColor[1];
                o[2] = lc.edgeColor[2];
                o[3] = lc.edge == EdgeMode::Color ? lc.edgeColor[3] : 0.0f;
                continue;
            }
            double inside = 1.0;
            if (lc.edge != EdgeMode::Extension) {
                const double fx = std::clamp(std::min(s.p[0] - fx0, fx1 - s.p[0]) + 0.5, 0.0, 1.0);
                const double fy = std::clamp(std::min(s.p[1] - fy0, fy1 - s.p[1]) + 0.5, 0.0, 1.0);
                inside = fx * fy;
            }
            const float alpha = std::clamp(bicubic(3, s.p[0], s.p[1]), 0.0f, 1.0f);
            const float straight[3] = {
                alpha > 1e-6f ? bicubic(0, s.p[0], s.p[1]) / alpha : 0.0f,
                alpha > 1e-6f ? bicubic(1, s.p[0], s.p[1]) / alpha : 0.0f,
                alpha > 1e-6f ? bicubic(2, s.p[0], s.p[1]) / alpha : 0.0f,
            };
            float col[3];
            col[0] = std::clamp(bicubic(0, s.pr[0], s.pr[1]) / std::max(alpha, 1e-6f) * s.gain, 0.0f, 1.0f);
            col[1] = std::clamp(straight[1] * s.gain, 0.0f, 1.0f);
            col[2] = std::clamp(bicubic(2, s.pb[0], s.pb[1]) / std::max(alpha, 1e-6f) * s.gain, 0.0f, 1.0f);
            if (lc.edge == EdgeMode::Color && inside < 1.0) {
                // 朝边缘色按外侧占比混合
                const float ea = lc.edgeColor[3];
                const float oa = alpha * static_cast<float>(inside) + ea * (1.0f - static_cast<float>(inside));
                for (int c = 0; c < 3; ++c) {
                    const float mixed = col[c] * alpha * static_cast<float>(inside) +
                                        static_cast<float>(lc.edgeColor[c]) * ea *
                                            (1.0f - static_cast<float>(inside));
                    o[c] = oa > 1e-6f ? mixed / oa : 0.0f;
                }
                o[3] = oa;
            } else {
                o[0] = col[0];
                o[1] = col[1];
                o[2] = col[2];
                o[3] = alpha * static_cast<float>(inside);
            }
        }
    }
    return out;
}
}  // namespace

Img correct(const Img& src, const RectI& frame, const Correction& lc) {
    const LensMap map(lc, frame);
    return remapImg(src, frame, frame, lc, map);
}

}  // namespace lens

}  // namespace montage
