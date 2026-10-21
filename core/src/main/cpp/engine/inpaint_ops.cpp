// 孔洞填充内核实现（M19a）。photocraft algo/inpaint.rs 逐式翻译（MIT OR Apache-2.0）。
// 单线程带状扫掠 = photocraft 的规范序（其测试证明结果与线程数无关——各 16 行带独立
// 传播、每带独立播种 SplitMix64）；并联 wavefront/lanes 等纯提速路径未翻译。
// 宿主测试 .debug/m19/test_inpaint.cpp 以「优化实现 vs 朴素扫描位级一致」锁定 onion peel，
// 以统计阈值（rmse）锁定 complete/synthesize，与 photocraft 的 oracle 同构。

#include "inpaint_ops.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>

#include "poisson_ops.h"

namespace montage {
namespace inpaint {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr int32_t kMinI32 = std::numeric_limits<int32_t>::min();
// 每通道矩保留的通道上限（颜色适配拟合的通道数，其余原样拷贝）。
constexpr size_t kMomCh = 8;
// Sources 索引的列块宽。
constexpr size_t kSourceBlock = 64;
// 保留源矩的层像素上限。
constexpr size_t kSourceMomentsMax = size_t{1} << 20;

bool cancelled(const CancelFn& c) {
    return c && c();
}

std::pair<int, int> invalidField() {
    return {kMinI32, kMinI32};
}

}  // namespace

// ---- Rng (SplitMix64) ----

Rng Rng::seeded(uint64_t seed) {
    return Rng{seed ^ 0x9E3779B97F4A7C15ull};
}

uint64_t Rng::next() {
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Rng::skip(uint64_t n) {
    state += n * 0x9E3779B97F4A7C15ull;
}

int Rng::range(int lo, int hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + static_cast<int>(next() % static_cast<uint64_t>(hi - lo + 1));
}

// ---- 几何与积分图 ----

namespace {

struct Bounds {
    size_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    size_t width() const { return x1 - x0; }
    size_t height() const { return y1 - y0; }
    size_t area() const { return width() * height(); }
    // 各边外扩 r，夹在 w×h 格内。
    Bounds grown(size_t r, size_t w, size_t h) const {
        return Bounds{ x0 > r ? x0 - r : 0, y0 > r ? y0 - r : 0, std::min(x1 + r, w),
                       std::min(y1 + r, h) };
    }
    // 下一粗层（其格胞为本层 2×2）上的同一盒子。
    Bounds halved() const {
        return Bounds{x0 / 2, y0 / 2, (x1 + 1) / 2, (y1 + 1) / 2};
    }
};

std::optional<Bounds> holeBoundsOf(size_t w, size_t h, const std::vector<Px>& px) {
    std::optional<Bounds> b;
    for (size_t y = 0; y < h; ++y) {
        size_t x0 = 0, x1 = 0;
        bool has = false;
        for (size_t x = 0; x < w; ++x) {
            if (px[y * w + x] == Px::Hole) {
                if (!has) {
                    x0 = x;
                    has = true;
                }
                x1 = x + 1;
            }
        }
        if (has) {
            b = b ? Bounds{std::min(b->x0, x0), b->y0, std::max(b->x1, x1), y + 1}
                  : Bounds{x0, y, x1, y + 1};
        }
    }
    return b;
}

}  // namespace

std::vector<uint32_t> integralOf(size_t w, size_t h, const std::vector<bool>& m) {
    std::vector<uint32_t> s((w + 1) * (h + 1), 0);
    for (size_t y = 0; y < h; ++y) {
        uint32_t row = 0;
        for (size_t x = 0; x < w; ++x) {
            row += m[y * w + x] ? 1u : 0u;
            s[(y + 1) * (w + 1) + x + 1] = s[y * (w + 1) + x + 1] + row;
        }
    }
    return s;
}

uint32_t windowCount(const std::vector<uint32_t>& s, size_t gw, size_t gh, int x0, int y0, int x1,
                     int y1) {
    const size_t cx0 = static_cast<size_t>(std::max(x0, 0));
    const size_t cy0 = static_cast<size_t>(std::max(y0, 0));
    const size_t cx1 = std::min(static_cast<size_t>(std::max(x1, 0)), gw);
    const size_t cy1 = std::min(static_cast<size_t>(std::max(y1, 0)), gh);
    if (cx1 <= cx0 || cy1 <= cy0) {
        return 0;
    }
    const size_t w1 = gw + 1;
    return s[cy1 * w1 + cx1] + s[cy0 * w1 + cx0] - s[cy0 * w1 + cx1] - s[cy1 * w1 + cx0];
}

namespace {

// 布尔网格积分图（限定孔洞包围盒域，proximity_integral.rs 的 HoleIntegral）。
struct HoleIntegral {
    int ox = 0, oy = 0;
    size_t w = 0, h = 0;
    std::vector<uint32_t> data;

    HoleIntegral(size_t stride, const std::vector<bool>& hole, Bounds box) {
        w = box.width();
        h = box.height();
        ox = static_cast<int>(box.x0);
        oy = static_cast<int>(box.y0);
        const size_t rowStride = w + 1;
        data.assign(rowStride * (h + 1), 0);
        for (size_t y = 0; y < h; ++y) {
            uint32_t count = 0;
            for (size_t x = 0; x < w; ++x) {
                count += hole[(box.y0 + y) * stride + box.x0 + x] ? 1u : 0u;
                data[y * rowStride + x + 1] = 0;  // 已清零
                data[(y + 1) * rowStride + x + 1] = data[y * rowStride + x + 1] + count;
            }
        }
    }

    uint32_t count(int x0, int y0, int x1, int y1) const {
        return windowCount(data, w, h, x0 - ox, y0 - oy, x1 - ox, y1 - oy);
    }
};

}  // namespace

// ---- 有效源中心 ----

std::vector<bool> validSources(size_t w, size_t h, int r, const std::vector<Px>& px) {
    const size_t side = static_cast<size_t>(2 * r + 1);
    std::vector<bool> run(w * h, false);
    if (w < side || h < side) {
        return run;
    }
    // 逐行：以 x 为中心的 side 像素横向窗口是否全可拷贝（滑动计数）。
    for (size_t y = 0; y < h; ++y) {
        const Px* row = &px[y * w];
        auto copyable = [&](size_t x) -> size_t { return row[x] == Px::Source ? 1 : 0; };
        size_t count = 0;
        for (size_t x = 0; x < side; ++x) {
            count += copyable(x);
        }
        if (r < w) {
            run[y * w + r] = count == side;
        }
        for (size_t x = r + 1; x + r < w; ++x) {
            count = count + copyable(x + r) - copyable(x - r - 1);
            run[y * w + x] = count == side;
        }
    }
    // 逐列：side 个横向运行纵向叠满即有效。
    std::vector<bool> valid(w * h, false);
    std::vector<size_t> stack(w, 0);
    for (size_t yy = 0; yy < h; ++yy) {
        for (size_t x = 0; x < w; ++x) {
            stack[x] = run[yy * w + x] ? stack[x] + 1 : 0;
        }
        if (yy < static_cast<size_t>(2 * r)) {
            continue;  // 中心 y = yy − r 须 ≥ r
        }
        const size_t y = yy - static_cast<size_t>(r);
        for (size_t x = r; x + r < w; ++x) {
            valid[y * w + x] = stack[x] >= side;
        }
    }
    return valid;
}

namespace {

// 有效源中心索引：行前缀 + 64 列块前缀（第 k 个中心免全列）。
struct Sources {
    std::vector<size_t> rowStart;    // 每行前的有效中心数（h+1 项）
    std::vector<uint32_t> blockStart;  // 每行每块前的有效中心数
    size_t blocks = 0;

    Sources(const std::vector<bool>& valid, size_t w) {
        const size_t hh = valid.size() / w;
        blocks = (w + kSourceBlock - 1) / kSourceBlock;
        blockStart.assign(hh * blocks, 0);
        rowStart.reserve(hh + 1);
        rowStart.push_back(0);
        size_t total = 0;
        for (size_t y = 0; y < hh; ++y) {
            uint32_t n = 0;
            for (size_t b = 0; b < blocks; ++b) {
                blockStart[y * blocks + b] = n;
                const size_t lo = y * w + b * kSourceBlock;
                const size_t hi = y * w + std::min((b + 1) * kSourceBlock, w);
                n += static_cast<uint32_t>(std::count(valid.begin() + static_cast<long>(lo),
                                                      valid.begin() + static_cast<long>(hi), true));
            }
            total += n;
            rowStart.push_back(total);
        }
    }

    size_t len() const { return rowStart.empty() ? 0 : rowStart.back(); }

    std::pair<int, int> nth(size_t k, const std::vector<bool>& valid, size_t w) const {
        size_t y = static_cast<size_t>(
            std::partition_point(rowStart.begin(), rowStart.end(),
                                 [&](size_t s) { return s <= k; }) - rowStart.begin());
        y = y == 0 ? 0 : y - 1;
        size_t left = k - rowStart[y];
        const uint32_t* rb = &blockStart[y * blocks];
        size_t b = static_cast<size_t>(
            std::partition_point(rb, rb + blocks, [&](uint32_t s) {
                return static_cast<size_t>(s) <= left;
            }) - rb);
        b = b == 0 ? 0 : b - 1;
        left -= rb[b];
        const size_t x0 = b * kSourceBlock;
        for (size_t x = x0; x < w; ++x) {
            if (!valid[y * w + x]) {
                continue;
            }
            if (left == 0) {
                return {static_cast<int>(x), static_cast<int>(y)};
            }
            --left;
        }
        return {0, 0};
    }

    std::vector<std::pair<int, int>> all(const std::vector<bool>& valid, size_t w) const {
        std::vector<std::pair<int, int>> out;
        out.reserve(len());
        for (size_t i = 0; i < valid.size(); ++i) {
            if (valid[i]) {
                out.emplace_back(static_cast<int>(i % w), static_cast<int>(i / w));
            }
        }
        return out;
    }
};

// tb 格胞：其补丁是否与孔相交（目标中心）。
std::vector<bool> targetsAround(size_t w, int r, const std::vector<Px>& px, const Bounds& hb,
                                const Bounds& tb) {
    const size_t bw = hb.width(), bh = hb.height();
    std::vector<uint32_t> s((bw + 1) * (bh + 1), 0);
    for (size_t y = 0; y < bh; ++y) {
        uint32_t row = 0;
        for (size_t x = 0; x < bw; ++x) {
            row += px[(hb.y0 + y) * w + hb.x0 + x] == Px::Hole ? 1u : 0u;
            s[(y + 1) * (bw + 1) + x + 1] = s[y * (bw + 1) + x + 1] + row;
        }
    }
    std::vector<bool> targets(tb.area(), false);
    const int ri = r;
    const int ox = static_cast<int>(hb.x0), oy = static_cast<int>(hb.y0);
    for (size_t y = tb.y0; y < tb.y1; ++y) {
        for (size_t x = tb.x0; x < tb.x1; ++x) {
            const size_t k = (y - tb.y0) * tb.width() + (x - tb.x0);
            const int xi = static_cast<int>(x), yi = static_cast<int>(y);
            targets[k] = windowCount(s, bw, bh, xi - ri - ox, yi - ri - oy, xi + ri + 1 - ox,
                                     yi + ri + 1 - oy) > 0;
        }
    }
    return targets;
}

// 补丁矩（patch_moments）：左上 (x0,y0) 的 side×side 补丁逐通道 Σv, Σv²，逐行逐像素求和。
void patchMoments(const std::vector<float>& img, size_t w, size_t ch, size_t c8, size_t side,
                  size_t x0, size_t y0, float* out /* c8 组 [和, 平方和] */) {
    for (size_t c = 0; c < c8; ++c) {
        out[c * 2] = 0.0f;
        out[c * 2 + 1] = 0.0f;
    }
    for (size_t y = y0; y < y0 + side; ++y) {
        const float* row = &img[(y * w + x0) * ch];
        for (size_t p = 0; p < side; ++p) {
            for (size_t c = 0; c < c8; ++c) {
                const float a = row[p * ch + c];
                out[c * 2] += a;
                out[c * 2 + 1] += a * a;
            }
        }
    }
}

// 目标矩 (Σt, Σt²) 对源矩 (Σs, Σs²)（n 样本）拟合 gain/bias（限幅内）。
inline void fitMoment(const Adapt& a, float n, float st, float stt, float ss, float sss,
                      float* g, float* b) {
    const float mt = st / n, ms = ss / n;
    const float vt = std::max(stt / n - mt * mt, 0.0f);
    const float vs = std::max(sss / n - ms * ms, 0.0f);
    float gain = vs > 1e-10f ? std::sqrt(vt / vs) : 1.0f;
    gain = std::clamp(gain, a.gainLo, a.gainHi);
    *g = gain;
    *b = std::clamp(mt - gain * ms, -a.bias, a.bias);
}

// 由各通道矩 [Σt, Σt², Σs, Σs², Σts]（n 样本）求适配后距离：源拟合后剩下的目标残差。
inline float adaptedCost(const Adapt& a, float n, const float (*m)[5], size_t chUsed) {
    float sum = 0.0f;
    for (size_t c = 0; c < chUsed; ++c) {
        float g, b;
        fitMoment(a, n, m[c][0], m[c][1], m[c][2], m[c][3], &g, &b);
        // Σ (t − g·s − b)² 展开
        sum += m[c][1] + g * g * m[c][3] + n * b * b - 2.0f * g * m[c][4] - 2.0f * b * m[c][0] +
               2.0f * g * b * m[c][2];
    }
    return std::max(sum, 0.0f) / (n * static_cast<float>(chUsed));
}

// Σ(t−s)²（目标补丁已知像素、偏移 offs、ch 通道）对源中心 base；达 limit 返回 nullopt。
inline std::optional<float> knownSse(const float* image, size_t ch, const std::vector<ptrdiff_t>& offs,
                                     const std::vector<float>& tv, ptrdiff_t base, float limit) {
    float e = 0.0f;
    size_t k = 0;
    for (ptrdiff_t o : offs) {
        const size_t si = static_cast<size_t>(base + o) * ch;
        for (size_t c = 0; c < ch; ++c) {
            const float d = tv[k + c] - image[si + c];
            e += d * d;
        }
        k += ch;
        if (e >= limit) {
            return std::nullopt;
        }
    }
    return e;
}

// 最小和 e ≥ 0 使 e/n > v（strict）或 float 算术下 e/n ≥ v；商随 e 单调不降。
float leastSumImpl(float v, float n, bool strict) {
    auto holds = [&](float e) { return strict ? e / n > v : e / n >= v; };
    if (!std::isfinite(v) || !std::isfinite(n) || n <= 0.0f) {
        return kInf;
    }
    float e = std::max(v * n, 0.0f);
    while (e > 0.0f && holds(std::nextafterf(e, -kInf))) {
        e = std::nextafterf(e, -kInf);
    }
    while (!holds(e) && e < kInf) {
        e = std::nextafterf(e, kInf);
    }
    return e;
}

// 洋葱剥皮的当前最优源。
struct PeelBest {
    float v = kInf;      // 每样本误差
    size_t j = SIZE_MAX;  // 源序位
    float worse = kInf;  // e/n 严格大于 v 的最小和
    float equal = kInf;  // e/n 至少 v（平局）的最小和
};

// 洋葱剥皮初始化（onion_peel）：从孔边缘向内逐环，每像素取周边已知域最佳匹配源块的中心
// 像素。优化语义（邻居种子候选 + 每 16 源 + 早停）与朴素顺序扫描位级一致（测试锁定）。
bool onionPeelImpl(size_t w, size_t h, size_t ch, int r, std::vector<float>& img,
                   const std::vector<Px>& px, const Bounds& hb,
                   const std::vector<std::pair<int, int>>& sources, const CancelFn& cancel) {
    constexpr uint32_t kNone = UINT32_MAX;
    std::vector<bool> known(w * h);
    for (size_t i = 0; i < w * h; ++i) {
        known[i] = px[i] == Px::Known || px[i] == Px::Source;
    }
    std::vector<uint32_t> index(w * h, kNone);
    for (size_t j = 0; j < sources.size(); ++j) {
        index[static_cast<size_t>(sources[j].second) * w + static_cast<size_t>(sources[j].first)] =
            static_cast<uint32_t>(j);
    }
    std::vector<uint32_t> took(w * h, kNone);
    const std::pair<int, int> neighbours[8] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
                                               {1, 0},  {-1, 1}, {0, 1},  {1, 1}};
    while (true) {
        if (cancelled(cancel)) {
            return false;
        }
        std::vector<size_t> front;
        for (size_t y = hb.y0; y < hb.y1; ++y) {
            for (size_t x = hb.x0; x < hb.x1; ++x) {
                const size_t i = y * w + x;
                if (known[i] || px[i] != Px::Hole) {
                    continue;
                }
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const long long nx = static_cast<long long>(x) + dx;
                        const long long ny = static_cast<long long>(y) + dy;
                        if (nx >= 0 && ny >= 0 && nx < static_cast<long long>(w) &&
                            ny < static_cast<long long>(h) &&
                            known[static_cast<size_t>(ny) * w + static_cast<size_t>(nx)]) {
                            edge = true;
                            break;
                        }
                    }
                }
                if (edge) {
                    front.push_back(i);
                }
            }
        }
        if (front.empty()) {
            break;
        }
        // 先算完整个 front（读 front 开始时的 img/known），再统一写回——同轮 front 内
        // 已填像素绝不污染后续像素的收集（photocraft par::map + 朴素扫描同语义）。
        std::vector<std::vector<float>> vals(front.size());
        std::vector<uint32_t> picks(front.size(), kNone);
        for (size_t fi = 0; fi < front.size(); ++fi) {
            const size_t i = front[fi];
            const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
            // 目标补丁的已知像素（规范扫描序收集一次）。
            const size_t side = static_cast<size_t>(2 * r + 1);
            std::vector<ptrdiff_t> offs;
            std::vector<float> tv;
            offs.reserve(side * side);
            tv.reserve(side * side * ch);
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    const int tx = x + dx, ty = y + dy;
                    if (tx >= 0 && ty >= 0 && static_cast<size_t>(tx) < w &&
                        static_cast<size_t>(ty) < h &&
                        known[static_cast<size_t>(ty) * w + static_cast<size_t>(tx)]) {
                        offs.push_back(static_cast<ptrdiff_t>(dy) * static_cast<ptrdiff_t>(w) + dx);
                        const size_t ti = (static_cast<size_t>(ty) * w + static_cast<size_t>(tx)) * ch;
                        tv.insert(tv.end(), img.begin() + static_cast<long>(ti),
                                  img.begin() + static_cast<long>(ti + ch));
                    }
                }
            }
            PeelBest best;
            if (!offs.empty()) {
                const float nf = static_cast<float>(offs.size());
                auto baseOf = [&](size_t j) -> ptrdiff_t {
                    if (j >= sources.size()) {
                        return 0;
                    }
                    return static_cast<ptrdiff_t>(sources[j].second) * static_cast<ptrdiff_t>(w) +
                           sources[j].first;
                };
                auto limitOf = [&](size_t j, const PeelBest& b) {
                    return j > b.j ? b.equal : b.worse;
                };
                auto adopt = [&](size_t j, float e, PeelBest& b) {
                    const float v = e / nf;
                    if (v < kInf && (v < b.v || (v == b.v && j < b.j))) {
                        PeelBest nb;
                        nb.v = v;
                        nb.j = j;
                        nb.worse = leastSumImpl(v, nf, true);
                        nb.equal = leastSumImpl(v, nf, false);
                        b = nb;
                    }
                };
                auto consider = [&](size_t j, PeelBest& b) {
                    if (j >= sources.size()) {
                        return;
                    }
                    const float limit = limitOf(j, b);
                    const auto e = knownSse(img.data(), ch, offs, tv, baseOf(j), limit);
                    if (e) {
                        adopt(j, *e, b);
                    }
                };
                // 早界：已填邻居的源所指处（PatchMatch 传播同式）；孔边缘处每 16 源。
                for (const auto& [dx, dy] : neighbours) {
                    const int qx = x + dx, qy = y + dy;
                    if (qx < 0 || qy < 0 || static_cast<size_t>(qx) >= w ||
                        static_cast<size_t>(qy) >= h) {
                        continue;
                    }
                    const uint32_t tq = took[static_cast<size_t>(qy) * w + static_cast<size_t>(qx)];
                    if (tq >= sources.size()) {
                        continue;
                    }
                    const auto& src = sources[tq];
                    const int cx = src.first - dx, cy = src.second - dy;
                    if (cx >= 0 && cy >= 0 && static_cast<size_t>(cx) < w &&
                        static_cast<size_t>(cy) < h &&
                        index[static_cast<size_t>(cy) * w + static_cast<size_t>(cx)] != kNone) {
                        consider(index[static_cast<size_t>(cy) * w + static_cast<size_t>(cx)], best);
                    }
                }
                if (best.j == SIZE_MAX) {
                    for (size_t j = 0; j < sources.size(); j += 16) {
                        consider(j, best);
                    }
                }
                // 顺序全扫（早停只砍注定输的源：和单调增）。
                for (size_t j = 0; j < sources.size(); ++j) {
                    if (j % 4096 == 0 && j > 0 && cancelled(cancel)) {
                        return false;
                    }
                    consider(j, best);
                }
            }
            const auto& src = best.j < sources.size() ? sources[best.j]
                                                      : std::pair<int, int>{0, 0};
            const size_t si = (static_cast<size_t>(src.second) * w + static_cast<size_t>(src.first)) * ch;
            vals[fi].assign(img.begin() + static_cast<long>(si),
                            img.begin() + static_cast<long>(si + ch));
            picks[fi] = best.j == SIZE_MAX ? kNone : static_cast<uint32_t>(best.j);
        }
        for (size_t fi = 0; fi < front.size(); ++fi) {
            const size_t i = front[fi];
            std::memcpy(&img[i * ch], vals[fi].data(), ch * sizeof(float));
            known[i] = true;
            took[i] = picks[fi];
        }
        if (cancelled(cancel)) {
            return false;
        }
    }
    return true;
}

// 半尺寸层：任一子像素为孔则孔；均值为非 Void 子像素均值；全 Source 才可拷贝。
struct Level {
    size_t w = 0, h = 0;
    std::vector<float> img;
    std::vector<Px> px;
};

Level downsample(const Level& l, size_t ch) {
    Level out;
    out.w = (l.w + 1) / 2;
    out.h = (l.h + 1) / 2;
    out.img.assign(out.w * out.h * ch, 0.0f);
    out.px.assign(out.w * out.h, Px::Void);
    for (size_t y = 0; y < out.h; ++y) {
        for (size_t x = 0; x < out.w; ++x) {
            float n = 0.0f;
            // 平面缓冲行主序（photocraft 的 irow 行切片 + 行内偏移 x*ch 的等价式）
            const size_t o = (y * out.w + x) * ch;
            bool hole = false, allSource = true, any = false;
            for (auto [dx, dy] : {std::pair{0u, 0u}, {1u, 0u}, {0u, 1u}, {1u, 1u}}) {
                const size_t sx = x * 2 + dx, sy = y * 2 + dy;
                if (sx >= l.w || sy >= l.h) {
                    continue;
                }
                const size_t si = sy * l.w + sx;
                switch (l.px[si]) {
                    case Px::Hole:
                        hole = true;
                        break;
                    case Px::Void:
                        allSource = false;
                        break;
                    default:
                        any = true;
                        allSource = allSource && l.px[si] == Px::Source;
                        for (size_t c = 0; c < ch; ++c) {
                            out.img[o + c] += l.img[si * ch + c];
                        }
                        n += 1.0f;
                        break;
                }
            }
            if (n > 0.0f) {
                for (size_t c = 0; c < ch; ++c) {
                    out.img[o + c] /= n;
                }
            }
            out.px[y * out.w + x] = hole              ? Px::Hole
                                    : !any            ? Px::Void
                                    : allSource       ? Px::Source
                                                      : Px::Known;
        }
    }
    return out;
}

// 已完成的粗层：尺寸、像素、匹配场（目标盒逐格源中心，MIN = 无）。
struct Coarser {
    size_t w = 0, h = 0;
    std::vector<float> img;
    std::vector<std::pair<int, int>> field;
    Bounds tbox{};

    std::pair<int, int> fieldAt(size_t x, size_t y) const {
        if (x < tbox.x0 || x >= tbox.x1 || y < tbox.y0 || y >= tbox.y1) {
            return invalidField();
        }
        const size_t k = (y - tbox.y0) * tbox.width() + (x - tbox.x0);
        return k < field.size() ? field[k] : invalidField();
    }
};

// ---- Solver：单层 PatchMatch/EM 状态 ----

struct Solver {
    size_t w = 0, h = 0, ch = 0;
    int r = 0;
    std::vector<float>* img = nullptr;
    const std::vector<Px>* px = nullptr;
    std::vector<bool> valid;
    std::vector<bool> targets;  // 目标中心（补丁与孔相交）
    std::vector<bool> full;     // tb 格胞：整补丁在格内且非 Void
    std::vector<float> tmom;    // full 目标补丁自矩（适配时），[Σ, Σ²]×c8
    std::vector<float> smom;    // 源补丁矩（保留时）
    Bounds holeBox{}, tbox{};
    Adapt adapt{};
    CancelFn cancel;

    bool stop() const { return cancelled(cancel); }

    bool isValid(int x, int y) const {
        return x >= 0 && y >= 0 && static_cast<size_t>(x) < w && static_cast<size_t>(y) < h &&
               valid[static_cast<size_t>(y) * w + static_cast<size_t>(x)];
    }

    // 标记 full 目标补丁；适配时保留源矩（有回报时）。
    void prepare(bool keepSourceMoments) {
        const size_t ru = static_cast<size_t>(r);
        const size_t side = 2 * ru + 1;
        full.assign(tbox.area(), false);
        for (size_t ty = 0; ty < tbox.height(); ++ty) {
            const size_t y = tbox.y0 + ty;
            if (y < ru || y + ru >= h) {
                continue;
            }
            for (size_t tx = 0; tx < tbox.width(); ++tx) {
                const size_t x = tbox.x0 + tx;
                if (x < ru || x + ru >= w) {
                    continue;
                }
                bool ok = true;
                for (size_t yy = y - ru; ok && yy <= y + ru; ++yy) {
                    for (size_t xx = x - ru; xx <= x - ru + side - 1; ++xx) {
                        if ((*px)[yy * w + xx] == Px::Void) {
                            ok = false;
                            break;
                        }
                    }
                }
                full[ty * tbox.width() + tx] = ok;
            }
        }
        if (adapt.isNone()) {
            return;
        }
        const size_t c8 = std::min(ch, kMomCh);
        tmom.assign(tbox.area() * c8 * 2, 0.0f);
        if (!keepSourceMoments) {
            return;
        }
        smom.assign(w * h * c8 * 2, 0.0f);
        for (size_t y = 0; y < h; ++y) {
            if (stop()) {
                return;
            }
            for (size_t x = 0; x < w; ++x) {
                if (valid[y * w + x]) {
                    patchMoments(*img, w, ch, c8, side, x - ru, y - ru, &smom[(y * w + x) * c8 * 2]);
                }
            }
        }
    }

    float dist(const std::pair<int, int>& t, size_t k, const std::pair<int, int>& s,
               float cutoff) const {
        const bool fullP = full[k];
        if (!adapt.isNone()) {
            return fullP ? distAdaptedFull(k, t, s) : distAdapted(t, s);
        }
        return fullP ? distFull(t, s, cutoff) : distClipped(t, s, cutoff);
    }

    // 无适配、可裁剪补丁的距离（早停 cutoff）。
    float distClipped(const std::pair<int, int>& t, const std::pair<int, int>& s,
                      float cutoff) const {
        const int wi = static_cast<int>(w), hi = static_cast<int>(h);
        const float full = static_cast<float>((2 * r + 1) * (2 * r + 1) * static_cast<int>(ch));
        float sum = 0.0f;
        size_t n = 0;
        for (int dy = -r; dy <= r; ++dy) {
            const int ty = t.second + dy;
            if (ty < 0 || ty >= hi) {
                continue;
            }
            const int sy = s.second + dy;
            for (int dx = -r; dx <= r; ++dx) {
                const int tx = t.first + dx;
                if (tx < 0 || tx >= wi ||
                    (*px)[static_cast<size_t>(ty) * w + static_cast<size_t>(tx)] == Px::Void) {
                    continue;
                }
                const size_t ti = (static_cast<size_t>(ty) * w + static_cast<size_t>(tx)) * ch;
                const size_t si =
                    (static_cast<size_t>(sy) * w + static_cast<size_t>(s.first + dx)) * ch;
                for (size_t c = 0; c < ch; ++c) {
                    const float d = (*img)[ti + c] - (*img)[si + c];
                    sum += d * d;
                }
                n += ch;
            }
            if (sum > cutoff * full) {
                return kInf;
            }
        }
        return n == 0 ? kInf : sum / static_cast<float>(n);
    }

    // full 补丁距离：同序求和，超 cutoff 即弃（调用方只问是否更优）。
    float distFull(const std::pair<int, int>& t, const std::pair<int, int>& s, float cutoff) const {
        const size_t side = static_cast<size_t>(2 * r + 1);
        const float full = static_cast<float>((2 * r + 1) * (2 * r + 1) * static_cast<int>(ch));
        const size_t span = side * ch;
        const float limit = cutoff * full;
        float sum = 0.0f;
        for (int dy = -r; dy <= r; ++dy) {
            const size_t ti =
                (static_cast<size_t>(t.second + dy) * w + static_cast<size_t>(t.first - r)) * ch;
            const size_t si =
                (static_cast<size_t>(s.second + dy) * w + static_cast<size_t>(s.first - r)) * ch;
            for (size_t p = 0; p < span; p += ch) {
                for (size_t c = 0; c < ch; ++c) {
                    const float d = (*img)[ti + p + c] - (*img)[si + p + c];
                    sum += d * d;
                }
                if (sum > limit) {
                    return kInf;
                }
            }
        }
        return sum / static_cast<float>(side * side * ch);
    }

    // 目标补丁 t 对源补丁 s 的逐通道矩 [Σt, Σt², Σs, Σs², Σts]，返回样本数。
    size_t moments(const std::pair<int, int>& t, const std::pair<int, int>& s,
                   float (*m)[5]) const {
        const int wi = static_cast<int>(w), hi = static_cast<int>(h);
        const size_t c8 = std::min(ch, kMomCh);
        for (size_t c = 0; c < c8; ++c) {
            for (int q = 0; q < 5; ++q) {
                m[c][q] = 0.0f;
            }
        }
        size_t n = 0;
        for (int dy = -r; dy <= r; ++dy) {
            const int ty = t.second + dy;
            if (ty < 0 || ty >= hi) {
                continue;
            }
            const int sy = s.second + dy;
            for (int dx = -r; dx <= r; ++dx) {
                const int tx = t.first + dx;
                if (tx < 0 || tx >= wi ||
                    (*px)[static_cast<size_t>(ty) * w + static_cast<size_t>(tx)] == Px::Void) {
                    continue;
                }
                const size_t ti = (static_cast<size_t>(ty) * w + static_cast<size_t>(tx)) * ch;
                const size_t si =
                    (static_cast<size_t>(sy) * w + static_cast<size_t>(s.first + dx)) * ch;
                for (size_t c = 0; c < c8; ++c) {
                    const float a = (*img)[ti + c], b = (*img)[si + c];
                    m[c][0] += a;
                    m[c][1] += a * a;
                    m[c][2] += b;
                    m[c][3] += b * b;
                    m[c][4] += a * b;
                }
                n += 1;
            }
        }
        return n;
    }

    // full 目标补丁矩（k 索引 tbox 表）：目标取 costs 缓存，源取 prepare 保留（同序求和）。
    void momentsFull(size_t k, const std::pair<int, int>& t, const std::pair<int, int>& s,
                     float (*m)[5]) const {
        const size_t c8 = std::min(ch, kMomCh);
        const size_t side = static_cast<size_t>(2 * r + 1);
        const float* tm = &tmom[k * c8 * 2];
        const size_t sIdx = (static_cast<size_t>(s.second) * w + static_cast<size_t>(s.first)) * c8 * 2;
        const bool hasSm = !smom.empty();
        float acc[kMomCh][3] = {};
        for (int dy = -r; dy <= r; ++dy) {
            const size_t ti =
                (static_cast<size_t>(t.second + dy) * w + static_cast<size_t>(t.first - r)) * ch;
            const size_t si =
                (static_cast<size_t>(s.second + dy) * w + static_cast<size_t>(s.first - r)) * ch;
            for (size_t p = 0; p < side; ++p) {
                for (size_t c = 0; c < c8; ++c) {
                    const float a = (*img)[ti + p * ch + c], b = (*img)[si + p * ch + c];
                    if (hasSm) {
                        acc[c][2] += a * b;
                    } else {
                        acc[c][0] += b;
                        acc[c][1] += b * b;
                        acc[c][2] += a * b;
                    }
                }
            }
        }
        for (size_t c = 0; c < c8; ++c) {
            const float ss = hasSm ? smom[sIdx + c * 2] : acc[c][0];
            const float sss = hasSm ? smom[sIdx + c * 2 + 1] : acc[c][1];
            m[c][0] = tm[c * 2];
            m[c][1] = tm[c * 2 + 1];
            m[c][2] = ss;
            m[c][3] = sss;
            m[c][4] = acc[c][2];
        }
    }

    float distAdapted(const std::pair<int, int>& t, const std::pair<int, int>& s) const {
        float m[kMomCh][5];
        const size_t c8 = std::min(ch, kMomCh);
        const size_t n = moments(t, s, m);
        if (n == 0) {
            return kInf;
        }
        return adaptedCost(adapt, static_cast<float>(n), m, c8);
    }

    float distAdaptedFull(size_t k, const std::pair<int, int>& t,
                          const std::pair<int, int>& s) const {
        float m[kMomCh][5];
        const size_t c8 = std::min(ch, kMomCh);
        momentsFull(k, t, s, m);
        const float n = static_cast<float>((2 * r + 1) * (2 * r + 1));
        return adaptedCost(adapt, n, m, c8);
    }

    // 匹配 t→s 的逐通道 gain/bias（无适配 = 恒等）；写 out[0..n]。
    void transform(const std::pair<int, int>& t, size_t k, const std::pair<int, int>& s,
                   std::pair<float, float>* out, size_t n) const {
        if (adapt.isNone()) {
            for (size_t c = 0; c < n; ++c) {
                out[c] = {1.0f, 0.0f};
            }
            return;
        }
        float m[kMomCh][5];
        const size_t c8 = std::min(ch, kMomCh);
        size_t nSamples = 0;
        if (full[k]) {
            momentsFull(k, t, s, m);
            nSamples = static_cast<size_t>((2 * r + 1) * (2 * r + 1));
        } else {
            nSamples = moments(t, s, m);
        }
        for (size_t c = 0; c < n; ++c) {
            if (nSamples == 0 || c >= c8) {
                out[c] = {1.0f, 0.0f};
            } else {
                float g, b;
                fitMoment(adapt, static_cast<float>(nSamples), m[c][0], m[c][1], m[c][2], m[c][3],
                          &g, &b);
                out[c] = {g, b};
            }
        }
    }

    // PatchMatch 单步：传播（本趟方向的左/上邻居匹配）+ 指数收缩随机搜索。
    std::pair<std::pair<int, int>, float> pmStep(const std::pair<int, int>& t, size_t k,
                                                 const std::pair<std::pair<int, int>, float>& cur,
                                                 const std::pair<int, int>* left,
                                                 const std::pair<int, int>* up, int step, Rng& rng,
                                                 int searchRadius) const {
        std::pair<int, int> best = cur.first;
        float bc = cur.second;
        auto tryCand = [&](const std::pair<int, int>& cand) {
            if (isValid(cand.first, cand.second) && cand != best) {
                const float d = dist(t, k, cand, bc);
                if (d < bc) {
                    best = cand;
                    bc = d;
                }
            }
        };
        if (left) {
            tryCand({left->first + step, left->second});
        }
        if (up) {
            tryCand({up->first, up->second + step});
        }
        int rad = searchRadius;
        while (rad >= 1) {
            tryCand({best.first + rng.range(-rad, rad), best.second + rng.range(-rad, rad)});
            rad /= 2;
        }
        return {best, bc};
    }

    // 一趟 PatchMatch：16 行带逐带规范序扫掠（带内反向趟反向行/列），带内传播不跨界。
    void patchmatch(std::vector<std::pair<int, int>>& nnf, std::vector<float>& cost, size_t pass,
                    uint64_t seed, int searchRadius) {
        constexpr size_t kBAND = 16;
        const size_t tw = tbox.width();
        if (tw == 0 || tbox.height() == 0 || nnf.size() < tbox.area() || cost.size() < tbox.area()) {
            return;
        }
        const bool reverse = pass % 2 == 1;
        const int step = reverse ? -1 : 1;
        auto bandSeed = [&](size_t bi) {
            return seed ^ (bi * 0x2545F4914F6CDD1Dull) ^ (static_cast<uint64_t>(pass) << 40);
        };
        std::vector<std::tuple<size_t, size_t, size_t>> bands;  // bi, 首行, 带内行数
        for (size_t y = tbox.y0; y < tbox.y1;) {
            const size_t bi = y / kBAND;
            const size_t end = std::min((bi + 1) * kBAND, tbox.y1);
            bands.emplace_back(bi, y, end - y);
            y = end;
        }
        for (const auto& [bi, ys, rows] : bands) {
            if (stop()) {
                return;
            }
            const size_t base = (ys - tbox.y0) * tw;
            Rng rng = Rng::seeded(bandSeed(bi));
            for (size_t ry = 0; ry < rows; ++ry) {
                if (stop()) {
                    return;
                }
                const size_t ly = reverse ? rows - 1 - ry : ry;
                const size_t y = ys + ly;
                for (size_t rx = 0; rx < tw; ++rx) {
                    const size_t lx = reverse ? tw - 1 - rx : rx;
                    const size_t li = ly * tw + lx;              // 带内索引
                    const size_t k = (y - tbox.y0) * tw + lx;    // tbox 全局索引
                    if (!targets[k]) {
                        continue;
                    }
                    std::pair<int, int> left{}, up{};
                    bool hasLeft = false, hasUp = false;
                    const long long nx = static_cast<long long>(lx) - step;
                    if (nx >= 0 && nx < static_cast<long long>(tw)) {
                        left = nnf[base + ly * tw + static_cast<size_t>(nx)];
                        hasLeft = true;
                    }
                    const long long nly = static_cast<long long>(ly) - step;
                    if (nly >= 0 && nly < static_cast<long long>(rows)) {
                        up = nnf[base + static_cast<size_t>(nly) * tw + lx];
                        hasUp = true;
                    }
                    const std::pair<int, int> t{static_cast<int>(tbox.x0 + lx), static_cast<int>(y)};
                    const auto [m, c] = pmStep(t, k, {nnf[base + li], cost[base + li]},
                                               hasLeft ? &left : nullptr, hasUp ? &up : nullptr,
                                               step, rng, searchRadius);
                    nnf[base + li] = m;
                    cost[base + li] = c;
                }
            }
        }
    }

    // 重算每个目标的匹配代价（孔像素在迭代间变化）；适配时同步 full 目标自矩。
    bool costs(const std::vector<std::pair<int, int>>& nnf, std::vector<float>& cost) {
        if (stop()) {
            return false;
        }
        const size_t ru = static_cast<size_t>(r);
        if (!adapt.isNone()) {
            const size_t c8 = std::min(ch, kMomCh);
            const size_t side = 2 * ru + 1;
            for (size_t ty = 0; ty < tbox.height(); ++ty) {
                if (stop()) {
                    return false;
                }
                for (size_t tx = 0; tx < tbox.width(); ++tx) {
                    const size_t k = ty * tbox.width() + tx;
                    if (full[k]) {
                        patchMoments(*img, w, ch, c8, side, tbox.x0 + tx - ru, tbox.y0 + ty - ru,
                                     &tmom[k * c8 * 2]);
                    }
                }
            }
        }
        for (size_t ty = 0; ty < tbox.height(); ++ty) {
            if (stop()) {
                return false;
            }
            const size_t y = tbox.y0 + ty;
            for (size_t tx = 0; tx < tbox.width(); ++tx) {
                const size_t k = ty * tbox.width() + tx;
                if (targets[k]) {
                    cost[k] = dist({static_cast<int>(tbox.x0 + tx), static_cast<int>(y)}, k,
                                   nnf[k], kInf);
                }
            }
        }
        return !stop();
    }

    // 重估孔像素：全部覆盖补丁匹配的相似度加权投票（拷贝带各自 gain/bias）。
    void vote(const std::vector<std::pair<int, int>>& nnf, const std::vector<float>& cost) {
        const size_t tbW = tbox.width();
        if (stop()) {
            return;
        }
        auto used = [&](size_t k) { return targets[k] && std::isfinite(cost[k]); };
        // σ² 取匹配代价 75 分位（Wexler 等的稳健尺度）。
        std::vector<float> cs;
        for (size_t k = 0; k < tbox.area(); ++k) {
            if (used(k)) {
                cs.push_back(cost[k]);
            }
        }
        float sigma2 = 1.0f;
        if (!cs.empty()) {
            const size_t k = std::min(cs.size() * 3 / 4, cs.size() - 1);
            std::nth_element(cs.begin(), cs.begin() + static_cast<long>(k), cs.end());
            sigma2 = std::max(cs[k], 1e-6f);
        }
        const bool adapted = !adapt.isNone();
        const size_t c8 = std::min(ch, kMomCh);
        std::vector<float> wt(tbox.area(), 0.0f);
        std::vector<ptrdiff_t> src(tbox.area(), 0);
        for (size_t ty = 0; ty < tbox.height(); ++ty) {
            for (size_t tx = 0; tx < tbox.width(); ++tx) {
                const size_t k = ty * tbW + tx;
                if (used(k)) {
                    wt[k] = std::max(std::exp(-cost[k] / (2.0f * sigma2)), 1e-8f);
                    src[k] = static_cast<ptrdiff_t>(nnf[k].second) * static_cast<ptrdiff_t>(w) +
                             nnf[k].first;
                }
            }
        }
        std::vector<std::pair<float, float>> gb;
        if (adapted) {
            gb.assign(tbox.area() * ch, {1.0f, 0.0f});
            for (size_t ty = 0; ty < tbox.height(); ++ty) {
                if (stop()) {
                    return;
                }
                const size_t y = tbox.y0 + ty;
                for (size_t tx = 0; tx < tbox.width(); ++tx) {
                    const size_t k = ty * tbW + tx;
                    if (used(k)) {
                        transform({static_cast<int>(tbox.x0 + tx), static_cast<int>(y)}, k, nnf[k],
                                  &gb[k * ch], ch);
                    }
                }
            }
        }
        // 投票写入孔包围盒域缓冲（每像素读旧像素）。
        const Bounds& hb = holeBox;
        std::vector<float> out(hb.area() * ch, 0.0f);
        std::vector<bool> got(hb.area(), false);
        for (size_t yy = 0; yy < hb.height(); ++yy) {
            if (stop()) {
                return;
            }
            const size_t y = hb.y0 + yy;
            for (size_t x = hb.x0; x < hb.x1; ++x) {
                if ((*px)[y * w + x] != Px::Hole) {
                    continue;
                }
                // 平面缓冲行主序（photocraft 的 orow 行切片 + 行内偏移的等价式）
                const size_t o = (yy * hb.width() + (x - hb.x0)) * ch;
                std::vector<float> acc(ch, 0.0f);
                float wsum = 0.0f;
                const size_t yLo = y > r ? y - r : 0;
                const size_t yHi = std::min(y + r, h - 1);
                for (size_t ty = yLo; ty <= yHi; ++ty) {
                    const ptrdiff_t dy = static_cast<ptrdiff_t>(ty) - static_cast<ptrdiff_t>(y);
                    const size_t krow = (ty - tbox.y0) * tbW;
                    const size_t xLo = x > r ? x - r : 0;
                    const size_t xHi = std::min(x + r, w - 1);
                    for (size_t tx = xLo; tx <= xHi; ++tx) {
                        const size_t k = krow + tx - tbox.x0;
                        const float wgt = wt[k];
                        if (wgt == 0.0f) {
                            continue;
                        }
                        // 匹配补丁落在该处的像素。
                        const size_t si = static_cast<size_t>(
                            src[k] - dy * static_cast<ptrdiff_t>(w) -
                            (static_cast<ptrdiff_t>(tx) - static_cast<ptrdiff_t>(x))) * ch;
                        for (size_t c = 0; c < ch; ++c) {
                            const float v = adapted ? gb[k * ch + c].first * (*img)[si + c] +
                                                          gb[k * ch + c].second
                                                    : (*img)[si + c];
                            acc[c] += v * wgt;
                        }
                        wsum += wgt;
                    }
                }
                if (wsum > 0.0f) {
                    for (size_t c = 0; c < ch; ++c) {
                        out[o + c] = acc[c] / wsum;
                    }
                    got[yy * hb.width() + (x - hb.x0)] = true;
                }
            }
        }
        if (stop()) {
            return;
        }
        for (size_t yy = 0; yy < hb.height(); ++yy) {
            for (size_t xx = 0; xx < hb.width(); ++xx) {
                if (got[yy * hb.width() + xx]) {
                    const size_t d = ((hb.y0 + yy) * w + hb.x0 + xx) * ch;
                    const size_t s = (yy * hb.width() + xx) * ch;
                    std::memcpy(&(*img)[d], &out[s], ch * sizeof(float));
                }
            }
        }
    }
};

// ---- 主流程 ----

CompleteResult completeCow(size_t w, size_t h, size_t ch, std::vector<float>&& imgIn,
                           const std::vector<Px>& px, const CompleteParams& p,
                           const CancelFn& cancel) {
    auto noSource = [&](std::vector<float>&& back) {
        return CompleteResult{CompleteStatus::NoSource, std::move(back)};
    };
    auto stopped = [] { return CompleteResult{CompleteStatus::Cancelled, {}}; };
    auto hb0 = holeBoundsOf(w, h, px);
    if (!hb0) {
        return CompleteResult{CompleteStatus::Ok, std::move(imgIn)};  // 无孔
    }
    if (ch == 0) {
        return CompleteResult{CompleteStatus::Ok, {}};
    }
    const int r = std::max(p.patchRadius, 1);
    const size_t psz = static_cast<size_t>(2 * r + 1);
    // 金字塔：收缩到孔约四补丁宽（更深一层在纹理已平均化的尺度上定结构，填出平面）。
    std::vector<Level> levels;
    levels.push_back(Level{w, h, std::move(imgIn), {}});
    levels.back().px = px;  // 层 0 共享调用方角色表（只读）
    std::vector<Bounds> boxes{*hb0};
    size_t ext = std::max(hb0->width(), hb0->height());
    std::vector<size_t> exts{ext};
    while (true) {
        if (cancelled(cancel)) {
            return stopped();
        }
        const Level& l = levels.back();
        if (ext <= 4 * psz || l.w / 2 < 3 * psz || l.h / 2 < 3 * psz) {
            break;
        }
        levels.push_back(downsample(l, ch));
        boxes.push_back(boxes.back().halved());
        ext = (ext + 1) / 2;
        exts.push_back(ext);
    }
    const size_t nlev = levels.size();
    std::optional<Coarser> prev;
    size_t totalPx = 0, donePx = 0;
    for (const auto& l : levels) {
        totalPx += l.w * l.h;
    }
    for (size_t li = nlev; li-- > 0;) {
        if (cancelled(cancel)) {
            return stopped();
        }
        Level lev = std::move(levels[li]);
        levels[li] = Level{};
        const Bounds& hb = boxes[li];
        const Bounds tb = hb.grown(static_cast<size_t>(r), lev.w, lev.h);
        const std::vector<bool> valid = validSources(lev.w, lev.h, r, lev.px);
        const Sources sources(valid, lev.w);
        // 最细层无源可拷：调用方回退（原像素奉还）。
        if (li == 0 && sources.len() == 0) {
            return noSource(std::move(lev.img));
        }
        const std::vector<bool> targets = targetsAround(lev.w, r, lev.px, hb, tb);
        if (cancelled(cancel)) {
            return stopped();
        }
        // 孔初始化：最粗层洋葱剥皮（无源可拷则膜填充），其余取粗层像素再投票精化。
        std::optional<Coarser> coarser = std::move(prev);
        prev.reset();
        if (coarser) {
            const size_t pw = coarser->w, ph = coarser->h;
            for (size_t y = hb.y0; y < hb.y1; ++y) {
                for (size_t x = hb.x0; x < hb.x1; ++x) {
                    if (lev.px[y * lev.w + x] == Px::Hole) {
                        const size_t sx = std::min(x / 2, pw - 1), sy = std::min(y / 2, ph - 1);
                        std::memcpy(&lev.img[(y * lev.w + x) * ch], &coarser->img[(sy * pw + sx) * ch],
                                    ch * sizeof(float));
                    }
                }
            }
        } else if (sources.len() > 0) {
            if (!onionPeelImpl(lev.w, lev.h, ch, r, lev.img, lev.px, hb,
                               sources.all(valid, lev.w), cancel)) {
                return stopped();
            }
        } else {
            std::vector<bool> unknown(lev.w * lev.h);
            for (size_t i = 0; i < lev.w * lev.h; ++i) {
                unknown[i] = lev.px[i] == Px::Hole || lev.px[i] == Px::Void;
            }
            const std::vector<float> m = poisson::membraneFill(lev.w, lev.h, ch, lev.img, unknown);
            for (size_t i = 0; i < lev.w * lev.h; ++i) {
                if (lev.px[i] == Px::Hole) {
                    std::memcpy(&lev.img[i * ch], &m[i * ch], ch * sizeof(float));
                }
            }
        }
        if (sources.len() == 0) {
            prev = Coarser{lev.w, lev.h, std::move(lev.img), {}, tb};
            continue;
        }
        const bool refineOnly = li + 1 < nlev && exts[li] > static_cast<size_t>(p.refineExtent);
        size_t nTargets = 0;
        for (bool t : targets) {
            nTargets += t ? 1u : 0u;
        }
        const bool keepSourceMoments =
            lev.w * lev.h <= kSourceMomentsMax &&
            sources.len() <= nTargets * (refineOnly ? 4u : 64u);
        Solver solver;
        solver.w = lev.w;
        solver.h = lev.h;
        solver.ch = ch;
        solver.r = r;
        solver.img = &lev.img;
        solver.px = &lev.px;
        solver.valid = valid;
        solver.targets = targets;
        solver.holeBox = hb;
        solver.tbox = tb;
        solver.adapt = p.adapt;
        solver.cancel = cancel;
        solver.prepare(keepSourceMoments);
        if (cancelled(cancel)) {
            return stopped();
        }
        // NNF 初始化：粗层匹配 ×2 位移上采样（落点有效时），否则随机源。
        Rng rng = Rng::seeded(p.seed ^ (static_cast<uint64_t>(li) << 20));
        const size_t tw = tb.width();
        std::vector<std::pair<int, int>> nnf(tb.area(), {0, 0});
        bool upsampled = false;
        for (size_t y = tb.y0; y < tb.y1; ++y) {
            if ((y % 32) == 0 && cancelled(cancel)) {
                return stopped();
            }
            for (size_t x = tb.x0; x < tb.x1; ++x) {
                const size_t k = (y - tb.y0) * tw + (x - tb.x0);
                if (!solver.targets[k]) {
                    continue;
                }
                std::optional<std::pair<int, int>> cand;
                if (coarser && !coarser->field.empty()) {
                    const size_t cx = std::min(x / 2, coarser->w - 1);
                    const size_t cy = std::min(y / 2, coarser->h - 1);
                    const auto [sx, sy] = coarser->fieldAt(cx, cy);
                    if (sx != kMinI32) {
                        // 同一位移，加倍。
                        const std::pair<int, int> c{
                            static_cast<int>(x) + 2 * (sx - static_cast<int>(cx)),
                            static_cast<int>(y) + 2 * (sy - static_cast<int>(cy))};
                        if (solver.isValid(c.first, c.second)) {
                            cand = c;
                        }
                    }
                }
                upsampled = upsampled || cand.has_value();
                if (cand) {
                    nnf[k] = *cand;
                } else {
                    nnf[k] = sources.nth(
                        static_cast<size_t>(rng.next() % sources.len()), valid, lev.w);
                }
            }
        }
        coarser.reset();  // 粗层已完成使命
        std::vector<float> cost(tb.area(), kInf);
        // 细层从粗层匹配（以其自身锐利像素投票）开始。
        if (upsampled) {
            if (!solver.costs(nnf, cost)) {
                return stopped();
            }
            solver.vote(nnf, cost);
        }
        const int em = li + 1 == nlev  ? p.emIters
                       : refineOnly    ? 1
                       : std::max(p.emIters >> (nlev - 1 - li), p.minEmIters);
        const int pmIters = refineOnly ? 1 : p.pmIters;
        const int fullSearch = static_cast<int>(std::max(lev.w, lev.h));
        for (int it = 0; it < em; ++it) {
            // 孔像素被重估：每次 PatchMatch 前刷新代价。
            if (!solver.costs(nnf, cost)) {
                return stopped();
            }
            const int radius = refineOnly            ? 2
                               : (li + 1 == nlev || it == 0) ? fullSearch
                                                             : std::min(4 * static_cast<int>(psz), fullSearch);
            for (int pass = 0; pass < pmIters; ++pass) {
                if (cancelled(cancel)) {
                    return stopped();
                }
                solver.patchmatch(nnf, cost, static_cast<size_t>(pass + it * pmIters),
                                  p.seed + static_cast<uint64_t>(li * 1000 + it), radius);
            }
            if (cancelled(cancel)) {
                return stopped();
            }
            solver.vote(nnf, cost);
            if (cancelled(cancel)) {
                return stopped();
            }
        }
        // 匹配场留给下一层（最后一层不用）。
        std::vector<std::pair<int, int>> field;
        if (li != 0) {
            field.resize(tb.area(), invalidField());
            for (size_t k = 0; k < tb.area(); ++k) {
                if (solver.targets[k]) {
                    field[k] = nnf[k];
                }
            }
        }
        donePx += lev.w * lev.h;
                prev = Coarser{lev.w, lev.h, std::move(lev.img), std::move(field), tb};
    }
    if (!prev) {
        return CompleteResult{CompleteStatus::Ok, {}};
    }
    return CompleteResult{CompleteStatus::Ok, std::move(prev->img)};
}

}  // namespace

// ---- 公共 API 包装 ----

namespace {

std::optional<Bounds> holeBBoxOf(size_t w, size_t h, const std::vector<bool>& hole) {
    std::optional<Bounds> b;
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            if (hole[y * w + x]) {
                b = b ? Bounds{std::min(b->x0, x), std::min(b->y0, y), std::max(b->x1, x + 1),
                               std::max(b->y1, y + 1)}
                      : Bounds{x, y, x + 1, y + 1};
            }
        }
    }
    return b;
}

}  // namespace

CompleteResult complete(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                        const std::vector<bool>& hole, const CompleteParams& p,
                        const CancelFn& cancel) {
    std::vector<Px> px(w * h);
    for (size_t i = 0; i < w * h; ++i) {
        px[i] = hole[i] ? Px::Hole : Px::Source;
    }
    return completeMasked(w, h, ch, img, px, p, cancel);
}

CompleteResult completeMasked(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                              const std::vector<Px>& px, const CompleteParams& p,
                              const CancelFn& cancel) {
    return completeCow(w, h, ch, std::vector<float>(img), px, p, cancel);
}

std::optional<std::pair<int, int>> bestOffset(size_t w, size_t h, size_t ch,
                                              const std::vector<float>& img,
                                              const std::vector<bool>& hole, size_t ring,
                                              int maxRadius) {
    const auto bboxOpt = holeBBoxOf(w, h, hole);
    if (!bboxOpt) {
        return std::nullopt;
    }
    const auto& bb = *bboxOpt;
    // 有效位移不会超出源图范围；钳制恶意入参防回绕/挂死（photocraft 同款护栏）。
    const int extent =
        static_cast<int>(std::min<size_t>(std::max(w, h), static_cast<size_t>(INT32_MAX) / 4));
    maxRadius = std::clamp(maxRadius, 0, extent);
    if (maxRadius == 0) {
        return std::nullopt;
    }
    const int ri = static_cast<int>(std::clamp(ring, size_t{1}, static_cast<size_t>(extent)));
    const Bounds hb{bb.x0, bb.y0, bb.x1, bb.y1};
    HoleIntegral hs(w, hole, hb);
    // 采样点：孔的环带（膨胀减孔），为速度子采样。
    std::vector<std::pair<int, int>> pts;
    const int x0 = static_cast<int>(bb.x0) - ri, y0 = static_cast<int>(bb.y0) - ri;
    const int x1 = static_cast<int>(bb.x1) + ri, y1 = static_cast<int>(bb.y1) + ri;
    for (int y = std::max(y0, 0); y < std::min(y1, static_cast<int>(h)); ++y) {
        for (int x = std::max(x0, 0); x < std::min(x1, static_cast<int>(w)); ++x) {
            if (hole[static_cast<size_t>(y) * w + static_cast<size_t>(x)]) {
                continue;
            }
            if (hs.count(x - ri, y - ri, x + ri + 1, y + ri + 1) > 0) {
                pts.emplace_back(x, y);
            }
        }
    }
    const size_t stride = std::max<size_t>(pts.size() / 2000, 1);
    std::vector<std::pair<int, int>> sampled;
    for (size_t i = 0; i < pts.size(); i += stride) {
        sampled.push_back(pts[i]);
    }
    pts.swap(sampled);
    const float n = static_cast<float>(std::max<size_t>(pts.size(), 1));
    auto eval = [&](int dx, int dy, float cutoff) -> std::optional<float> {
        const int hx0 = x0 + dx, hy0 = y0 + dy;
        const int hx1 = x1 + dx, hy1 = y1 + dy;
        const bool inside = hx0 >= 0 && hy0 >= 0 && hx1 <= static_cast<int>(w) &&
                            hy1 <= static_cast<int>(h);
        if ((dx == 0 && dy == 0) || !inside || hs.count(hx0, hy0, hx1, hy1) != 0) {
            return std::nullopt;
        }
        float e = 0.0f;
        const float penalty =
            1.0f + 1e-3f * (std::hypot(static_cast<float>(dx), static_cast<float>(dy)) /
                            static_cast<float>(maxRadius));
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto& [x, y] = pts[i];
            const size_t a = (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * ch;
            const size_t b =
                (static_cast<size_t>(y + dy) * w + static_cast<size_t>(x + dx)) * ch;
            for (size_t c = 0; c < ch; ++c) {
                const float d = img[a + c] - img[b + c];
                e += d * d;
            }
            // SSD 项非负：部分和是终分的下界；同式除法 + 惩罚保持浮点与严格平局序不变。
            if (i % 32 == 31 && std::isfinite(cutoff) && e / n * penalty >= cutoff) {
                return std::nullopt;
            }
        }
        return e / n * penalty;  // 平局微偏好更近源
    };
    // 搜索窗粗网格，再在胜者邻域全分辨率精化。
    const int step = std::max(maxRadius / 24, 1);
    std::optional<std::pair<int, int>> bestPos;
    float bestE = kInf;
    auto consider = [&](int dx, int dy) {
        const auto e = eval(dx, dy, bestPos ? bestE : kInf);
        if (e && *e < bestE) {
            bestPos = {dx, dy};
            bestE = *e;
        }
    };
    for (int dy = -maxRadius; dy <= maxRadius; dy += step) {
        for (int dx = -maxRadius; dx <= maxRadius; dx += step) {
            consider(dx, dy);
        }
    }
    if (step > 1 && bestPos) {
        const int bx = bestPos->first, by = bestPos->second;
        for (int dy = by - step; dy <= by + step; ++dy) {
            for (int dx = bx - step; dx <= bx + step; ++dx) {
                consider(dx, dy);
            }
        }
    }
    return bestPos;
}

namespace {

// 最小误差边界切割（Efros-Freeman quilting，动态规划）：沿带状域每步给出切缝的横向偏移；
// 切缝之前保留原像素。全域零代价时返回空（无可切）。
std::vector<size_t> minCut(size_t across, size_t along,
                           const std::function<float(size_t, size_t)>& err) {
    if (across == 0 || along == 0) {
        return {};
    }
    std::vector<float> acc(across * along);
    bool any = false;
    for (size_t v = 0; v < along; ++v) {
        for (size_t u = 0; u < across; ++u) {
            acc[v * across + u] = err(u, v);
            any = any || acc[v * across + u] != 0.0f;
        }
    }
    if (!any) {
        return {};
    }
    for (size_t v = 1; v < along; ++v) {
        for (size_t u = 0; u < across; ++u) {
            const size_t lo = u > 0 ? u - 1 : 0;
            const size_t hi = std::min(u + 1, across - 1);
            float m = kInf;
            for (size_t k = lo; k <= hi; ++k) {
                m = std::min(m, acc[(v - 1) * across + k]);
            }
            acc[v * across + u] += m;
        }
    }
    auto argmin = [&](size_t v, size_t lo, size_t hi) {
        size_t best = lo;
        for (size_t k = lo + 1; k <= hi; ++k) {
            if (acc[v * across + k] < acc[v * across + best]) {
                best = k;
            }
        }
        return best;
    };
    std::vector<size_t> cut(along, 0);
    size_t u = argmin(along - 1, 0, across - 1);
    for (size_t v = along; v-- > 0;) {
        if (v + 1 < along) {
            u = argmin(v, u > 0 ? u - 1 : 0, std::min(u + 1, across - 1));
        }
        cut[v] = u;
    }
    return cut;
}

}  // namespace

std::vector<float> synthesize(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                              const std::vector<bool>& hole, size_t block, uint64_t seed) {
    std::vector<float> out(img);
    const auto bboxOpt = holeBBoxOf(w, h, hole);
    if (!bboxOpt) {
        return out;
    }
    const auto& bb = *bboxOpt;
    const int b = static_cast<int>(std::max<size_t>(block, 4));
    const int ov = std::max(b / 3, 1);
    const std::vector<uint32_t> hs = integralOf(w, h, hole);
    // 候选块原点：整块（含重叠）在格内且不触孔。
    std::vector<std::pair<int, int>> cands;
    for (int y = ov; y < static_cast<int>(h) - b - ov; ++y) {
        for (int x = ov; x < static_cast<int>(w) - b - ov; ++x) {
            if (windowCount(hs, w, h, x - ov, y - ov, x + b + ov, y + b + ov) == 0) {
                cands.emplace_back(x, y);
            }
        }
    }
    if (cands.empty()) {
        return poisson::membraneFill(w, h, ch, img, hole);
    }
    std::vector<bool> filled(w * h);
    for (size_t i = 0; i < w * h; ++i) {
        filled[i] = !hole[i];
    }
    Rng rng = Rng::seeded(seed);
    for (int by = static_cast<int>(bb.y0); by < static_cast<int>(bb.y1); by += b) {
        for (int bx = static_cast<int>(bb.x0); bx < static_cast<int>(bb.x1); bx += b) {
            if (windowCount(hs, w, h, bx, by, bx + b, by + b) > 0) {
                // 取与已填像素最合的候选块（48 次随机抽选）。
                std::pair<int, int> best = cands[0];
                float bestE = kInf;
                for (int t = 0; t < 48; ++t) {
                    const auto& c = cands[static_cast<size_t>(rng.next() % cands.size())];
                    float e = 0.0f;
                    uint32_t n = 0;
                    for (int y = by - ov; y < by + b + ov; ++y) {
                        for (int x = bx - ov; x < bx + b + ov; ++x) {
                            if (x < 0 || y < 0 || x >= static_cast<int>(w) ||
                                y >= static_cast<int>(h) ||
                                !filled[static_cast<size_t>(y) * w + static_cast<size_t>(x)]) {
                                continue;
                            }
                            const size_t a =
                                (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * ch;
                            const size_t s = (static_cast<size_t>(y - by + c.second) * w +
                                              static_cast<size_t>(x - bx + c.first)) * ch;
                            for (size_t k = 0; k < ch; ++k) {
                                const float d = out[a + k] - img[s + k];
                                e += d * d;
                            }
                            n += 1;
                        }
                    }
                    const float err = n == 0 ? 0.0f : e / static_cast<float>(n);
                    if (err < bestE) {
                        best = c;
                        bestE = err;
                    }
                }
                const auto& c = best;
                const int x0 = std::max(bx - ov, 0), y0 = std::max(by - ov, 0);
                const int x1 = std::min(bx + b + ov, static_cast<int>(w));
                const int y1 = std::min(by + b + ov, static_cast<int>(h));
                auto src = [&](int x, int y) -> size_t {
                    return (static_cast<size_t>(y - by + c.second) * w +
                            static_cast<size_t>(x - bx + c.first)) * ch;
                };
                // 已有像素与块共存处的平方差（切割代价）。
                auto err = [&](int x, int y) -> float {
                    const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
                    if (!(hole[i] && filled[i])) {
                        return 0.0f;
                    }
                    const size_t a = i * ch, s = src(x, y);
                    float sum = 0.0f;
                    for (size_t q = 0; q < ch; ++q) {
                        const float d = out[a + q] - img[s + q];
                        sum += d * d;
                    }
                    return sum;
                };
                const auto left =
                    minCut(static_cast<size_t>(std::min(bx + ov, x1) - x0),
                           static_cast<size_t>(y1 - y0),
                           [&](size_t u, size_t v) { return err(x0 + static_cast<int>(u), y0 + static_cast<int>(v)); });
                const auto top =
                    minCut(static_cast<size_t>(std::min(by + ov, y1) - y0),
                           static_cast<size_t>(x1 - x0),
                           [&](size_t u, size_t v) { return err(x0 + static_cast<int>(v), y0 + static_cast<int>(u)); });
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        const size_t i = static_cast<size_t>(y) * w + static_cast<size_t>(x);
                        if (!hole[i]) {
                            continue;
                        }
                        const size_t u = static_cast<size_t>(x - x0), v = static_cast<size_t>(y - y0);
                        const bool keep =
                            filled[i] && ((v < left.size() && u < left[v]) || (u < top.size() && v < top[u]));
                        if (!keep) {
                            const size_t s = src(x, y);
                            std::memcpy(&out[i * ch], &img[s], ch * sizeof(float));
                        }
                        filled[i] = true;
                    }
                }
            }
        }
    }
    return out;
}

RectI samplingWindow(const RectI& hole, const RectI& canvas) {
    if (hole.empty() || canvas.empty()) {
        return {};
    }
    const double w = static_cast<double>(std::max(hole.width(), 50));
    const double h = static_cast<double>(std::max(hole.height(), 50));
    const long long side =
        static_cast<long long>(std::llround(4.0 * std::sqrt(w * h)));
    auto span = [&](long long c2, int extent, int lo, int hi) -> std::pair<int, int> {
        const long long slo = lo, shi = hi;
        const long long s = std::max(side, static_cast<long long>(extent));
        if (s >= shi - slo) {
            return {lo, hi};
        }
        const long long a = std::clamp((c2 - s) / 2, slo, shi - s);
        return {static_cast<int>(a), static_cast<int>(a + s)};
    };
    const auto sx = span(static_cast<long long>(hole.x0) + hole.x1, hole.width(), canvas.x0,
                         canvas.x1);
    const auto sy = span(static_cast<long long>(hole.y0) + hole.y1, hole.height(), canvas.y0,
                         canvas.y1);
    return RectI{sx.first, sy.first, sx.second, sy.second};
}

bool onionPeel(size_t w, size_t h, size_t ch, int r, std::vector<float>& img,
               const std::vector<Px>& px, const std::vector<std::pair<int, int>>& sources,
               const CancelFn& cancel) {
    const auto hb = holeBoundsOf(w, h, px);
    if (!hb) {
        return true;
    }
    return onionPeelImpl(w, h, ch, r, img, px, *hb, sources, cancel);
}

float leastSum(float v, float n, bool strict) {
    return leastSumImpl(v, n, strict);
}

}  // namespace inpaint
}  // namespace montage
