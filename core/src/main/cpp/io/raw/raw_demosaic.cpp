// M23a：去马赛克实现。翻译自 photocraft crates/raw/src/demosaic.rs（MIT OR Apache-2.0）。
// par::chunks_mut 单线程化（photocraft 测试已证线程数无关；设备性能基线挂 M16）。

#include "io/raw/raw_demosaic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

#include "io/raw/raw_color.h"

namespace montage {
namespace raw {

void Padded::reset(size_t width, size_t height) {
    w = width;
    h = height;
    stride = width + 2 * kDemosaicPad;
    data.assign(stride * (height + 2 * kDemosaicPad), 0.0f);
}

namespace {

// 把 i 镜像反射进 0..n（n ≥ 1）；n 极小时钳制。
inline size_t reflect(long long i, size_t n) {
    long long nn = static_cast<long long>(n);
    if (nn <= 1) return 0;
    if (i < 0) i = -i;
    if (i >= nn) i = 2 * (nn - 1) - i;
    if (i < 0) i = 0;
    if (i >= nn) i = nn - 1;
    return static_cast<size_t>(i);
}

void fillBordersImpl(std::vector<float>& data, size_t w, size_t h, size_t s) {
    if (w == 0 || h == 0) return;
    constexpr size_t PAD = kDemosaicPad;
    for (size_t y = PAD; y < PAD + h; ++y) {
        float* row = data.data() + y * s;
        for (size_t px = 0; px < PAD; ++px) row[px] = row[PAD + reflect(static_cast<long long>(px) - static_cast<long long>(PAD), w)];
        for (size_t px = PAD + w; px < s; ++px)
            row[px] = row[PAD + reflect(static_cast<long long>(px) - static_cast<long long>(PAD), w)];
    }
    auto sideRows = [&](size_t py) {
        size_t src = PAD + reflect(static_cast<long long>(py) - static_cast<long long>(PAD), h);
        std::memcpy(data.data() + py * s, data.data() + src * s, s * sizeof(float));
    };
    for (size_t py = 0; py < PAD; ++py) sideRows(py);
    for (size_t py = PAD + h; py < h + 2 * PAD; ++py) sideRows(py);
}

}  // namespace

void Padded::fillBorders() { fillBordersImpl(data, w, h, stride); }

uint8_t DemosaicPattern::at(long long x, long long y) const {
    long long ww = static_cast<long long>(w > 0 ? w : 1);
    long long hh = static_cast<long long>(h > 0 ? h : 1);
    long long mx = ((x % ww) + ww) % ww;
    long long my = ((y % hh) + hh) % hh;
    size_t idx = static_cast<size_t>(my) * w + static_cast<size_t>(mx);
    return idx < colors.size() ? colors[idx] : 1;
}

namespace {

// 每 2×2 位置、每输出颜色的核抽头 (offset, weight)。offset = dy*stride + dx。
using Taps = std::array<std::array<std::vector<std::pair<long long, float>>, 3>, 4>;

Taps kernels(const uint8_t phase[4], Demosaic method, size_t stride) {
    auto color = [&](long long x, long long y) -> int { return phase[(static_cast<size_t>(y) & 1) * 2 + (static_cast<size_t>(x) & 1)]; };
    long long s = static_cast<long long>(stride);
    Taps ks;
    for (size_t pos = 0; pos < 4; ++pos) {
        long long px = static_cast<long long>(pos & 1);
        long long py = static_cast<long long>(pos >> 1);
        int own = color(px, py);
        for (size_t c = 0; c < 3; ++c) {
            int cc = static_cast<int>(c);
            std::vector<std::tuple<long long, long long, float>> taps;
            if (cc == own) {
                taps = {{0, 0, 1.0f}};
            } else if (method == Demosaic::Bilinear) {
                for (long long dy = -1; dy <= 1; ++dy)
                    for (long long dx = -1; dx <= 1; ++dx)
                        if (color(px + dx, py + dy) == cc) taps.emplace_back(dx, dy, 0.0f);
                float wgt = 1.0f / static_cast<float>(std::max<size_t>(taps.size(), 1));
                for (auto& t : taps) std::get<2>(t) = wgt;
            } else if (cc == 1) {
                // Malvar–He–Cutler（权重 / 8）：R 或 B 处的 G。
                taps = {{0, 0, 4.0f},  {-1, 0, 2.0f},  {1, 0, 2.0f},  {0, -1, 2.0f}, {0, 1, 2.0f},
                        {-2, 0, -1.0f}, {2, 0, -1.0f}, {0, -2, -1.0f}, {0, 2, -1.0f}};
            } else if (own == 1) {
                // G 处的 R 或 B：取行邻或列邻。
                long long ax = color(px + 1, py) == cc ? 1 : 0;
                long long ay = ax == 1 ? 0 : 1;
                taps = {{0, 0, 5.0f},
                        {-ax, -ay, 4.0f},
                        {ax, ay, 4.0f},
                        {-1, -1, -1.0f},
                        {1, -1, -1.0f},
                        {-1, 1, -1.0f},
                        {1, 1, -1.0f},
                        {-2 * ax, -2 * ay, -1.0f},
                        {2 * ax, 2 * ay, -1.0f},
                        {-2 * ay, -2 * ax, 0.5f},
                        {2 * ay, 2 * ax, 0.5f}};
            } else {
                // B 处的 R 或 R 处的 B。
                taps = {{0, 0, 6.0f},  {-1, -1, 2.0f}, {1, -1, 2.0f},  {-1, 1, 2.0f},  {1, 1, 2.0f},
                        {-2, 0, -1.5f}, {2, 0, -1.5f}, {0, -2, -1.5f}, {0, 2, -1.5f}};
            }
            float norm = (method == Demosaic::Bilinear || cc == own) ? 1.0f : 1.0f / 8.0f;
            for (auto& t : taps) ks[pos][c].emplace_back(std::get<1>(t) * s + std::get<0>(t), std::get<2>(t) * norm);
        }
    }
    return ks;
}

// 反梯度权重正则化（归一化单位，约满刻度的 1/256）。
constexpr float kGradEps = 1.0f / 256.0f;
// 色差的绿相似性权重正则化。
constexpr float kDiffEps = 1.0f / 64.0f;

// CIE f(t)（L*a*b*），0..2 上列表化线性插值。
struct LabF {
    std::vector<float> table;
    static constexpr size_t kSteps = 1 << 14;
    static constexpr float kRange = 2.0f;

    LabF() {
        table.reserve(kSteps + 2);
        for (size_t i = 0; i <= kSteps + 1; ++i) table.push_back(exact(static_cast<float>(i) * kRange / static_cast<float>(kSteps)));
    }

    static float exact(float t) {
        if (t > 0.008856f) return std::cbrt(t);
        return 7.787f * t + 16.0f / 116.0f;
    }

    inline float f(float t) const {
        if (!(t >= 0.0f && t < kRange)) return exact(std::max(t, 0.0f));
        float x = t * (static_cast<float>(kSteps) / kRange);
        size_t i = std::min(static_cast<size_t>(x), kSteps);
        float a = table[i];
        return a + (table[i + 1] - a) * (x - static_cast<float>(i));
    }
};

// AHD 每带输出行数 / 带上下额外的行数。
constexpr size_t kAhdBand = 32;
constexpr size_t kAhdMargin = 3;

void ahd(const Padded& p, const uint8_t phase[4], const Mat3& toXyz, float* out, size_t w) {
    size_t s = p.stride;
    const float* d = p.data.data();
    // 填充坐标的颜色（PAD 为偶数，相位不变）。
    auto color = [&](size_t px, size_t py) -> uint8_t { return phase[(py & 1) * 2 + (px & 1)]; };
    const float white[3] = {0.9642f, 1.0f, 0.8249f};
    float m[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) m[i][j] = static_cast<float>(toXyz[i][j]) / white[i];
    LabF labf;
    size_t totalRows = p.h;
    size_t bands = (totalRows + kAhdBand - 1) / kAhdBand;
    for (size_t band = 0; band < bands; ++band) {
        size_t rows = std::min(kAhdBand, totalRows - band * kAhdBand);
        size_t n = rows + 2 * kAhdMargin;
        auto py = [&](size_t b) { return band * kAhdBand + kDemosaicPad - kAhdMargin + b; };
        // 1. 绿色水平 (0) / 垂直 (1) 插值，列 2..s-2。
        std::vector<float> g[2] = {std::vector<float>(n * s, 0.0f), std::vector<float>(n * s, 0.0f)};
        for (size_t b = 0; b < n; ++b) {
            size_t y = py(b);
            for (size_t x = 2; x + 2 < s; ++x) {
                size_t i = y * s + x;
                float c = d[i];
                size_t k = b * s + x;
                if (color(x, y) == 1) {
                    g[0][k] = c;
                    g[1][k] = c;
                    continue;
                }
                float l = d[i - 1], r = d[i + 1];
                float gh = (l + r) * 0.5f + (2.0f * c - d[i - 2] - d[i + 2]) * 0.25f;
                g[0][k] = std::clamp(gh, std::min(l, r), std::max(l, r));
                float u = d[i - s], dn = d[i + s];
                float gv = (u + dn) * 0.5f + (2.0f * c - d[i - 2 * s] - d[i + 2 * s]) * 0.25f;
                g[1][k] = std::clamp(gv, std::min(u, dn), std::max(u, dn));
            }
        }
        // 2. RGB 候选与其 Lab，带行 1..n-1，列 3..s-3。
        std::vector<float> rgb[2] = {std::vector<float>(n * s * 3, 0.0f), std::vector<float>(n * s * 3, 0.0f)};
        std::vector<float> lab[2] = {std::vector<float>(n * s * 3, 0.0f), std::vector<float>(n * s * 3, 0.0f)};
        for (int dir = 0; dir < 2; ++dir) {
            const float* gd = g[dir].data();
            auto diffv = [&](size_t b, size_t x) { return d[py(b) * s + x] - gd[b * s + x]; };
            float* rgbD = rgb[dir].data();
            float* labD = lab[dir].data();
            for (size_t b = 1; b + 1 < n; ++b) {
                size_t y = py(b);
                for (size_t x = 3; x + 3 < s; ++x) {
                    uint8_t own = color(x, y);
                    float gv = gd[b * s + x];
                    float px[3] = {gv, gv, gv};
                    if (own == 1) {
                        uint8_t hc = color(x + 1, y) & 3;
                        if (hc > 2) hc = 2;
                        px[hc] = gv + 0.5f * (diffv(b, x - 1) + diffv(b, x + 1));
                        px[2 - hc] = gv + 0.5f * (diffv(b - 1, x) + diffv(b + 1, x));
                    } else {
                        size_t o = own > 2 ? 2 : own;
                        px[o] = d[y * s + x];
                        px[2 - o] = gv + 0.25f * (diffv(b - 1, x - 1) + diffv(b - 1, x + 1) + diffv(b + 1, x - 1) + diffv(b + 1, x + 1));
                    }
                    for (float& v : px) v = std::clamp(v, 0.0f, 1.0f);
                    size_t i = (b * s + x) * 3;
                    for (int k = 0; k < 3; ++k) rgbD[i + k] = px[k];
                    float xyz[3];
                    for (int k = 0; k < 3; ++k) xyz[k] = m[k][0] * px[0] + m[k][1] * px[1] + m[k][2] * px[2];
                    float fx = labf.f(xyz[0]), fy = labf.f(xyz[1]), fz = labf.f(xyz[2]);
                    labD[i] = 116.0f * fy - 16.0f;
                    labD[i + 1] = 500.0f * (fx - fy);
                    labD[i + 2] = 200.0f * (fy - fz);
                }
            }
        }
        // 3. 同质性图，带行 2..n-2，列 4..s-4。
        std::vector<uint8_t> hom[2] = {std::vector<uint8_t>(n * s, 0), std::vector<uint8_t>(n * s, 0)};
        auto at3 = [&](const std::vector<float>& l, size_t k) {
            return std::make_tuple(l[k * 3], l[k * 3 + 1], l[k * 3 + 2]);
        };
        auto dist = [](const std::tuple<float, float, float>& a, const std::tuple<float, float, float>& c) {
            return std::make_pair(std::abs(std::get<0>(a) - std::get<0>(c)),
                                  (std::get<1>(a) - std::get<1>(c)) * (std::get<1>(a) - std::get<1>(c)) +
                                      (std::get<2>(a) - std::get<2>(c)) * (std::get<2>(a) - std::get<2>(c)));
        };
        for (size_t b = 2; b + 2 < n; ++b) {
            for (size_t x = 4; x + 4 < s; ++x) {
                size_t k = b * s + x;
                size_t nb[4] = {k - 1, k + 1, k - s, k + s};
                auto ch = at3(lab[0], k);
                auto cv = at3(lab[1], k);
                auto dh0 = dist(ch, at3(lab[0], nb[0])), dh1 = dist(ch, at3(lab[0], nb[1]));
                auto dh2 = dist(ch, at3(lab[0], nb[2])), dh3 = dist(ch, at3(lab[0], nb[3]));
                auto dv0 = dist(cv, at3(lab[1], nb[0])), dv1 = dist(cv, at3(lab[1], nb[1]));
                auto dv2 = dist(cv, at3(lab[1], nb[2])), dv3 = dist(cv, at3(lab[1], nb[3]));
                // 水平候选沿行判定，垂直候选沿列判定。
                float epsL = std::min(std::max(dh0.first, dh1.first), std::max(dv2.first, dv3.first));
                float epsC = std::min(std::max(dh0.second, dh1.second), std::max(dv2.second, dv3.second));
                int h0 = 0, h1 = 0;
                for (auto* dp : {&dh0, &dh1, &dh2, &dh3})
                    if (dp->first <= epsL && dp->second <= epsC) ++h0;
                for (auto* dp : {&dv0, &dv1, &dv2, &dv3})
                    if (dp->first <= epsL && dp->second <= epsC) ++h1;
                hom[0][k] = static_cast<uint8_t>(h0);
                hom[1][k] = static_cast<uint8_t>(h1);
            }
        }
        // 4. 每像素取 3×3 同质性和更大的候选。
        for (size_t r = 0; r < rows; ++r) {
            size_t b = r + kAhdMargin;
            for (size_t x = 0; x < w; ++x) {
                size_t px = x + kDemosaicPad;
                uint32_t sh = 0, sv = 0;
                for (size_t bb = b - 1; bb <= b + 1; ++bb)
                    for (size_t xx = px - 1; xx <= px + 1; ++xx) {
                        sh += hom[0][bb * s + xx];
                        sv += hom[1][bb * s + xx];
                    }
                size_t i = (b * s + px) * 3;
                float* o = out + ((band * kAhdBand + r) * w + x) * 3;
                for (int k = 0; k < 3; ++k) {
                    if (sh > sv) {
                        o[k] = rgb[0][i + k];
                    } else if (sh < sv) {
                        o[k] = rgb[1][i + k];
                    } else {
                        o[k] = 0.5f * (rgb[0][i + k] + rgb[1][i + k]);
                    }
                }
            }
        }
    }
}

}  // namespace

std::vector<float> demosaicBayer(const Padded& p, const uint8_t phase[4], Demosaic method, const Mat3& toXyz) {
    size_t w = p.w, h = p.h, s = p.stride;
    std::vector<float> out(w * h * 3, 0.0f);
    if (w == 0 || h == 0 || p.data.size() < s * (h + 2 * kDemosaicPad)) return out;
    if (method == Demosaic::Ahd) {
        ahd(p, phase, toXyz, out.data(), w);
        return out;
    }
    Taps ks = kernels(phase, method, s);
    const float* d = p.data.data();
    for (size_t y = 0; y < h; ++y) {
        size_t base = (y + kDemosaicPad) * s + kDemosaicPad;
        for (size_t x = 0; x < w; ++x) {
            float* o = out.data() + (y * w + x) * 3;
            size_t pos = (y & 1) * 2 + (x & 1);
            size_t i = base + x;
            if (method == Demosaic::Bilinear) {
                // 直接 Bayer 双线性（核结果的手写展开）。
                size_t own = std::min<size_t>(phase[pos], 2);
                float n = d[i - s], so = d[i + s], e = d[i + 1], wv = d[i - 1];
                if (own == 1) {
                    size_t hc = std::min<size_t>(phase[(y & 1) * 2 + ((x + 1) & 1)], 2);
                    o[1] = d[i];
                    o[hc] = (e + wv) * 0.5f;
                    o[2 - hc] = (n + so) * 0.5f;
                } else {
                    o[own] = d[i];
                    o[1] = (n + so + e + wv) * 0.25f;
                    o[2 - own] = (d[i - s - 1] + d[i - s + 1] + d[i + s - 1] + d[i + s + 1]) * 0.25f;
                }
                continue;
            }
            for (size_t c = 0; c < 3; ++c) {
                float v = 0.0f;
                for (const auto& tap : ks[pos][c]) v += tap.second * d[static_cast<size_t>(static_cast<long long>(i) + tap.first)];
                o[c] = std::clamp(v, 0.0f, 1.0f);
            }
        }
    }
    return out;
}

namespace {

// 一个非绿位的插值轴。
struct Axis {
    // 轴上距离 1 的绿邻。
    std::vector<std::pair<long long, long long>> near;
    // 轴上的同色样本对 (a, b, 1/距离)。
    std::vector<std::tuple<std::pair<long long, long long>, std::pair<long long, long long>, float>> pairs;
};

// 一个相位的抽头。
struct TapsCfa {
    uint8_t own = 0;
    Axis axes[2];
    // 无轴有绿邻时的 3×3（或 5×5）窗口绿。
    std::vector<std::pair<long long, long long>> greens;
    // 每颜色在窗口内的位（权重 1/距离²）。
    std::vector<std::pair<std::pair<long long, long long>, float>> sites[3];
};

TapsCfa phaseTaps(const DemosaicPattern& pat, long long px, long long py) {
    auto col = [&](long long dx, long long dy) { return pat.at(px + dx, py + dy); };
    constexpr long long R = 2;  // 通用去马赛克的抽头半径（5×5 窗）
    std::vector<std::pair<long long, long long>> window;
    for (long long dy = -R; dy <= R; ++dy)
        for (long long dx = -R; dx <= R; ++dx) window.emplace_back(dx, dy);
    TapsCfa t;
    const long long axes[2][2] = {{1, 0}, {0, 1}};
    for (int ai = 0; ai < 2; ++ai) {
        long long ax = axes[ai][0], ay = axes[ai][1];
        Axis& axis = t.axes[ai];
        for (auto sgn : {1ll, -1ll})
            if (col(ax * sgn, ay * sgn) == 1) axis.near.emplace_back(ax * sgn, ay * sgn);
        for (const auto& w : window) {
            long long dx = w.first, dy = w.second;
            // 轴线或其相邻线上的对。
            if ((ax == 1 && std::abs(dy) > 1) || (ay == 1 && std::abs(dx) > 1)) continue;
            for (long long k = 1; k <= 2; ++k) {
                long long bx = dx + k * ax, by = dy + k * ay;
                if (std::abs(bx) <= R && std::abs(by) <= R && col(dx, dy) == col(bx, by)) {
                    axis.pairs.emplace_back(std::make_pair(dx, dy), std::make_pair(bx, by), 1.0f / static_cast<float>(k));
                }
            }
        }
    }
    auto ring = [&](long long r) {
        std::vector<std::pair<long long, long long>> out;
        for (const auto& w : window) {
            long long dx = w.first, dy = w.second;
            if (std::abs(dx) <= r && std::abs(dy) <= r && !(dx == 0 && dy == 0) && col(dx, dy) == 1) out.push_back(w);
        }
        return out;
    };
    t.greens = ring(1);
    if (t.greens.empty()) t.greens = ring(R);
    for (uint8_t c = 0; c < 3; ++c) {
        for (const auto& w : window) {
            long long dx = w.first, dy = w.second;
            if (!(dx == 0 && dy == 0) && col(dx, dy) == c) {
                t.sites[c].emplace_back(w, 1.0f / static_cast<float>(dx * dx + dy * dy));
            }
        }
    }
    t.own = col(0, 0);
    return t;
}

}  // namespace

std::vector<float> demosaicCfa(const Padded& p, const DemosaicPattern& pat) {
    size_t w = p.w, h = p.h, s = p.stride;
    std::vector<float> out(w * h * 3, 0.0f);
    if (w == 0 || h == 0 || pat.w == 0 || pat.h == 0 || pat.colors.size() < pat.w * pat.h ||
        p.data.size() < s * (h + 2 * kDemosaicPad)) {
        return out;
    }
    std::vector<TapsCfa> taps;
    taps.reserve(pat.w * pat.h);
    for (size_t i = 0; i < pat.w * pat.h; ++i)
        taps.push_back(phaseTaps(pat, static_cast<long long>(i % pat.w), static_cast<long long>(i / pat.w)));
    auto tapAt = [&](size_t x, size_t y) -> const TapsCfa& { return taps[(y % pat.h) * pat.w + x % pat.w]; };
    const float* d = p.data.data();
    // (x+dx, y+dy) 处的样本，图像外为空。
    auto raw = [&](size_t x, size_t y, std::pair<long long, long long> o) -> std::optional<float> {
        long long xx = static_cast<long long>(x) + o.first;
        long long yy = static_cast<long long>(y) + o.second;
        if (xx < 0 || yy < 0 || xx >= static_cast<long long>(w) || yy >= static_cast<long long>(h)) return std::nullopt;
        return d[(static_cast<size_t>(yy) + kDemosaicPad) * s + kDemosaicPad + static_cast<size_t>(xx)];
    };

    // 1. 全域绿。
    std::vector<float> green(w * h, 0.0f);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const TapsCfa& t = tapAt(x, y);
            float v = 0.0f;
            if (auto c = raw(x, y, {0, 0})) v = *c;
            if (t.own == 1) {
                green[y * w + x] = v;
                continue;
            }
            float num = 0.0f, den = 0.0f;
            for (const Axis& a : t.axes) {
                float sum = 0.0f;
                uint32_t n = 0;
                for (const auto& o : a.near) {
                    if (auto val = raw(x, y, o)) {
                        sum += *val;
                        ++n;
                    }
                }
                if (n == 0) continue;
                float grad = 0.0f, m = 0.0f;
                for (const auto& pr : a.pairs) {
                    auto va = raw(x, y, std::get<0>(pr));
                    auto vb = raw(x, y, std::get<1>(pr));
                    if (va && vb) {
                        grad += std::abs(*va - *vb) * std::get<2>(pr);
                        m += 1.0f;
                    }
                }
                grad = m > 0.0f ? grad / m : 0.0f;
                float wt = 1.0f / ((kGradEps + grad) * (kGradEps + grad));
                num += wt * sum / static_cast<float>(n);
                den += wt;
            }
            if (den > 0.0f) {
                green[y * w + x] = num / den;
            } else {
                float sum = 0.0f;
                uint32_t n = 0;
                for (const auto& o : t.greens) {
                    if (auto val = raw(x, y, o)) {
                        sum += *val;
                        ++n;
                    }
                }
                green[y * w + x] = n > 0 ? sum / static_cast<float>(n) : v;
            }
        }
    }

    // 2. 色差法求红与蓝。
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const TapsCfa& t = tapAt(x, y);
            float g = y * w + x < green.size() ? green[y * w + x] : 0.0f;
            float v = 0.0f;
            if (auto c = raw(x, y, {0, 0})) v = *c;
            float* o = out.data() + (y * w + x) * 3;
            o[1] = g;
            auto gat = [&](size_t gx, size_t gy, std::pair<long long, long long> off) -> std::optional<float> {
                long long xx = static_cast<long long>(gx) + off.first;
                long long yy = static_cast<long long>(gy) + off.second;
                if (xx < 0 || yy < 0 || xx >= static_cast<long long>(w) || yy >= static_cast<long long>(h)) return std::nullopt;
                return green[static_cast<size_t>(yy) * w + static_cast<size_t>(xx)];
            };
            for (size_t ci : {0u, 2u}) {
                if (t.own == ci) {
                    o[ci] = v;
                    continue;
                }
                float num = 0.0f, den = 0.0f;
                for (const auto& site : t.sites[ci]) {
                    auto vq = raw(x, y, site.first);
                    auto gq = gat(x, y, site.first);
                    if (vq && gq) {
                        float wt = site.second / (kDiffEps + std::abs(*gq - g));
                        num += wt * (*vq - *gq);
                        den += wt;
                    }
                }
                o[ci] = den > 0.0f ? g + num / den : g;
            }
            for (int k = 0; k < 3; ++k) o[k] = std::clamp(o[k], 0.0f, 1.0f);
        }
    }
    return out;
}

}  // namespace raw
}  // namespace montage
