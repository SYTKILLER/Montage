// P5③：Auto 采样区（photocraft content_aware.rs auto_sampling_with +
// segment/mod.rs grid_cut_impl + selection/distance.rs edt 的 C++ 翻译）。
// 语义逐条对拍：降采样格特征、环带、确定性 k-means 起点（ring[j·n/kc]）、
// τ = P90(spread)·1.6 max 0.03、HARD_FG = 孔及 dist≤1.5 邻域、8 连通 NB4 对折、
// 边权 = weight(d²)/len、fixed 邻接折入终端容量、源不足回退全可用。
#include "engine/auto_sample.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/maxflow.h"

namespace montage {
namespace autosample {

namespace {

constexpr uint8_t kFree = 0;
constexpr uint8_t kHardFg = 1;
constexpr uint8_t kHardBg = 2;
constexpr size_t kAutoSide = 384;  // 最大工作边（更大的窗按比例降采样）

// 8 连通半邻域（photocraft NB：(1,0,1)(0,1,1)(1,1,√2)(-1,1,√2)——往返各扫一遍即全 8 邻）
struct Nb {
    int dx;
    int dy;
    float len;
};
constexpr Nb kNb[4] = {{1, 0, 1.0f}, {0, 1, 1.0f}, {1, 1, 1.41421356f}, {-1, 1, 1.41421356f}};

float colorD2(const float* a, const float* b, int ch) {
    float s = 0.0f;
    for (int c = 0; c < ch; ++c) {
        const float d = a[c] - b[c];
        s += d * d;
    }
    return s;
}

// 半径 r 的 box mean（边缘 clamp 窗口；积分图）。
std::vector<float> boxMean(int w, int h, const std::vector<float>& v, int r) {
    const int w1 = w + 1;
    std::vector<double> s(static_cast<size_t>(w1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y) {
        double row = 0.0;
        for (int x = 0; x < w; ++x) {
            row += v[static_cast<size_t>(y) * w + x];
            s[static_cast<size_t>(y + 1) * w1 + x + 1] =
                s[static_cast<size_t>(y) * w1 + x + 1] + row;
        }
    }
    std::vector<float> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        const int y0 = std::max(0, y - r);
        const int y1 = std::min(h, y + r + 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - r);
            const int x1 = std::min(w, x + r + 1);
            const double sum = s[static_cast<size_t>(y1) * w1 + x1] +
                               s[static_cast<size_t>(y0) * w1 + x0] -
                               s[static_cast<size_t>(y0) * w1 + x1] -
                               s[static_cast<size_t>(y1) * w1 + x0];
            out[static_cast<size_t>(y) * w + x] =
                static_cast<float>(sum / static_cast<double>((x1 - x0) * (y1 - y0)));
        }
    }
    return out;
}

// 二值欧氏距离变换（Felzenszwalb–Huttenlocher：列最近种子 + 行抛物线下包络；
// 无种子 = 1e10）。auto_sampling 工作格 ≤384²，标量双扫即可。
std::vector<float> edt(const std::vector<bool>& seed, int w, int h) {
    const float kEmpty = 1e10f;
    const size_t n = static_cast<size_t>(w) * h;
    bool any = false;
    for (bool b : seed) {
        if (b) {
            any = true;
            break;
        }
    }
    if (!any) {
        return std::vector<float>(n, kEmpty);
    }
    // 列向最近种子（线性距离；无种子 = kEmpty）→ 行向抛物线包络（平方域）
    std::vector<float> lin(n, kEmpty);
    for (int x = 0; x < w; ++x) {
        float best = kEmpty;
        for (int y = 0; y < h; ++y) {
            if (seed[static_cast<size_t>(y) * w + x]) {
                best = 0.0f;
            }
            if (best < kEmpty) {
                lin[static_cast<size_t>(y) * w + x] = best;
                best += 1.0f;
            }
        }
        best = kEmpty;
        for (int y = h - 1; y >= 0; --y) {
            if (seed[static_cast<size_t>(y) * w + x]) {
                best = 0.0f;
            }
            if (best < lin[static_cast<size_t>(y) * w + x]) {
                lin[static_cast<size_t>(y) * w + x] = best;
            }
            if (best < kEmpty) {
                best += 1.0f;
            }
        }
    }
    std::vector<float> d2(n);
    for (size_t i = 0; i < n; ++i) {
        d2[i] = lin[i] >= kEmpty ? kEmpty : lin[i] * lin[i];
    }
    // 行向 1D 平方距离下包络
    std::vector<float> out(n);
    std::vector<double> f(static_cast<size_t>(w));
    std::vector<double> z(static_cast<size_t>(w) + 1);
    std::vector<int> v(static_cast<size_t>(w));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            f[x] = d2[static_cast<size_t>(y) * w + x];
        }
        int k = 0;
        v[0] = 0;
        z[0] = -1e18;
        z[1] = 1e18;
        for (int q = 1; q < w; ++q) {
            double s = 0.0;
            while (true) {
                s = ((f[q] + static_cast<double>(q) * q) -
                     (f[v[k]] + static_cast<double>(v[k]) * v[k])) /
                    (2.0 * q - 2.0 * v[k]);
                if (s <= z[k]) {
                    --k;
                } else {
                    break;
                }
            }
            ++k;
            v[k] = q;
            z[k] = s;
            z[k + 1] = 1e18;
        }
        k = 0;
        for (int x = 0; x < w; ++x) {
            while (z[k + 1] < x) {
                ++k;
            }
            const double d = static_cast<double>(x) - v[k];
            out[static_cast<size_t>(y) * w + x] =
                static_cast<float>(std::sqrt(d * d + f[v[k]]));
        }
    }
    return out;
}

std::vector<bool> gridCutImpl(const float* img, int w, int h, int ch,
                              const std::vector<float>& costFg,
                              const std::vector<float>& costBg,
                              const std::vector<uint8_t>& fixed,
                              const std::function<float(float)>& weight) {
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<bool> out(n, false);
    // 自由像素编号（fixed/fixed 链不进图）
    std::vector<uint32_t> idx(n, MaxflowGraph::kNone);
    uint32_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (fixed[i] == kFree) {
            idx[i] = count++;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        out[i] = fixed[i] == kHardFg;
    }
    if (count == 0) {
        return out;
    }
    MaxflowGraph g(count, count);
    for (size_t i = 0; i < n; ++i) {
        if (idx[i] != MaxflowGraph::kNone) {
            g.addTweights(idx[i], costBg[i], costFg[i]);
        }
    }
    const auto addFixedLink = [&](size_t node, uint8_t fx, float wt) {
        if (fx == kHardFg) {
            g.addTweights(node, wt, 0.0f);  // 标背景会切断该链
        } else {
            g.addTweights(node, 0.0f, wt);
        }
    };
    // 稠密栅格遍历（photocraft 同序；半邻域 NB4 前向扫 = 全 8 邻入图一次）
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const float* a = img + i * ch;
            for (const Nb& nb : kNb) {
                const int nx = x + nb.dx;
                const int ny = y + nb.dy;
                if (nx < 0 || nx >= w || ny >= h) {
                    continue;
                }
                const size_t j = static_cast<size_t>(ny) * w + nx;
                const float wt = weight(colorD2(a, img + j * ch, ch)) / nb.len;
                const bool fi = idx[i] != MaxflowGraph::kNone;
                const bool fj = idx[j] != MaxflowGraph::kNone;
                if (fi && fj) {
                    g.addEdge(idx[i], idx[j], wt, wt);
                } else if (fi) {
                    addFixedLink(idx[i], fixed[j], wt);
                } else if (fj) {
                    addFixedLink(idx[j], fixed[i], wt);
                }
            }
        }
    }
    g.maxflow();
    for (size_t i = 0; i < n; ++i) {
        if (idx[i] != MaxflowGraph::kNone) {
            out[i] = g.inSource(idx[i]);
        }
    }
    return out;
}

}  // namespace

std::vector<bool> gridCut(const float* img, int w, int h, int ch,
                          const std::vector<float>& costFg, const std::vector<float>& costBg,
                          const std::vector<uint8_t>& fixed,
                          const std::function<float(float)>& weight) {
    return gridCutImpl(img, w, h, ch, costFg, costBg, fixed, weight);
}

std::vector<bool> autoSampling(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                               const std::vector<bool>& hole) {
    const size_t n = w * h;
    const auto everything = [&]() {
        std::vector<bool> all(n, true);
        for (size_t i = 0; i < n; ++i) {
            all[i] = !hole[i];
        }
        return all;
    };
    if (w == 0 || h == 0 || ch == 0 || img.size() < n * ch || hole.size() < n) {
        return {};
    }
    bool anyHole = false;
    for (bool b : hole) {
        if (b) {
            anyHole = true;
            break;
        }
    }
    if (!anyHole) {
        return everything();
    }
    // 降采样格（k×k 块合并；工作边 ≤ 384）
    const int k = static_cast<int>(std::max<size_t>(1, (std::max(w, h) + kAutoSide - 1) / kAutoSide));
    const int sw = static_cast<int>((w + k - 1) / k);
    const int sh = static_cast<int>((h + k - 1) / k);
    const int nc = static_cast<int>(std::min<size_t>(ch, 3));
    const int nf = nc + 1;
    const size_t sn = static_cast<size_t>(sw) * sh;
    std::vector<double> sums(sn * (nc + 2), 0.0);
    for (int y = 0; y < static_cast<int>(h); ++y) {
        const int sy = y / k;
        for (int x = 0; x < static_cast<int>(w); ++x) {
            const size_t p = static_cast<size_t>(y) * w + x;
            double* cell = &sums[(static_cast<size_t>(sy) * sw + x / k) * (nc + 2)];
            if (hole[p]) {
                cell[nc + 1] = 1.0;
            }
            for (int c = 0; c < nc; ++c) {
                cell[c] += img[p * ch + c];
            }
            cell[nc] += 1.0;
        }
    }
    std::vector<std::vector<float>> planes(nc);
    for (int c = 0; c < nc; ++c) {
        planes[c].resize(sn);
        for (size_t i = 0; i < sn; ++i) {
            const double* cell = &sums[i * (nc + 2)];
            planes[c][i] = static_cast<float>(cell[c] / std::max(1.0, cell[nc]));
        }
    }
    std::vector<bool> shole(sn);
    for (size_t i = 0; i < sn; ++i) {
        shole[i] = sums[i * (nc + 2) + nc + 1] > 0.0;
    }
    // 特征：局部均值（各色面）+ 局部对比（亮度面 7×7 标准差 ×2）
    std::vector<std::vector<float>> means(nc);
    for (int c = 0; c < nc; ++c) {
        means[c] = boxMean(sw, sh, planes[c], 3);
    }
    std::vector<float> luma(sn);
    for (size_t i = 0; i < sn; ++i) {
        float s = 0.0f;
        for (int c = 0; c < nc; ++c) {
            s += planes[c][i];
        }
        luma[i] = s / static_cast<float>(nc);
    }
    std::vector<float> luma2(sn);
    for (size_t i = 0; i < sn; ++i) {
        luma2[i] = luma[i] * luma[i];
    }
    const std::vector<float> lm = boxMean(sw, sh, luma, 3);
    const std::vector<float> lm2 = boxMean(sw, sh, luma2, 3);
    std::vector<float> contrast(sn);
    for (size_t i = 0; i < sn; ++i) {
        contrast[i] = std::sqrt(std::max(0.0f, lm2[i] - lm[i] * lm[i]));
    }
    const auto feat = [&](size_t i, std::vector<float>& f) {
        for (int c = 0; c < nc; ++c) {
            f[c] = means[c][i];
        }
        f[nc] = 2.0f * contrast[i];
    };
    // 环带：孔外、距孔 ≤ 6/k+1
    const std::vector<float> dist = edt(shole, sw, sh);
    const float ringW = 6.0f / static_cast<float>(k) + 1.0f;
    std::vector<size_t> ring;
    for (size_t i = 0; i < sn; ++i) {
        if (!shole[i] && dist[i] <= ringW) {
            ring.push_back(i);
        }
    }
    if (ring.empty()) {
        return everything();
    }
    // 确定性 k-means（起点 = ring 等分采样；8 轮）
    const int kc = static_cast<int>(std::min<size_t>(6, ring.size()));
    std::vector<float> f(nf);
    std::vector<std::vector<float>> centres(kc);
    for (int j = 0; j < kc; ++j) {
        feat(ring[static_cast<size_t>(j) * ring.size() / static_cast<size_t>(kc)], f);
        centres[j] = f;
    }
    const auto d2f = [&](const std::vector<float>& a, const std::vector<float>& b) {
        float s = 0.0f;
        for (int c = 0; c < nf; ++c) {
            const float d = a[c] - b[c];
            s += d * d;
        }
        return s;
    };
    for (int it = 0; it < 8; ++it) {
        std::vector<std::vector<float>> acc(kc, std::vector<float>(nf, 0.0f));
        std::vector<int> cnt(kc, 0);
        for (size_t i : ring) {
            feat(i, f);
            int best = 0;
            float bd = 1e30f;
            for (int j = 0; j < kc; ++j) {
                const float d = d2f(f, centres[j]);
                if (d < bd) {
                    bd = d;
                    best = j;
                }
            }
            for (int c = 0; c < nf; ++c) {
                acc[best][c] += f[c];
            }
            cnt[best]++;
        }
        for (int j = 0; j < kc; ++j) {
            if (cnt[j] > 0) {
                for (int c = 0; c < nf; ++c) {
                    centres[j][c] = acc[j][c] / cnt[j];
                }
            }
        }
    }
    const auto nearest = [&](size_t i, std::vector<float>& ff) {
        feat(i, ff);
        float bd = 1e30f;
        for (int j = 0; j < kc; ++j) {
            bd = std::min(bd, d2f(ff, centres[j]));
        }
        return std::sqrt(bd);
    };
    // τ = 环带离簇距 P90 × 1.6（下限 0.03）
    std::vector<float> spread(ring.size());
    for (size_t r = 0; r < ring.size(); ++r) {
        spread[r] = nearest(ring[r], f);
    }
    const size_t q = std::min(spread.size() - 1, spread.size() * 9 / 10);
    std::nth_element(spread.begin(), spread.begin() + static_cast<long>(q), spread.end());
    const float tau = std::max(spread[q] * 1.6f, 0.03f);
    // 数据项 + 硬约束（孔与近孔 = HARD_FG）
    std::vector<float> costFg(sn, 0.0f);
    std::vector<float> costBg(sn, 0.0f);
    std::vector<uint8_t> fixed(sn, kFree);
    for (size_t i = 0; i < sn; ++i) {
        if (shole[i] || dist[i] <= 1.5f) {
            fixed[i] = kHardFg;
            continue;
        }
        const float d = nearest(i, f) / tau;
        costFg[i] = d * d;
        costBg[i] = 1.0f;
    }
    // 对比敏感平滑项（photocraft 拟合 PS Auto 覆盖：8·e^(−d²/(2·0.05²)) + 0.05）
    const auto weight = [](float d2v) {
        return 8.0f * std::exp(-d2v / (2.0f * 0.05f * 0.05f)) + 0.05f;
    };
    // 图割输入：RGB = 均值面（缺面用最后一面补）
    std::vector<float> rgb(sn * 3, 0.0f);
    for (size_t i = 0; i < sn; ++i) {
        for (int c = 0; c < 3; ++c) {
            rgb[i * 3 + c] = means[std::min(c, nc - 1)][i];
        }
    }
    const std::vector<bool> area = gridCut(rgb.data(), sw, sh, 3, costFg, costBg, fixed, weight);
    // 上采样（k 块）+ 孔恒不可采样
    std::vector<bool> out(n, false);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const size_t p = y * w + x;
            out[p] = !hole[p] && area[(y / k) * sw + x / k];
        }
    }
    // 源不足（< 2·max(孔,64)）回退全可用
    size_t nHole = 0;
    for (bool b : hole) {
        nHole += b ? 1u : 0u;
    }
    size_t nOut = 0;
    for (bool b : out) {
        nOut += b ? 1u : 0u;
    }
    if (nOut < 2 * std::max<size_t>(nHole, 64)) {
        return everything();
    }
    return out;
}

}  // namespace autosample
}  // namespace montage
