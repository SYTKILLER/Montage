// 泊松融合内核实现（M19a）。photocraft algo/poisson.rs 逐式翻译（MIT OR Apache-2.0），
// 常数与扫掠次序保持一致（FINE_ITERS=30/ω=1.7、粗层 2000 轮 ω=1.85、TOL=2e-5）。

#include "poisson_ops.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace montage {
namespace poisson {
namespace {

// 收敛阈值（单轮最大更新量）。
constexpr float kTol = 2e-5f;
// 粗层以上每层的 SOR 扫掠上限与松弛因子：96×72 圆盘解与全收敛 Gauss-Seidel 差 <1e-3。
constexpr size_t kFineIters = 30;
constexpr float kFineOmega = 1.7f;

void sor(size_t w, size_t h, const std::vector<bool>& unknown, std::vector<float>& v,
         size_t maxIter, float omega, const StopFn& stop) {
    // 预收集 unknown 像素及其邻居存在位。
    std::vector<std::pair<uint32_t, uint8_t>> cells;
    cells.reserve(w * h);
    for (size_t i = 0; i < w * h; ++i) {
        if (!unknown[i]) {
            continue;
        }
        const size_t x = i % w, y = i / w;
        const uint8_t m = static_cast<uint8_t>((x > 0 ? 1 : 0) | ((x + 1 < w) << 1) |
                                               ((y > 0) << 2) | ((y + 1 < h) << 3));
        cells.emplace_back(static_cast<uint32_t>(i), m);
    }
    for (size_t it = 0; it < maxIter; ++it) {
        if (stop && stop()) {
            return;
        }
        float maxD = 0.0f;
        for (const auto& cell : cells) {
            const size_t i = cell.first;
            const uint8_t m = cell.second;
            float s;
            float n;
            if (m == 0b1111) {
                s = v[i - 1] + v[i + 1] + v[i - w] + v[i + w];
                n = 4.0f;
            } else {
                s = 0.0f;
                n = 0.0f;
                if (m & 1) { s += v[i - 1]; n += 1.0f; }
                if (m & 2) { s += v[i + 1]; n += 1.0f; }
                if (m & 4) { s += v[i - w]; n += 1.0f; }
                if (m & 8) { s += v[i + w]; n += 1.0f; }
                if (n == 0.0f) {
                    continue;
                }
            }
            const float d = omega * (s / n - v[i]);
            v[i] += d;
            maxD = std::max(maxD, std::abs(d));
        }
        if (maxD < kTol) {
            break;
        }
    }
}

void solveRec(size_t w, size_t h, const std::vector<bool>& unknown, std::vector<float>& v,
              size_t depth, const StopFn& stop) {
    if (stop && stop()) {
        return;
    }
    size_t nUnknown = 0;
    for (bool u : unknown) {
        nUnknown += u ? 1u : 0u;
    }
    if (nUnknown == 0) {
        return;
    }
    std::vector<float> known;
    known.reserve(nUnknown);
    for (size_t i = 0; i < v.size(); ++i) {
        if (!unknown[i]) {
            known.push_back(v[i]);
        }
    }
    if (known.empty()) {
        return;  // 无可插值来源
    }
    if (w >= 8 && h >= 8 && nUnknown > 64 && depth < 16) {
        // 限制：粗像素任一子像素已知则已知（值 = 已知子像素均值）。
        const size_t cw = (w + 1) / 2, ch = (h + 1) / 2;
        std::vector<bool> cu(cw * ch, true);
        std::vector<float> cv(cw * ch, 0.0f);
        for (size_t cy = 0; cy < ch; ++cy) {
            for (size_t cx = 0; cx < cw; ++cx) {
                float s = 0.0f;
                uint32_t n = 0;
                for (auto [dx, dy] : {std::pair{0u, 0u}, {1u, 0u}, {0u, 1u}, {1u, 1u}}) {
                    const size_t x = cx * 2 + dx, y = cy * 2 + dy;
                    if (x < w && y < h && !unknown[y * w + x]) {
                        s += v[y * w + x];
                        n += 1;
                    }
                }
                if (n > 0) {
                    cu[cy * cw + cx] = false;
                    cv[cy * cw + cx] = s / static_cast<float>(n);
                }
            }
        }
        solveRec(cw, ch, cu, cv, depth + 1, stop);
        // 延拓：粗解双线性采样作为初值。
        for (size_t y = 0; y < h; ++y) {
            const float fy = std::clamp((static_cast<float>(y) + 0.5f) / 2.0f - 0.5f, 0.0f,
                                        static_cast<float>(ch - 1));
            const size_t y0 = static_cast<size_t>(fy);
            const size_t y1 = std::min(y0 + 1, ch - 1);
            const float ty = fy - static_cast<float>(y0);
            for (size_t x = 0; x < w; ++x) {
                if (!unknown[y * w + x]) {
                    continue;
                }
                const float fx = std::clamp((static_cast<float>(x) + 0.5f) / 2.0f - 0.5f, 0.0f,
                                            static_cast<float>(cw - 1));
                const size_t x0 = static_cast<size_t>(fx);
                const size_t x1 = std::min(x0 + 1, cw - 1);
                const float tx = fx - static_cast<float>(x0);
                const float a = cv[y0 * cw + x0] + (cv[y0 * cw + x1] - cv[y0 * cw + x0]) * tx;
                const float b = cv[y1 * cw + x0] + (cv[y1 * cw + x1] - cv[y1 * cw + x0]) * tx;
                v[y * w + x] = a + (b - a) * ty;
            }
        }
        sor(w, h, unknown, v, kFineIters, kFineOmega, stop);
    } else {
        float mean = 0.0f;
        for (float x : known) {
            mean += x;
        }
        mean /= static_cast<float>(known.size());
        for (size_t i = 0; i < v.size(); ++i) {
            if (unknown[i]) {
                v[i] = mean;
            }
        }
        sor(w, h, unknown, v, 2000, 1.85f, stop);
    }
}

StopFn nonNull(const StopFn& stop) {
    return stop ? stop : StopFn([] { return false; });
}

// 通道独立求解（w*h ≥ 64×64 时并行，结果与线程数无关）。
std::vector<float> perChannel(size_t w, size_t h, size_t ch,
                              const std::function<std::vector<float>(size_t)>& f) {
    std::vector<std::vector<float>> planes(ch);
    const bool par = w * h >= 64 * 64 && ch > 1;
    if (par) {
        std::vector<std::thread> threads;
        threads.reserve(ch);
        for (size_t c = 0; c < ch; ++c) {
            threads.emplace_back([&planes, &f, c] { planes[c] = f(c); });
        }
        for (auto& t : threads) {
            t.join();
        }
    } else {
        for (size_t c = 0; c < ch; ++c) {
            planes[c] = f(c);
        }
    }
    std::vector<float> out(w * h * ch, 0.0f);
    for (size_t c = 0; c < ch; ++c) {
        for (size_t i = 0; i < planes[c].size(); ++i) {
            out[i * ch + c] = planes[c][i];
        }
    }
    return out;
}

}  // namespace

void solveMembrane(size_t w, size_t h, const std::vector<bool>& unknown, std::vector<float>& v,
                   const StopFn& stop) {
    solveRec(w, h, unknown, v, 0, nonNull(stop));
}

void solveMembraneReference(size_t w, size_t h, const std::vector<bool>& unknown,
                            std::vector<float>& v, float tol, size_t maxIter) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (unknown[i]) {
            v[i] = 0.0f;
        }
    }
    for (size_t it = 0; it < maxIter; ++it) {
        float maxD = 0.0f;
        for (size_t y = 0; y < h; ++y) {
            for (size_t x = 0; x < w; ++x) {
                const size_t i = y * w + x;
                if (!unknown[i]) {
                    continue;
                }
                float s = 0.0f, n = 0.0f;
                const size_t xs[4] = {x - 1, x + 1, x, x};
                const size_t ys[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (xs[k] < w && ys[k] < h) {  // size_t 回绕 = 出界
                        s += v[ys[k] * w + xs[k]];
                        n += 1.0f;
                    }
                }
                if (n > 0.0f) {
                    const float nv = s / n;
                    maxD = std::max(maxD, std::abs(nv - v[i]));
                    v[i] = nv;
                }
            }
        }
        if (maxD < tol) {
            break;
        }
    }
}

std::vector<float> seamlessClone(size_t w, size_t h, size_t ch, const std::vector<float>& src,
                                 const std::vector<float>& dst, const std::vector<bool>& mask) {
    return perChannel(w, h, ch, [&](size_t c) {
        std::vector<float> hv(w * h);
        for (size_t i = 0; i < w * h; ++i) {
            hv[i] = mask[i] ? 0.0f : dst[i * ch + c] - src[i * ch + c];
        }
        solveMembrane(w, h, mask, hv);
        std::vector<float> out(w * h);
        for (size_t i = 0; i < w * h; ++i) {
            out[i] = mask[i] ? src[i * ch + c] + hv[i] : dst[i * ch + c];
        }
        return out;
    });
}

std::vector<float> membraneFill(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                                const std::vector<bool>& hole) {
    return perChannel(w, h, ch, [&](size_t c) {
        std::vector<float> v(w * h);
        for (size_t i = 0; i < w * h; ++i) {
            v[i] = hole[i] ? 0.0f : img[i * ch + c];
        }
        solveMembrane(w, h, hole, v);
        return v;
    });
}

std::vector<float> membraneFillWith(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                                    const std::vector<bool>& hole, const StopFn& stop) {
    const StopFn s = nonNull(stop);
    s();
    std::vector<float> out = perChannel(w, h, ch, [&](size_t c) {
        std::vector<float> v(w * h);
        for (size_t i = 0; i < w * h; ++i) {
            v[i] = hole[i] ? 0.0f : img[i * ch + c];
        }
        if (hole.size() == w * h && v.size() == w * h) {
            solveRec(w, h, hole, v, 0, s);
        }
        return v;
    });
    return s() ? std::vector<float>() : out;
}

}  // namespace poisson
}  // namespace montage
