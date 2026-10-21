// 内容感知比例实现（M19c）。photocraft algo/seam.rs 逐式翻译（MIT OR Apache-2.0）。
// 缝搜索平局序 = 正上 → 左上 → 右上（严格小于才换）；缝尾取最左。与朴素参照实现
// 位级一致（宿主 400 例对拍 + 上游 pinned fnv 输出哈希锁定）。

#include "seam_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace seam {
namespace {

constexpr float kProtectEnergy = 1.0e4f;
// 大图收缩代理：缝搜索高度上限（24MP 工作尺寸下 DP 工作量约省 4 倍，输出像素不量化）。
constexpr size_t kSeamSearchMaxHeight = 512;
constexpr size_t kProxyShrinkMinHeight = 2048;
constexpr size_t kProxyShrinkMinSeams = 64;

// cw 宽、行距 stride 的图在 (x,y) 的能量（全图与逐缝增量更新共用，保证位级一致）。
inline float energyAt(const std::vector<float>& img, size_t stride, size_t cw, size_t h, size_t ch,
                      size_t x, size_t y) {
    const size_t xl = x > 0 ? x - 1 : 0, xr = std::min(x + 1, cw - 1);
    const size_t yu = y > 0 ? y - 1 : 0, yd = std::min(y + 1, h - 1);
    auto at = [&](size_t xx, size_t yy, size_t c) { return img[(yy * stride + xx) * ch + c]; };
    float s = 0.0f;
    for (size_t c = 0; c < ch; ++c) {
        const float v = at(x, y, c);
        s += std::abs(at(xr, y, c) - v) + std::abs(v - at(xl, y, c));
        s += std::abs(at(x, yd, c) - v) + std::abs(v - at(x, yu, c));
    }
    return s * 0.5f;
}

// 缝搜索的 DP 草稿缓冲（跨缝复用）。平局序：正上(1) → 左上(0) → 右上(2)。
struct Dp {
    std::vector<float> cost;
    std::vector<float> next;
    std::vector<uint8_t> from;  // 每像素：0=左上 1=正上 2=右上

    std::vector<size_t> seamOf(size_t stride, size_t cw, size_t h, const std::vector<float>& e) {
        cost.assign(e.begin(), e.begin() + static_cast<long>(cw));
        next.resize(cw, 0.0f);
        from.resize(cw * h, 0);
        for (size_t y = 1; y < h; ++y) {
            const float* ev = &e[y * stride];
            uint8_t* fr = &from[y * cw];
            if (cw == 1) {
                next[0] = cost[0] + ev[0];
                fr[0] = 1;
            } else {
                if (cost[1] < cost[0]) {
                    next[0] = cost[1] + ev[0];
                    fr[0] = 2;
                } else {
                    next[0] = cost[0] + ev[0];
                    fr[0] = 1;
                }
                for (size_t x = 1; x + 1 < cw; ++x) {
                    float b;
                    uint8_t d;
                    if (cost[x - 1] < cost[x]) {
                        b = cost[x - 1];
                        d = 0;
                    } else {
                        b = cost[x];
                        d = 1;
                    }
                    if (cost[x + 1] < b) {
                        b = cost[x + 1];
                        d = 2;
                    }
                    next[x] = b + ev[x];
                    fr[x] = d;
                }
                if (cost[cw - 2] < cost[cw - 1]) {
                    next[cw - 1] = cost[cw - 2] + ev[cw - 1];
                    fr[cw - 1] = 0;
                } else {
                    next[cw - 1] = cost[cw - 1] + ev[cw - 1];
                    fr[cw - 1] = 1;
                }
            }
            cost.swap(next);
        }
        size_t x = 0;
        for (size_t i = 1; i < cw; ++i) {
            if (cost[i] < cost[x]) {
                x = i;  // 平局取最左（首个最小）
            }
        }
        std::vector<size_t> out(h, 0);
        for (size_t y = h; y-- > 0;) {
            out[y] = x;
            if (y > 0) {
                const uint8_t f = from[y * cw + x];
                x = f == 0 ? (x > 0 ? x - 1 : 0) : f == 2 ? std::min(x + 1, cw - 1) : x;
            }
        }
        return out;
    }
};

// 逐缝移除的就地雕刻器：能量图只重算缝邻域。
struct Carver {
    size_t stride = 0;
    size_t cw = 0;
    size_t h = 0;
    size_t ch = 0;
    std::vector<float> img;
    std::vector<float> prot;  // 空 = 无保护
    bool hasProt = false;
    std::vector<float> e;  // 能量 + 保护（缝搜索所见）
    std::vector<size_t> idx;  // 每像素原始列号（track 时）
    bool track = false;
    Dp dp;

    Carver(size_t w, size_t hh, size_t channels, std::vector<float>&& image,
           const std::vector<float>* protect, bool tracking)
        : stride(w), cw(w), h(hh), ch(channels), img(std::move(image)), track(tracking) {
        e = energyOf(w, h, ch, img);
        if (protect != nullptr) {
            hasProt = true;
            prot = *protect;
            for (size_t i = 0; i < e.size(); ++i) {
                e[i] += std::clamp(prot[i], 0.0f, 1.0f) * kProtectEnergy;
            }
        }
        if (track) {
            idx.resize(w * h);
            for (size_t y = 0; y < h; ++y) {
                for (size_t x = 0; x < w; ++x) {
                    idx[y * w + x] = x;
                }
            }
        }
    }

    // 移除最低能量缝；返回每行被移除的列（track 时为原始列）。
    std::vector<size_t> removeSeam() {
        const std::vector<size_t> seam = dp.seamOf(stride, cw, h, e);
        std::vector<size_t> removed(seam.size());
        for (size_t y = 0; y < seam.size(); ++y) {
            removed[y] = track ? idx[y * stride + seam[y]] : seam[y];
        }
        shiftRows(img, stride * ch, seam, ch);
        shiftRows(e, stride, seam, 1);
        if (hasProt) {
            shiftRows(prot, stride, seam, 1);
        }
        if (track) {
            shiftRows(idx, stride, seam, 1);
        }
        cw -= 1;
        updateEnergy(seam);
        return removed;
    }

    // 重算邻居变化的像素能量：行 y 从缝在 y-1..=y+1 的最左 −1 到最右。
    void updateEnergy(const std::vector<size_t>& seam) {
        if (cw == 0) {
            return;
        }
        for (size_t y = 0; y < h; ++y) {
            const size_t lo0 = y > 0 ? y - 1 : 0;
            const size_t hi0 = std::min(y + 2, h);
            size_t lo = SIZE_MAX, hi = 0;
            for (size_t i = lo0; i < hi0; ++i) {
                lo = std::min(lo, seam[i]);
                hi = std::max(hi, seam[i]);
            }
            const size_t x0 = lo > 0 ? lo - 1 : 0;
            const size_t x1 = std::min(hi, cw - 1);
            for (size_t x = x0; x <= x1; ++x) {
                float v = energyAt(img, stride, cw, h, ch, x, y);
                if (hasProt) {
                    v += std::clamp(prot[y * stride + x], 0.0f, 1.0f) * kProtectEnergy;
                }
                e[y * stride + x] = v;
            }
        }
    }

    // 雕刻后的像素，cw×h×ch 行紧排。
    std::vector<float> intoPixels() {
        const size_t row = stride * ch, used = cw * ch;
        if (row == used) {
            return std::move(img);
        }
        std::vector<float> out;
        out.reserve(cw * h * ch);
        for (size_t y = 0; y < h; ++y) {
            out.insert(out.end(), img.begin() + static_cast<long>(y * row),
                       img.begin() + static_cast<long>(y * row + used));
        }
        return out;
    }

private:
    // 每行移除 seam[y]（n 值/像素），其余左移。
    template <typename T>
    void shiftRows(std::vector<T>& buf, size_t rowLen, const std::vector<size_t>& seam,
                   size_t n) {
        for (size_t y = 0; y < h; ++y) {
            const size_t x = seam[y];
            T* row = &buf[y * rowLen];
            std::memmove(row + x * n, row + (x + 1) * n, (cw - x - 1) * n * sizeof(T));
        }
    }
};

std::vector<float> transpose(size_t w, size_t h, size_t ch, const std::vector<float>& img) {
    std::vector<float> out(img.size(), 0.0f);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const size_t s = (y * w + x) * ch, d = (x * h + y) * ch;
            std::memcpy(&out[d], &img[s], ch * sizeof(float));
        }
    }
    return out;
}

// 大图收缩代理：行均值降采样上找缝，路径应用到原像素（输出全分辨率）。
std::vector<float> carveWidthProxyHeight(size_t w, size_t h, size_t ch,
                                         const std::vector<float>& img,
                                         const std::vector<float>* protect, size_t newW,
                                         const CancelFn& cancel) {
    const size_t step = (h + kSeamSearchMaxHeight - 1) / kSeamSearchMaxHeight;
    const size_t proxyH = (h + step - 1) / step;
    const size_t rowLen = w * ch;
    std::vector<float> proxy(proxyH * rowLen, 0.0f);
    for (size_t py = 0; py < proxyH; ++py) {
        const size_t y0 = py * step;
        const size_t y1 = std::min(y0 + step, h);
        const float divisor = static_cast<float>(y1 - y0);
        float* outRow = &proxy[py * rowLen];
        for (size_t y = y0; y < y1; ++y) {
            const float* inRow = &img[y * rowLen];
            for (size_t i = 0; i < rowLen; ++i) {
                outRow[i] += inRow[i] / divisor;
            }
        }
    }
    std::vector<float> proxyProtect;
    bool hasProt = protect != nullptr;
    if (hasProt) {
        proxyProtect.assign(proxyH * w, 0.0f);
        for (size_t py = 0; py < proxyH; ++py) {
            const size_t y0 = py * step;
            const size_t y1 = std::min(y0 + step, h);
            const float divisor = static_cast<float>(y1 - y0);
            float* outRow = &proxyProtect[py * w];
            for (size_t y = y0; y < y1; ++y) {
                const float* inRow = &(*protect)[y * w];
                for (size_t x = 0; x < w; ++x) {
                    outRow[x] += inRow[x] / divisor;
                }
            }
        }
    }
    const size_t seamCount = w - newW;
    Carver carver(w, proxyH, ch, std::move(proxy), hasProt ? &proxyProtect : nullptr, true);
    std::vector<std::vector<size_t>> removed;
    removed.reserve(seamCount);
    while (carver.cw > newW) {
        if (cancel && cancel()) {
            return {};
        }
        removed.push_back(carver.removeSeam());
    }
    // 每代理行的被移除列（升序）
    std::vector<std::vector<size_t>> removedByRow(proxyH);
    for (size_t py = 0; py < proxyH; ++py) {
        std::vector<size_t> columns(seamCount);
        for (size_t i = 0; i < seamCount; ++i) {
            columns[i] = removed[i][py];
        }
        std::sort(columns.begin(), columns.end());
        removedByRow[py] = std::move(columns);
    }
    std::vector<float> out;
    out.reserve(newW * h * ch);
    std::vector<size_t> columns(seamCount, 0);
    for (size_t y = 0; y < h; ++y) {
        if (cancel && cancel()) {
            return {};
        }
        const size_t proxyY = std::min(y / step, proxyH - 1);
        const size_t nextProxyY = std::min(proxyY + 1, proxyH - 1);
        const float blend = static_cast<float>(y % step) / static_cast<float>(step);
        const std::vector<size_t>& lower = removedByRow[proxyY];
        const std::vector<size_t>& upper = removedByRow[nextProxyY];
        size_t minX = 0;
        for (size_t i = 0; i < seamCount; ++i) {
            const float interpolated =
                static_cast<float>(lower[i]) +
                (static_cast<float>(upper[i]) - static_cast<float>(lower[i])) * blend;
            const size_t maxX = w - (seamCount - i);
            size_t x = static_cast<size_t>(std::llround(interpolated));
            x = std::min(std::max(x, minX), maxX);
            columns[i] = x;
            minX = x + 1;
        }
        size_t next = 0;
        const float* row = &img[y * rowLen];
        for (size_t x = 0; x < w; ++x) {
            if (next < seamCount && columns[next] == x) {
                next += 1;
            } else {
                out.insert(out.end(), row + x * ch, row + (x + 1) * ch);
            }
        }
    }
    return out;
}

}  // namespace

std::vector<float> energyOf(size_t w, size_t h, size_t ch, const std::vector<float>& img) {
    std::vector<float> e(w * h, 0.0f);
    if (w == 0 || h == 0) {
        return e;
    }
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            e[y * w + x] = energyAt(img, w, w, h, ch, x, y);
        }
    }
    return e;
}

std::vector<size_t> findSeam(size_t w, size_t h, const std::vector<float>& energy) {
    if (w == 0 || h == 0 || energy.size() < w * h) {
        return std::vector<size_t>(h, 0);
    }
    return Dp().seamOf(w, w, h, energy);
}

std::vector<float> carveWidth(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                              const std::vector<float>* protect, size_t newW,
                              const CancelFn& cancel) {
    newW = std::max<size_t>(newW, 1);
    if (newW == w || w == 0 || h == 0) {
        return img;
    }
    const size_t seams = std::max(newW > w ? newW - w : w - newW, size_t{1});
    if (newW < w) {
        if (h >= kProxyShrinkMinHeight && w - newW >= kProxyShrinkMinSeams) {
            return carveWidthProxyHeight(w, h, ch, img, protect, newW, cancel);
        }
        Carver carver(w, h, ch, std::vector<float>(img), protect, false);
        while (carver.cw > newW) {
            if (cancel && cancel()) {
                return {};
            }
            carver.removeSeam();
        }
        (void)seams;
        return carver.intoPixels();
    }
    // 放大：每轮至多加半宽，轮内复制 k 条最低缝（收缩副本上找，映射回原列）。
    std::vector<float> cur(img);
    size_t cw = w;
    std::vector<float> prot;
    bool hasProt = protect != nullptr;
    if (hasProt) {
        prot = *protect;
    }
    while (cw < newW) {
        const size_t k = std::min(newW - cw, std::max(cw / 2, size_t{1}));
        Carver carver(cw, h, ch, std::vector<float>(cur), hasProt ? &prot : nullptr, true);
        std::vector<std::vector<size_t>> dup(h);
        for (size_t s = 0; s < k; ++s) {
            if (carver.cw == 0) {
                break;
            }
            if (cancel && cancel()) {
                return {};
            }
            const std::vector<size_t> removed = carver.removeSeam();
            for (size_t y = 0; y < h; ++y) {
                dup[y].push_back(removed[y]);
            }
        }
        const size_t added = dup[0].size();
        if (added == 0) {
            break;
        }
        const size_t nw = cw + added;
        std::vector<float> out;
        out.reserve(nw * h * ch);
        std::vector<float> nprot;
        if (hasProt) {
            nprot.reserve(nw * h);
        }
        for (size_t y = 0; y < h; ++y) {
            std::vector<size_t>& d = dup[y];
            std::sort(d.begin(), d.end());
            const float* row = &cur[y * cw * ch];
            size_t di = 0;
            for (size_t x = 0; x < cw; ++x) {
                out.insert(out.end(), row + x * ch, row + (x + 1) * ch);
                if (hasProt) {
                    nprot.push_back(prot[y * cw + x]);
                }
                while (di < d.size() && d[di] == x) {
                    const size_t xr = std::min(x + 1, cw - 1);
                    for (size_t c = 0; c < ch; ++c) {
                        out.push_back((row[x * ch + c] + row[xr * ch + c]) * 0.5f);
                    }
                    if (hasProt) {
                        nprot.push_back(prot[y * cw + x]);
                    }
                    di += 1;
                }
            }
        }
        cur = std::move(out);
        prot = std::move(nprot);
        cw = nw;
    }
    return cur;
}

std::vector<float> carve(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                         const std::vector<float>* protect, size_t newW, size_t newH,
                         const CancelFn& cancel) {
    newW = std::max<size_t>(newW, 1);
    newH = std::max<size_t>(newH, 1);
    const std::vector<float> wide = carveWidth(w, h, ch, img, protect, newW, cancel);
    if (wide.empty()) {
        return {};
    }
    if (newH == h) {
        return wide;
    }
    // 保护图随宽变化走普通重采样（只引导缝）。
    std::vector<float> protT;
    bool hasProt = protect != nullptr;
    if (hasProt) {
        protT = transpose(newW, h, 1, resizeBilinear(w, h, 1, *protect, newW, h));
    }
    const std::vector<float> t = transpose(newW, h, ch, wide);
    std::vector<float> tall =
        carveWidth(h, newW, ch, t, hasProt ? &protT : nullptr, newH, cancel);
    if (tall.empty()) {
        return {};
    }
    return transpose(newH, newW, ch, tall);
}

std::vector<float> resizeBilinear(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                                  size_t nw, size_t nh) {
    std::vector<float> out(nw * nh * ch, 0.0f);
    if (w == 0 || h == 0) {
        return out;
    }
    for (size_t y = 0; y < nh; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(h) /
                                            static_cast<float>(nh) - 0.5f,
                                    0.0f, static_cast<float>(h - 1));
        const size_t y0 = static_cast<size_t>(fy);
        const float ty = fy - static_cast<float>(y0);
        const size_t y1 = std::min(y0 + 1, h - 1);
        for (size_t x = 0; x < nw; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(w) /
                                            static_cast<float>(nw) - 0.5f,
                                        0.0f, static_cast<float>(w - 1));
            const size_t x0 = static_cast<size_t>(fx);
            const float tx = fx - static_cast<float>(x0);
            const size_t x1 = std::min(x0 + 1, w - 1);
            for (size_t c = 0; c < ch; ++c) {
                auto p = [&](size_t xx, size_t yy) { return img[(yy * w + xx) * ch + c]; };
                const float top = p(x0, y0) + (p(x1, y0) - p(x0, y0)) * tx;
                const float bot = p(x0, y1) + (p(x1, y1) - p(x0, y1)) * tx;
                out[(y * nw + x) * ch + c] = top + (bot - top) * ty;
            }
        }
    }
    return out;
}

std::vector<float> skinMask(size_t w, size_t h, size_t ch, const std::vector<float>& img) {
    std::vector<float> m(w * h, 0.0f);
    if (ch < 3) {
        return m;
    }
    for (size_t i = 0; i < w * h; ++i) {
        const float r = img[i * ch] * 255.0f, g = img[i * ch + 1] * 255.0f,
                    b = img[i * ch + 2] * 255.0f;
        const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
        if (r > 95.0f && g > 40.0f && b > 20.0f && mx - mn > 15.0f && std::abs(r - g) > 15.0f &&
            r > g && r > b) {
            m[i] = 1.0f;
        }
    }
    return m;
}

}  // namespace seam
}  // namespace montage
