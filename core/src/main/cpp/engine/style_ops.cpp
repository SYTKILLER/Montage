#include "engine/style_ops.h"

// M14c 渲染核。全部在连续 strip（直 alpha RGBA8888）上执行；发布走 filterStripToGrid。

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "engine/filter_ops.h"
#include "engine/filters.h"
#include "io/image_export.h"

namespace montage {

namespace {

constexpr double kMaxBlurBudget = 64.0;   // 单项扩展上限（px）——防御异常参数
constexpr double kMaxPadBudget = 200.0;   // 派生域 pad 总上限
constexpr double kMaxDomainPixels = 64e6; // 派生域像素预算（M13 同量级）

// 底片（绕开样式派生——蒙版合成结果，蒙版未启用回原像素）
std::shared_ptr<const TileGrid> basePixels(const Layer& l) {
    return (l.mask != nullptr && l.mask->enabled && l.render != nullptr) ? l.render : l.pixels;
}

// van Herk 一维 max 滤波（O(N)，方形 SE 两趟正交）。r=0 时原样拷贝。
void dilate1D(const uint8_t* src, uint8_t* dst, int n, int r) {
    if (r <= 0) {
        std::memcpy(dst, src, static_cast<size_t>(n));
        return;
    }
    // 前向块最大 + 后向块最大（van Herk/Gil-Werman）
    std::vector<uint8_t> pref(static_cast<size_t>(n));
    std::vector<uint8_t> suf(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        pref[static_cast<size_t>(i)] = (i % (2 * r + 1) == 0)
                                           ? src[static_cast<size_t>(i)]
                                           : std::max(pref[static_cast<size_t>(i - 1)],
                                                      src[static_cast<size_t>(i)]);
    }
    for (int i = n - 1; i >= 0; --i) {
        suf[static_cast<size_t>(i)] = ((i == n - 1) || (i % (2 * r + 1) == 2 * r))
                                          ? src[static_cast<size_t>(i)]
                                          : std::max(suf[static_cast<size_t>(i + 1)],
                                                     src[static_cast<size_t>(i)]);
    }
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - r);
        const int hi = std::min(n - 1, i + r);
        uint8_t m = suf[static_cast<size_t>(lo)];
        const int bStart = ((lo / (2 * r + 1)) + 1) * (2 * r + 1);
        if (bStart <= hi) {
            m = std::max(m, pref[static_cast<size_t>(hi)]);
        }
        dst[static_cast<size_t>(i)] = m;
    }
}

// alpha 通道方形膨胀（行距 = w）
void dilateAlpha(std::vector<uint8_t>& a, int w, int h, int r) {
    if (r <= 0) {
        return;
    }
    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        dilate1D(a.data() + static_cast<size_t>(y) * w, tmp.data() + static_cast<size_t>(y) * w,
                 w, r);
    }
    for (int x = 0; x < w; ++x) {
        std::vector<uint8_t> col(static_cast<size_t>(h));
        std::vector<uint8_t> outCol(static_cast<size_t>(h));
        for (int y = 0; y < h; ++y) {
            col[static_cast<size_t>(y)] = tmp[static_cast<size_t>(y) * w + x];
        }
        dilate1D(col.data(), outCol.data(), h, r);
        for (int y = 0; y < h; ++y) {
            tmp[static_cast<size_t>(y) * w + x] = outCol[static_cast<size_t>(y)];
        }
    }
    a.swap(tmp);
}

// src-over 画布合成（直 alpha 域；Normal 混合 B=src → Cr = (1−ab)·src + ab·src = src，
// 与 image_export 的 W3C 组装一致）
void srcOver(uint8_t* canvas, const float src[4]) {
    const float as = src[3];
    if (as <= 0.0f) {
        return;
    }
    const float ab = canvas[3] / 255.0f;
    float dst[3] = {canvas[0] / 255.0f, canvas[1] / 255.0f, canvas[2] / 255.0f};
    const float ao = as + ab * (1.0f - as);
    if (ao > 0.0001f) {
        for (int c = 0; c < 3; ++c) {
            const float coP = as * src[c] + (1.0f - as) * dst[c] * ab;
            canvas[c] = static_cast<uint8_t>(std::lround(coP / ao * 255.0f));
        }
        canvas[3] = static_cast<uint8_t>(std::lround(ao * 255.0f));
    } else {
        canvas[0] = canvas[1] = canvas[2] = canvas[3] = 0;
    }
}

// 域内取底片 alpha 在展开段内直线推导（doc 坐标 = 网格 origin + 网格局部 = 域 origin + 域局部）

}  // namespace

bool restyleLayer(Document& doc, Layer& l, uint64_t rev, std::string& err) {
    // 填充层：全文档域重生成
    if (l.fill != nullptr) {
        const FillLayerState& f = *l.fill;
        if (doc.width == 0 || doc.height == 0) {
            err = "no document";
            return false;
        }
        const size_t bytes = static_cast<size_t>(doc.width) * doc.height * 4;
        std::vector<uint8_t> strip(bytes, 0);
        for (uint32_t y = 0; y < doc.height; ++y) {
            for (uint32_t x = 0; x < doc.width; ++x) {
                uint8_t* p = strip.data() + (static_cast<size_t>(y) * doc.width + x) * 4;
                float c[4] = {f.color[0], f.color[1], f.color[2], f.color[3]};
                if (f.kind == 1) {
                    // 线性渐变（fg→bg，角度 deg；angle=90 → 上→下）：像素沿方向投影 t∈[0,1]
                    const double rad = f.angleDeg * 3.14159265358979323846 / 180.0;
                    const double dx = std::cos(rad);
                    const double dy = std::sin(rad);
                    const double cx = doc.width / 2.0;
                    const double cy = doc.height / 2.0;
                    const double halfLen =
                        std::fabs(dx) * doc.width / 2.0 + std::fabs(dy) * doc.height / 2.0;
                    const double t =
                        halfLen > 0.0
                            ? (((x + 0.5 - cx) * dx + (y + 0.5 - cy) * dy) / halfLen + 1.0) / 2.0
                            : 0.0;
                    const float tc = static_cast<float>(std::clamp(t, 0.0, 1.0));
                    // 前景 = fill 色，背景 = 背景色（引擎 bgColor 语义由 bridge 映射进 color）
                    for (int k = 0; k < 3; ++k) {
                        c[k] = f.color[k] * (1.0f - tc) + (1.0f - f.color[k]) * tc;
                    }
                    c[3] = f.color[3];
                }
                for (int k = 0; k < 3; ++k) {
                    p[k] = static_cast<uint8_t>(std::lround(c[k] * 255.0f));
                }
                p[3] = static_cast<uint8_t>(std::lround(c[3] * 255.0f));
            }
        }
        l.styledRender = filterStripToGrid(strip, doc.width, doc.height, rev);
        l.styledOriginX = 0.0;
        l.styledOriginY = 0.0;
        return true;
    }

    const LayerStyle* st = l.style.get();
    const bool anyEffect = st != nullptr && st->enabled &&
                           (st->dropShadow || st->stroke || st->colorOverlay);
    if (!anyEffect) {
        l.styledRender = nullptr;
        return true;
    }
    const std::shared_ptr<const TileGrid> base = basePixels(l);
    if (base == nullptr) {
        l.styledRender = nullptr;  // 空层无样式可渲染（首笔后重算）
        return true;
    }
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    if (!layerContentBBox(*base, bx, by, bw, bh)) {
        l.styledRender = nullptr;
        return true;
    }
    const double bOriginX = l.transform.originX + static_cast<double>(bx);
    const double bOriginY = l.transform.originY + static_cast<double>(by);
    // pad = 各激活效果扩展的包络
    double pad = 0.0;
    double offX = 0.0;
    double offY = 0.0;
    if (st->dropShadow) {
        const double rad = st->shadowAngleDeg * 3.14159265358979323846 / 180.0;
        // PS 角度：0=向右，90=向下（y 向下域 cos/sin 直用）
        offX = std::cos(rad) * st->shadowDistance;
        offY = std::sin(rad) * st->shadowDistance;
        const double blur = std::min(kMaxBlurBudget, std::max(0.0, st->shadowSize));
        pad = std::max(pad, std::fabs(offX) + blur * 2.0);
        pad = std::max(pad, std::fabs(offY) + blur * 2.0);
    }
    if (st->stroke) {
        pad = std::max(pad, std::min(kMaxBlurBudget, std::max(0.0, st->strokeWidth)));
    }
    pad = std::min(pad, kMaxPadBudget);
    // 派生域（doc 坐标）→ 局部尺寸
    int x0 = static_cast<int>(std::floor(bOriginX - pad));
    int y0 = static_cast<int>(std::floor(bOriginY - pad));
    int x1 = static_cast<int>(std::ceil(bOriginX + bw + pad));
    int y1 = static_cast<int>(std::ceil(bOriginY + bh + pad));
    const uint32_t w = static_cast<uint32_t>(x1 - x0);
    const uint32_t h = static_cast<uint32_t>(y1 - y0);
    if (w == 0 || h == 0 || static_cast<double>(w) * h > kMaxDomainPixels) {
        err = "style domain over budget";
        return false;
    }
    // 底片 alpha 展开到域：网格局部 (bx+xx, by+yy) → doc = origin + 局部 → 域局部 = doc − (x0,y0)
    const int gOriginXi = static_cast<int>(std::lround(l.transform.originX));
    const int gOriginYi = static_cast<int>(std::lround(l.transform.originY));
    std::vector<uint8_t> alpha(static_cast<size_t>(w) * h, 0);
    for (uint32_t yy = 0; yy < bh; ++yy) {
        const int sy = gOriginYi + static_cast<int>(by + yy) - y0;
        if (sy < 0 || sy >= static_cast<int>(h)) {
            continue;
        }
        for (uint32_t xx = 0; xx < bw; ++xx) {
            const uint32_t txx = bx + xx;
            const uint32_t tyy = by + yy;
            const uint32_t tx = txx / kTileSize;
            const uint32_t ty = tyy / kTileSize;
            auto it = base->tiles.find(ty * base->cols + tx);
            if (it == base->tiles.end() || it->second == nullptr ||
                it->second->pixels == nullptr) {
                continue;
            }
            const uint8_t* ps = it->second->pixels->mapCpu();
            if (ps == nullptr) {
                continue;
            }
            const uint8_t a =
                ps[(static_cast<size_t>(tyy % kTileSize) * it->second->pixels->rowBytes() +
                    (txx % kTileSize) * 4) + 3];
            if (a == 0) {
                continue;
            }
            const int sx = gOriginXi + static_cast<int>(txx) - x0;
            if (sx < 0 || sx >= static_cast<int>(w)) {
                continue;
            }
            alpha[static_cast<size_t>(sy) * w + sx] = a;
        }
    }
    std::vector<uint8_t> canvas(static_cast<size_t>(w) * h * 4, 0);
    // 1) 投影：alpha×opacity 置影色 → 高斯模糊（M13 FGaussianBlur，σ=size）→ 偏移 src-over
    if (st->dropShadow) {
        std::vector<uint8_t> sh(static_cast<size_t>(w) * h * 4, 0);
        const float so = static_cast<float>(st->shadowOpacity);
        for (size_t i = 0; i < alpha.size(); ++i) {
            if (alpha[i] == 0) {
                continue;
            }
            sh[i * 4 + 0] = static_cast<uint8_t>(std::lround(st->shadowColor[0] * 255.0f));
            sh[i * 4 + 1] = static_cast<uint8_t>(std::lround(st->shadowColor[1] * 255.0f));
            sh[i * 4 + 2] = static_cast<uint8_t>(std::lround(st->shadowColor[2] * 255.0f));
            sh[i * 4 + 3] = static_cast<uint8_t>(
                std::lround(alpha[i] * so * st->shadowColor[3]));
        }
        if (st->shadowSize > 0.5) {
            filters::FilterParams p;
            p.kind = filters::FGaussianBlur;
            p.p0 = static_cast<float>(std::min(kMaxBlurBudget, st->shadowSize));
            std::vector<uint8_t> blurred(sh.size(), 0);
            if (!filters::applyFilter(sh.data(), blurred.data(), w, h, p, err)) {
                return false;
            }
            sh.swap(blurred);
        }
        const int ox = static_cast<int>(std::lround(offX));
        const int oy = static_cast<int>(std::lround(offY));
        for (int yy = 0; yy < static_cast<int>(h); ++yy) {
            const int sy = yy - oy;
            if (sy < 0 || sy >= static_cast<int>(h)) {
                continue;
            }
            for (int xx = 0; xx < static_cast<int>(w); ++xx) {
                const int sx = xx - ox;
                if (sx < 0 || sx >= static_cast<int>(w)) {
                    continue;
                }
                const uint8_t* s =
                    sh.data() + (static_cast<size_t>(sy) * w + sx) * 4;
                if (s[3] == 0) {
                    continue;
                }
                float src[4] = {s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f};
                srcOver(canvas.data() + (static_cast<size_t>(yy) * w + xx) * 4, src);
            }
        }
    }
    // 2) 外描边：膨胀环带 = dilate(alpha, w) − alpha → 描边色 src-over
    if (st->stroke && st->strokeWidth >= 0.5) {
        const int r = static_cast<int>(std::lround(std::min(kMaxBlurBudget, st->strokeWidth)));
        std::vector<uint8_t> dil = alpha;
        dilateAlpha(dil, static_cast<int>(w), static_cast<int>(h), r);
        for (size_t i = 0; i < alpha.size(); ++i) {
            const int band = static_cast<int>(dil[i]) - static_cast<int>(alpha[i]);
            if (band <= 0) {
                continue;
            }
            float src[4] = {st->strokeColor[0], st->strokeColor[1], st->strokeColor[2],
                            st->strokeColor[3] * (band / 255.0f)};
            srcOver(canvas.data() + i * 4, src);
        }
    }
    // 3) 本体：底片 RGB 按域坐标回取网格局部 src-over
    for (size_t i = 0; i < alpha.size(); ++i) {
        if (alpha[i] == 0) {
            continue;
        }
        const int docX = x0 + static_cast<int>(i % w);
        const int docY = y0 + static_cast<int>(i / w);
        const int gx = docX - gOriginXi;
        const int gy = docY - gOriginYi;
        float src[4] = {0.0f, 0.0f, 0.0f, alpha[i] / 255.0f};
        if (gx >= 0 && gy >= 0) {
            const uint32_t txx = static_cast<uint32_t>(gx);
            const uint32_t tyy = static_cast<uint32_t>(gy);
            const uint32_t tx = txx / kTileSize;
            const uint32_t ty = tyy / kTileSize;
            auto it = base->tiles.find(ty * base->cols + tx);
            if (it != base->tiles.end() && it->second != nullptr &&
                it->second->pixels != nullptr) {
                const uint8_t* ps = it->second->pixels->mapCpu();
                if (ps != nullptr) {
                    const uint8_t* p =
                        ps + (static_cast<size_t>(tyy % kTileSize) *
                                  it->second->pixels->rowBytes() +
                              (txx % kTileSize) * 4);
                    src[0] = p[0] / 255.0f;
                    src[1] = p[1] / 255.0f;
                    src[2] = p[2] / 255.0f;
                }
            }
        }
        srcOver(canvas.data() + i * 4, src);
    }
    // 4) 颜色叠加：base alpha × overlay 色/不透明度 src-over（本体之上）
    if (st->colorOverlay) {
        for (size_t i = 0; i < alpha.size(); ++i) {
            if (alpha[i] == 0) {
                continue;
            }
            float src[4] = {st->overlayColor[0], st->overlayColor[1], st->overlayColor[2],
                            st->overlayColor[3] * static_cast<float>(st->overlayOpacity) *
                                (alpha[i] / 255.0f)};
            srcOver(canvas.data() + i * 4, src);
        }
    }
    l.styledRender = filterStripToGrid(canvas, w, h, rev);
    l.styledOriginX = x0;
    l.styledOriginY = y0;
    return true;
}

void invalidateStyled(Layer& l) {
    l.styledRender = nullptr;
}

void restyleDocument(Document& doc) {
    static std::atomic<uint64_t> rev{1};
    std::string err;
    for (Layer& l : doc.layers) {
        if (l.style != nullptr || l.fill != nullptr) {
            restyleLayer(doc, l, rev.fetch_add(1), err);
        }
    }
}

void shiftLayerOrigins(Layer& l, double dx, double dy) {
    l.transform.originX += dx;
    l.transform.originY += dy;
    if (l.styledRender != nullptr) {
        l.styledOriginX += dx;
        l.styledOriginY += dy;
    }
}

}  // namespace montage
