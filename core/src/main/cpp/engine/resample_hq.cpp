// M20a 高品质重采样核 —— photocraft crates/algo/src/resample.rs 全量 C++ 翻译
// （MIT OR Apache-2.0）。PS 25.4 Image Size 实测拟合：Keys cubic 相位表 1/128、
// Smoother/Sharper 参数、两遍规划与遍间位深度量化、<1/4 先盒平均、画布边缘重复、
// PreserveDetails = 放大 Lanczos + USM(30%, 0.6·scale, 0) / 缩小 Bicubic Sharper。

#include "engine/resample_hq.h"
#include "engine/vector_render.h"  // affineShapeLayer

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace montage {

namespace {

constexpr int kTile = static_cast<int>(kTileSize);
// Photoshop 子像素相位：采样偏移取整到 1/128 像素（resample.rs PHASES）
constexpr double kPhases = 128.0;

// 单轴滤波核（resample.rs Kernel）
struct Kernel {
    enum Kind { Nearest, Bilinear, Keys, Lanczos } kind = Nearest;
    // Keys：a=曲线参数，stretch=源像素上的拉伸
    double a = 0.0;
    double stretch = 1.0;
};

double keysWeight(double x, double a) {
    x = std::fabs(x);
    if (x < 1.0) {
        return (a + 2.0) * x * x * x - (a + 3.0) * x * x + 1.0;
    }
    if (x < 2.0) {
        return a * (x * x * x - 5.0 * x * x + 8.0 * x - 4.0);
    }
    return 0.0;
}

double lanczosWeight(double x) {
    x = std::fabs(x);
    if (x < 1e-9) {
        return 1.0;
    }
    if (x < 3.0) {
        const double px = 3.14159265358979323846 * x;
        return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
    }
    return 0.0;
}

// Photoshop 的 bicubic 滤波参数（PS 25.4 拟合，s=设计缩放）：
// Bicubic a=−0.75；Smoother stretch×1.15、a=−0.625−0.5(1−s)；Sharper stretch×1.05、
// a=−1−1.6(1−s)。缩小按 1/s 拉宽（钳到 1/4，以下先盒平均）。
Kernel keys(ResampleMethod method, double s) {
    s = std::clamp(s, 0.25, 1.0);
    double a = -0.75;
    double stretch = 1.0;
    switch (method) {
        case ResampleMethod::BicubicSmoother:
            a = -0.625 - 0.5 * (1.0 - s);
            stretch = 1.15;
            break;
        case ResampleMethod::BicubicSharper:
            a = -1.0 - 1.6 * (1.0 - s);
            stretch = 1.05;
            break;
        default:
            break;
    }
    Kernel k;
    k.kind = Kernel::Keys;
    k.a = a;
    k.stretch = stretch / s;
    return k;
}

// 一遍重采样：两轴缩放与滤波（resample.rs Pass）
struct Pass {
    double sx = 1.0;
    double sy = 1.0;
    Kernel kx;
    Kernel ky;
};

Kernel nearestK() {
    Kernel k;
    k.kind = Kernel::Nearest;
    return k;
}

Kernel bilinearK() {
    Kernel k;
    k.kind = Kernel::Bilinear;
    return k;
}

Kernel lanczosK() {
    Kernel k;
    k.kind = Kernel::Lanczos;
    return k;
}

// Photoshop 的两遍规划（resample.rs plan）：bicubic 按高度缩放（高度不变取宽度）设计、
// 双轴共用、小轴先行（遍间量化=存储裁剪）；Automatic 混合缩放拆成 Sharper+Smoother 两遍；
// Nearest/Bilinear/Lanczos/放大的 PreserveDetails 单遍双轴。
std::vector<Pass> plan(ResampleMethod filter, double sx, double sy) {
    auto both = [](Kernel k, double sx_, double sy_) {
        std::vector<Pass> v;
        Pass p;
        p.sx = sx_;
        p.sy = sy_;
        p.kx = k;
        p.ky = k;
        v.push_back(p);
        return v;
    };
    switch (filter) {
        case ResampleMethod::Nearest:
            return both(nearestK(), sx, sy);
        case ResampleMethod::Bilinear:
            return both(bilinearK(), sx, sy);
        case ResampleMethod::Lanczos:
            return both(lanczosK(), sx, sy);
        case ResampleMethod::PreserveDetails:
            if (sx > 1.0 || sy > 1.0) {
                return both(lanczosK(), sx, sy);
            }
            return plan(ResampleMethod::BicubicSharper, sx, sy);
        case ResampleMethod::BicubicAutomatic:
            if (sx <= 1.0 && sy <= 1.0) {
                return plan(ResampleMethod::BicubicSharper, sx, sy);
            }
            if (sx >= 1.0 && sy >= 1.0) {
                return plan(ResampleMethod::BicubicSmoother, sx, sy);
            }
            if (sx < 1.0) {
                std::vector<Pass> v;
                Pass p1;
                p1.sx = sx;
                p1.sy = 1.0;
                p1.kx = keys(ResampleMethod::BicubicSharper, sx);
                p1.ky = nearestK();
                Pass p2;
                p2.sx = 1.0;
                p2.sy = sy;
                p2.kx = nearestK();
                p2.ky = keys(ResampleMethod::BicubicSmoother, sy);
                v.push_back(p1);
                v.push_back(p2);
                return v;
            }
            {
                std::vector<Pass> v;
                Pass p1;
                p1.sx = 1.0;
                p1.sy = sy;
                p1.kx = nearestK();
                p1.ky = keys(ResampleMethod::BicubicSharper, sy);
                Pass p2;
                p2.sx = sx;
                p2.sy = 1.0;
                p2.kx = keys(ResampleMethod::BicubicSmoother, sx);
                p2.ky = nearestK();
                v.push_back(p1);
                v.push_back(p2);
                return v;
            }
        case ResampleMethod::Bicubic:
        case ResampleMethod::BicubicSmoother:
        case ResampleMethod::BicubicSharper:
        default: {
            const Kernel k = keys(filter, sy != 1.0 ? sy : sx);
            if (sx == 1.0 || sy == 1.0) {
                return both(k, sx, sy);
            }
            std::vector<Pass> v;
            if (sy < sx) {
                Pass p1;
                p1.sx = 1.0;
                p1.sy = sy;
                p1.kx = k;
                p1.ky = k;
                Pass p2;
                p2.sx = sx;
                p2.sy = 1.0;
                p2.kx = k;
                p2.ky = k;
                v.push_back(p1);
                v.push_back(p2);
            } else {
                Pass p1;
                p1.sx = sx;
                p1.sy = 1.0;
                p1.kx = k;
                p1.ky = k;
                Pass p2;
                p2.sx = 1.0;
                p2.sy = sy;
                p2.kx = k;
                p2.ky = k;
                v.push_back(p1);
                v.push_back(p2);
            }
            return v;
        }
    }
}

// 输出采样 o 的源位置 (o+0.5)/scale−0.5，贴边样本钳制、1/128 相位取整（resample.rs position）
double position(int o, double scale, const bool hasEdge, const int edge[2]) {
    double u = (static_cast<double>(o) + 0.5) / scale - 0.5;
    if (hasEdge) {
        u = std::clamp(u, static_cast<double>(edge[0]),
                       static_cast<double>(std::max(edge[0], edge[1])));
    }
    const double j = std::floor(u);
    return j + std::round((u - j) * kPhases) / kPhases;
}

// 输出像素 o 覆盖的源像素区间 [floor(o/scale), floor((o+1)/scale))（resample.rs cover）
void cover(int o, double scale, int out[2]) {
    const int a = static_cast<int>(std::floor(static_cast<double>(o) / scale + 1e-9));
    out[0] = a;
    out[1] = std::max(static_cast<int>(std::floor((static_cast<double>(o) + 1.0) / scale + 1e-9)), a + 1);
}

// 输出采样 o 在源样本 lo..lo+len 上的未钳制权重（resample.rs weights）
void weights(int o, double scale, const Kernel& kernel, bool hasEdge, const int edge[2],
             int& lo, std::vector<double>& w) {
    switch (kernel.kind) {
        case Kernel::Nearest: {
            const double u = (static_cast<double>(o) + 0.5) / scale - 0.5;
            lo = static_cast<int>(std::floor(u + 0.5));
            w.assign(1, 1.0);
            return;
        }
        case Kernel::Bilinear: {
            if (scale < 0.5) {
                int cv[2];
                cover(o, scale, cv);
                lo = cv[0];
                w.assign(cv[1] - cv[0], 1.0 / static_cast<double>(cv[1] - cv[0]));
                return;
            }
            const double u = position(o, scale, hasEdge, edge);
            const double j = std::floor(u);
            lo = static_cast<int>(j);
            w.assign({1.0 - (u - j), u - j});
            return;
        }
        case Kernel::Keys: {
            if (scale < 0.25) {
                // 先盒平均到 4 倍目标（inner 像素），再滤波（resample.rs Keys scale<0.25 分支）
                const double inner = 4.0 * scale;
                int innerEdge[2] = {0, 0};
                bool hasInner = false;
                if (hasEdge) {
                    innerEdge[0] = static_cast<int>(std::ceil(static_cast<double>(edge[0]) * inner - 1e-9));
                    innerEdge[1] = static_cast<int>(std::ceil((static_cast<double>(edge[1]) + 1.0) * inner - 1e-9)) - 1;
                    hasInner = true;
                }
                int k0 = 0;
                std::vector<double> kw;
                weights(o, 0.25, kernel, hasInner, innerEdge, k0, kw);
                auto clampK = [&](int k) -> int {
                    if (!hasInner) {
                        return k;
                    }
                    return std::clamp(k, innerEdge[0], std::max(innerEdge[0], innerEdge[1]));
                };
                std::vector<std::pair<int, int>> boxes;
                for (size_t i = 0; i < kw.size(); ++i) {
                    int cv[2];
                    cover(clampK(k0 + static_cast<int>(i)), inner, cv);
                    boxes.emplace_back(cv[0], cv[1]);
                }
                int minLo = 0;
                int maxHi = 0;
                if (!boxes.empty()) {
                    minLo = boxes.front().first;
                    maxHi = boxes.front().second;
                    for (const auto& b : boxes) {
                        minLo = std::min(minLo, b.first);
                        maxHi = std::max(maxHi, b.second);
                    }
                }
                lo = minLo;
                w.assign(static_cast<size_t>(std::max(0, maxHi - minLo)), 0.0);
                for (size_t i = 0; i < boxes.size(); ++i) {
                    const double v = kw[i];
                    for (int slot = boxes[i].first; slot < boxes[i].second; ++slot) {
                        w[static_cast<size_t>(slot - minLo)] += v / static_cast<double>(boxes[i].second - boxes[i].first);
                    }
                }
                return;
            }
            const double u = position(o, scale, hasEdge, edge);
            const double st = kernel.stretch;
            const int l = static_cast<int>(std::floor(u - 2.0 * st));
            const int h = static_cast<int>(std::ceil(u + 2.0 * st));
            lo = l;
            w.clear();
            double sum = 0.0;
            for (int i = l; i <= h; ++i) {
                const double v = keysWeight((u - static_cast<double>(i)) / st, kernel.a);
                w.push_back(v);
                sum += v;
            }
            for (double& v : w) {
                v /= sum;
            }
            return;
        }
        case Kernel::Lanczos:
        default: {
            const double stretch = scale < 1.0 ? 1.0 / scale : 1.0;
            double u = (static_cast<double>(o) + 0.5) / scale - 0.5;
            if (hasEdge) {
                u = std::clamp(u, static_cast<double>(edge[0]),
                               static_cast<double>(std::max(edge[0], edge[1])));
            }
            const int l = static_cast<int>(std::ceil(u - 3.0 * stretch));
            const int h = static_cast<int>(std::floor(u + 3.0 * stretch));
            lo = l;
            w.clear();
            double sum = 0.0;
            for (int i = l; i <= h; ++i) {
                const double v = lanczosWeight((static_cast<double>(i) - u) / stretch);
                w.push_back(v);
                sum += v;
            }
            const double k = std::fabs(sum) > 1e-12 ? 1.0 / sum : 1.0;
            for (double& v : w) {
                v *= k;
            }
            return;
        }
    }
}

// 单输出采样的源窗口与归一化权重（resample.rs Taps）
struct Taps {
    int start = 0;
    std::vector<float> weights;
    float outside = 0.0f;  // 内容之外的全部权重（读默认像素）
};

// 输出坐标区间 [dst0,dst1) 到源的映射权重表（resample.rs taps）：edge 存在时权重折到边
// 样本（边缘外重复）；content 之外的权重折进 outside（读窗口永不越过内容）。
std::vector<Taps> taps(int dst0, int dst1, double scale, const Kernel& kernel, bool hasEdge,
                       const int edge[2], const int content[2]) {
    auto clampI = [&](int i) -> int {
        if (!hasEdge) {
            return i;
        }
        return std::clamp(i, edge[0], std::max(edge[0], edge[1]));
    };
    const Kernel k = scale == 1.0 ? nearestK() : kernel;
    std::vector<Taps> out;
    out.reserve(static_cast<size_t>(dst1 - dst0));
    for (int o = dst0; o < dst1; ++o) {
        int lo = 0;
        std::vector<double> w;
        weights(o, scale, k, hasEdge, edge, lo, w);
        const int hi = lo + static_cast<int>(w.size()) - 1;
        const int start = clampI(lo);
        const int end = clampI(hi);
        std::vector<float> folded(static_cast<size_t>(std::max(1, end - start + 1)), 0.0f);
        for (size_t i = 0; i < w.size(); ++i) {
            const int idx = clampI(lo + static_cast<int>(i)) - start;
            if (idx >= 0 && idx < static_cast<int>(folded.size())) {
                folded[static_cast<size_t>(idx)] += static_cast<float>(w[i]);
            }
        }
        const int a = std::max(start, content[0]);
        const int b = std::min(start + static_cast<int>(folded.size()) - 1, content[1]);
        Taps t;
        if (a > b) {
            t.start = content[0];
            float s = 0.0f;
            for (const float v : folded) {
                s += v;
            }
            t.outside = s;
            out.push_back(std::move(t));
            continue;
        }
        const size_t i0 = static_cast<size_t>(a - start);
        const size_t i1 = static_cast<size_t>(b - start) + 1;
        float outside = 0.0f;
        for (size_t i = 0; i < i0 && i < folded.size(); ++i) {
            outside += folded[i];
        }
        for (size_t i = i1; i < folded.size(); ++i) {
            outside += folded[i];
        }
        t.start = a;
        t.weights.assign(folded.begin() + static_cast<long>(i0),
                         folded.begin() + static_cast<long>(std::min(i1, folded.size())));
        t.outside = outside;
        out.push_back(std::move(t));
    }
    return out;
}

// 源缩放矩形（resample.rs scaled_rect，about 原点）
RectI scaledRect(const RectI& r, double sx, double sy) {
    RectI o;
    o.x0 = static_cast<int>(std::floor(static_cast<double>(r.x0) * sx));
    o.y0 = static_cast<int>(std::floor(static_cast<double>(r.y0) * sy));
    o.x1 = static_cast<int>(std::ceil(static_cast<double>(r.x1) * sx));
    o.y1 = static_cast<int>(std::ceil(static_cast<double>(r.y1) * sy));
    return o;
}

void premultiply(float* px, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float* p = px + i * 4;
        const float a = p[3];
        p[0] *= a;
        p[1] *= a;
        p[2] *= a;
    }
}

void unpremultiply(float* px, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float* p = px + i * 4;
        const float a = p[3];
        if (a > 1e-6f) {
            const float inv = 1.0f / a;
            p[0] = std::min(1.0f, p[0] * inv);
            p[1] = std::min(1.0f, p[1] * inv);
            p[2] = std::min(1.0f, p[2] * inv);
        } else {
            p[0] = p[1] = p[2] = 0.0f;
        }
    }
}

// 遍间存储量化（=PS 在 8/16 位存储上的裁剪与舍入）
void quantize(std::vector<float>& px, int bitDepth) {
    if (bitDepth >= 32) {
        return;
    }
    const double maxV = static_cast<double>((1u << bitDepth) - 1);
    for (float& v : px) {
        v = static_cast<float>(std::round(std::clamp(static_cast<double>(v), 0.0, 1.0) * maxV) / maxV);
    }
}

// 一行水平滤波（taps×4 通道；行覆盖源 x_lo..x_hi，出窗读默认像素）
void horizontalRow(const float* row, int xLo, int rowLen, const std::vector<Taps>& hx,
                   const float def[4], float* out) {
    for (size_t ti = 0; ti < hx.size(); ++ti) {
        const Taps& t = hx[ti];
        float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        for (size_t k = 0; k < t.weights.size(); ++k) {
            const int sx = t.start + static_cast<int>(k) - xLo;
            if (sx >= 0 && sx < rowLen) {
                const float* sp = row + static_cast<size_t>(sx) * 4;
                const float w = t.weights[k];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += sp[c] * w;
                }
            }
        }
        for (int c = 0; c < 4; ++c) {
            // 窗外权重补默认像素（resample.rs horizontal_*：acc += def·outside）
            out[ti * 4u + static_cast<size_t>(c)] = acc[c] + def[c] * t.outside;
        }
    }
}

// PreserveDetails 放大后的轻锐化（photocraft UnsharpMask{30, 0.6·max(sx,sy), 0} 直迁；
// 可分离高斯近似 + amount 差分）
void unsharpPreserveDetails(ImageF& img, double radius) {
    if (img.w <= 0 || img.h <= 0 || radius <= 0.0) {
        return;
    }
    const double sigma = std::max(0.3, radius * 0.5);
    const int rad = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
    std::vector<double> kern(static_cast<size_t>(rad * 2 + 1));
    double ksum = 0.0;
    for (int i = -rad; i <= rad; ++i) {
        const double v = std::exp(-static_cast<double>(i * i) / (2.0 * sigma * sigma));
        kern[static_cast<size_t>(i + rad)] = v;
        ksum += v;
    }
    for (double& v : kern) {
        v /= ksum;
    }
    const int w = img.w;
    const int h = img.h;
    std::vector<float> blurred(img.px.size(), 0.0f);
    // 水平
    std::vector<float> tmp(img.px.size(), 0.0f);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int i = -rad; i <= rad; ++i) {
                const int sx = std::clamp(x + i, 0, w - 1);
                const double kv = kern[static_cast<size_t>(i + rad)];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += img.px[(static_cast<size_t>(y) * w + sx) * 4u + c] * static_cast<float>(kv);
                }
            }
            for (int c = 0; c < 4; ++c) {
                tmp[(static_cast<size_t>(y) * w + x) * 4u + c] = acc[c];
            }
        }
    }
    // 垂直
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int i = -rad; i <= rad; ++i) {
                const int sy = std::clamp(y + i, 0, h - 1);
                const double kv = kern[static_cast<size_t>(i + rad)];
                for (int c = 0; c < 4; ++c) {
                    acc[c] += tmp[(static_cast<size_t>(sy) * w + x) * 4u + c] * static_cast<float>(kv);
                }
            }
            for (int c = 0; c < 4; ++c) {
                blurred[(static_cast<size_t>(y) * w + x) * 4u + c] = acc[c];
            }
        }
    }
    constexpr float kAmount = 0.30f;
    for (size_t i = 0; i < img.px.size(); i += 4u) {
        for (int c = 0; c < 3; ++c) {
            img.px[i + static_cast<size_t>(c)] =
                std::clamp(img.px[i + static_cast<size_t>(c)] +
                               kAmount * (img.px[i + static_cast<size_t>(c)] - blurred[i + static_cast<size_t>(c)]),
                           0.0f, 1.0f);
        }
    }
}

// 一遍重采样（resample.rs resize_pass 的稠密源版）：64 行输出带式流过，带内源行经水平
// 滤波直接累加进带输出（内存 O(带)，不随整图驻留 f32）。edge=画布∪内容时边缘重复。
ImageF resizePass(const ImageF& s, const Pass& pass, bool hasCanvas, const RectI& canvas,
                  int bitDepth) {
    ImageF out;
    out.w = 0;
    out.h = 0;
    // 内容=整图（稠密源）；edge=画布∪内容
    RectI srcRect;
    srcRect.x0 = 0;
    srcRect.y0 = 0;
    srcRect.x1 = s.w;
    srcRect.y1 = s.h;
    RectI edge = srcRect;
    if (hasCanvas && !canvas.empty()) {
        edge.x0 = std::min(edge.x0, canvas.x0);
        edge.y0 = std::min(edge.y0, canvas.y0);
        edge.x1 = std::max(edge.x1, canvas.x1);
        edge.y1 = std::max(edge.y1, canvas.y1);
    }
    const bool hasEdge = hasCanvas && !canvas.empty();
    const RectI dst = scaledRect(srcRect, pass.sx, pass.sy);
    if (dst.empty()) {
        return out;
    }
    // 边缘钳制区间（resample.rs edge.map(|r| (r.x0, r.x1-1))）
    int edgeX[2] = {edge.x0, edge.x1 - 1};
    int edgeY[2] = {edge.y0, edge.y1 - 1};
    int contentX[2] = {edge.x0, edge.x1 - 1};
    int contentY[2] = {edge.y0, edge.y1 - 1};
    if (!hasEdge) {
        contentX[0] = srcRect.x0;
        contentX[1] = srcRect.x1 - 1;
        contentY[0] = srcRect.y0;
        contentY[1] = srcRect.y1 - 1;
    }
    const std::vector<Taps> hx = taps(dst.x0, dst.x1, pass.sx, pass.kx, hasEdge, edgeX, contentX);
    const std::vector<Taps> vy = taps(dst.y0, dst.y1, pass.sy, pass.ky, hasEdge, edgeY, contentY);
    const float def[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // 稠密源默认像素=透明
    const int dw = dst.width();
    constexpr int kBand = 64;
    out.w = dst.width();
    out.h = dst.height();
    out.px.assign(static_cast<size_t>(dw) * dst.height() * 4u, 0.0f);
    std::vector<float> srcRow(static_cast<size_t>(std::max(1, s.w)) * 4u);
    std::vector<float> hrow(static_cast<size_t>(dw) * 4u);
    for (int b0 = 0; b0 < static_cast<int>(vy.size()); b0 += kBand) {
        const int b1 = std::min(b0 + kBand, static_cast<int>(vy.size()));
        int yLo = edge.y0;
        int yHi = edge.y0;
        bool first = true;
        for (int bi = b0; bi < b1; ++bi) {
            const Taps& t = vy[static_cast<size_t>(bi)];
            if (t.weights.empty()) {
                continue;
            }
            if (first) {
                yLo = t.start;
                yHi = t.start + static_cast<int>(t.weights.size());
                first = false;
            } else {
                yLo = std::min(yLo, t.start);
                yHi = std::max(yHi, t.start + static_cast<int>(t.weights.size()));
            }
        }
        for (int y = yLo; y < yHi; ++y) {
            if (y >= 0 && y < s.h) {
                std::memcpy(srcRow.data(), s.px.data() + static_cast<size_t>(y) * s.w * 4u,
                            static_cast<size_t>(s.w) * 4u * sizeof(float));
                premultiply(srcRow.data(), static_cast<size_t>(s.w));
                horizontalRow(srcRow.data(), 0, s.w, hx, def, hrow.data());
            } else {
                // 出图源行 = 默认像素行
                for (size_t i = 0; i < hrow.size(); i += 4u) {
                    for (int c = 0; c < 4; ++c) {
                        hrow[i + static_cast<size_t>(c)] = def[c];
                    }
                }
            }
            for (int bi = b0; bi < b1; ++bi) {
                const Taps& t = vy[static_cast<size_t>(bi)];
                const int idx = y - t.start;
                if (idx >= 0 && idx < static_cast<int>(t.weights.size())) {
                    const float w = t.weights[static_cast<size_t>(idx)];
                    float* d = out.px.data() + static_cast<size_t>(bi) * dw * 4u;
                    for (size_t i = 0; i < static_cast<size_t>(dw) * 4u; ++i) {
                        d[i] += hrow[i] * w;
                    }
                }
            }
        }
        // 出窗权重的默认像素补齐
        for (int bi = b0; bi < b1; ++bi) {
            const Taps& t = vy[static_cast<size_t>(bi)];
            if (t.outside != 0.0f) {
                float* d = out.px.data() + static_cast<size_t>(bi) * dw * 4u;
                for (int x = 0; x < dw; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        d[static_cast<size_t>(x) * 4u + static_cast<size_t>(c)] += def[c] * t.outside;
                    }
                }
            }
        }
    }
    // alpha 钳制 + 反预乘
    for (size_t i = 0; i < out.px.size(); i += 4u) {
        out.px[i + 3u] = std::clamp(out.px[i + 3u], 0.0f, 1.0f);
    }
    unpremultiply(out.px.data(), static_cast<size_t>(dw) * out.h);
    quantize(out.px, bitDepth);
    return out;
}

}  // namespace

ImageF resampleImageHQ(const ImageF& src, int dstW, int dstH, ResampleMethod method,
                       int bitDepth, const RectI& canvas) {
    const double sx = src.w > 0 ? static_cast<double>(dstW) / src.w : 1.0;
    const double sy = src.h > 0 ? static_cast<double>(dstH) / src.h : 1.0;
    ImageF cur = src;
    ImageF result;
    bool has = false;
    RectI can = canvas;
    for (const Pass& p : plan(method, sx, sy)) {
        ImageF next = resizePass(has ? cur : src, p, !can.empty(), can, bitDepth);
        // 画布随遍缩放（about 原点）
        if (!can.empty()) {
            can = scaledRect(can, p.sx, p.sy);
        }
        cur = std::move(next);
        has = true;
    }
    if (has) {
        result = std::move(cur);
    }
    if (method == ResampleMethod::PreserveDetails && (sx > 1.0 || sy > 1.0) && !result.px.empty()) {
        unsharpPreserveDetails(result, 0.6 * std::max(sx, sy));
        quantize(result.px, bitDepth);
    }
    if (result.w == 0 && result.h == 0) {
        result.w = dstW;
        result.h = dstH;
        result.px.assign(static_cast<size_t>(dstW) * dstH * 4u, 0.0f);
    }
    return result;
}

std::shared_ptr<const TileGrid> resampleGridHQ(const TileGrid& src, uint32_t dstCols,
                                               uint32_t dstRows, ResampleMethod method) {
    const uint32_t srcW = src.cols * kTileSize;
    const uint32_t srcH = src.rows * kTileSize;
    if (srcW == 0 || srcH == 0 || dstCols == 0 || dstRows == 0) {
        return nullptr;
    }
    // 整网格读成 f32 RGBA（图像大小为整档操作，网格 ≤20MP 预算）
    ImageF in;
    in.w = static_cast<int>(srcW);
    in.h = static_cast<int>(srcH);
    in.px.assign(static_cast<size_t>(srcW) * srcH * 4u, 0.0f);
    std::vector<uint8_t> row(static_cast<size_t>(srcW) * 4u);
    for (uint32_t y = 0; y < srcH; ++y) {
        const uint32_t ty = y / kTileSize;
        const uint32_t inY = y % kTileSize;
        std::memset(row.data(), 0, static_cast<size_t>(srcW) * 4u);
        for (uint32_t tx = 0; tx < src.cols; ++tx) {
            auto it = src.tiles.find(ty * src.cols + tx);
            if (it == src.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
                continue;
            }
            const uint8_t* sp = it->second->pixels->mapCpu();
            if (sp == nullptr) {
                continue;
            }
            std::memcpy(row.data() + static_cast<size_t>(tx) * kTileSize * 4u,
                        sp + static_cast<size_t>(inY) * it->second->pixels->rowBytes(),
                        static_cast<size_t>(kTileSize) * 4u);
        }
        for (uint32_t x = 0; x < srcW; ++x) {
            float* d = in.px.data() + (static_cast<size_t>(y) * srcW + x) * 4u;
            d[0] = static_cast<float>(row[static_cast<size_t>(x) * 4u]) / 255.0f;
            d[1] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 1u]) / 255.0f;
            d[2] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 2u]) / 255.0f;
            d[3] = static_cast<float>(row[static_cast<size_t>(x) * 4u + 3u]) / 255.0f;
        }
    }
    // 网格边界 = 画布（放大后贴边不褪边，resample_surface_in_canvas 语义）
    RectI canvas;
    canvas.x0 = 0;
    canvas.y0 = 0;
    canvas.x1 = static_cast<int>(srcW);
    canvas.y1 = static_cast<int>(srcH);
    ImageF out = resampleImageHQ(in, static_cast<int>(dstCols * kTileSize),
                                 static_cast<int>(dstRows * kTileSize), method, 8, canvas);
    const uint32_t outW = static_cast<uint32_t>(out.w);
    const uint32_t outH = static_cast<uint32_t>(out.h);
    TileGridBuilder builder(dstCols, dstRows);
    std::vector<uint8_t> outRow(static_cast<size_t>(outW) * 4u);
    for (uint32_t y = 0; y < outH; ++y) {
        bool any = false;
        for (uint32_t x = 0; x < outW; ++x) {
            const float* sp = out.px.data() + (static_cast<size_t>(y) * outW + x) * 4u;
            uint8_t* d = outRow.data() + static_cast<size_t>(x) * 4u;
            for (int c = 0; c < 4; ++c) {
                d[static_cast<size_t>(c)] =
                    static_cast<uint8_t>(std::lround(std::clamp(sp[c], 0.0f, 1.0f) * 255.0f));
            }
            if (d[3] != 0) {
                any = true;
            }
        }
        if (!any) {
            continue;
        }
        for (uint32_t tx = 0; tx < dstCols; ++tx) {
            bool tileAny = false;
            for (int k = 0; k < kTile; ++k) {
                if (outRow[(static_cast<size_t>(tx) * kTileSize + k) * 4u + 3u] != 0) {
                    tileAny = true;
                    break;
                }
            }
            if (!tileAny) {
                continue;
            }
            uint8_t* dst = builder.ensureTile(tx, y / kTileSize).mapCpuWrite() +
                           (y % kTileSize) * kTileSize * 4u;
            std::memcpy(dst, outRow.data() + static_cast<size_t>(tx) * kTileSize * 4u,
                        static_cast<size_t>(kTileSize) * 4u);
        }
    }
    return builder.publish(src.revision + 1);
}

bool resampleDocumentHQ(Document& doc, uint32_t newW, uint32_t newH, ResampleMethod method) {
    if (doc.width == 0 || doc.height == 0 || newW == 0 || newH == 0) {
        return false;
    }
    if (newW == doc.width && newH == doc.height) {
        return false;
    }
    const double sx = static_cast<double>(newW) / doc.width;
    const double sy = static_cast<double>(newH) / doc.height;
    for (Layer& l : doc.layers) {
        const uint32_t newCols = std::max(
            1u, static_cast<uint32_t>(std::ceil(static_cast<double>(l.pixels ? l.pixels->cols : 1) * sx)));
        const uint32_t newRows = std::max(
            1u, static_cast<uint32_t>(std::ceil(static_cast<double>(l.pixels ? l.pixels->rows : 1) * sy)));
        if (l.shape != nullptr) {
            // M18：形状路径同步缩放（x'=sx·x, y'=sy·y）
            affineShapeLayer(l, sx, 0.0, 0.0, sy, 0.0, 0.0);
        }
        if (l.pixels != nullptr) {
            l.pixels = resampleGridHQ(*l.pixels, newCols, newRows, method);
        }
        l.transform.originX *= sx;
        l.transform.originY *= sy;
        if (l.mask != nullptr) {
            auto m2 = std::make_shared<LayerMask>(*l.mask);
            if (m2->pixels != nullptr) {
                const uint32_t mCols = std::max(
                    1u, static_cast<uint32_t>(std::ceil(m2->pixels->cols * sx)));
                const uint32_t mRows = std::max(
                    1u, static_cast<uint32_t>(std::ceil(m2->pixels->rows * sy)));
                m2->pixels = resampleGridHQ(*m2->pixels, mCols, mRows, method);
            }
            m2->width = std::max(1u, static_cast<uint32_t>(std::lround(m2->width * sx)));
            m2->height = std::max(1u, static_cast<uint32_t>(std::lround(m2->height * sy)));
            m2->offsetX = static_cast<int>(std::lround(m2->offsetX * sx));
            m2->offsetY = static_cast<int>(std::lround(m2->offsetY * sy));
            l.mask = std::move(m2);
        }
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.width = newW;
    doc.height = newH;
    doc.selection = nullptr;
    doc.quickMask = nullptr;
    doc.channels.clear();
    return true;
}

}  // namespace montage
