#include "engine/style_ops.h"

#include "engine/smart_ops.h"

// M14c 渲染核。全部在连续 strip（直 alpha RGBA8888）上执行；发布走 filterStripToGrid。

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "engine/filter_ops.h"
#include "engine/filters.h"
#include "engine/vector_render.h"
#include "engine/vector_raster.h"
#include "engine/vector_flatten.h"
#include "io/image_export.h"

namespace montage {

namespace {

constexpr double kMaxBlurBudget = 64.0;   // 单项扩展上限（px）——防御异常参数
constexpr double kMaxPadBudget = 200.0;   // 派生域 pad 总上限
constexpr double kMaxDomainPixels = 64e6; // 派生域像素预算（M13 同量级）

// 底片（绕开样式派生——矢量蒙版派生（已含像素蒙版）→ 蒙版合成结果 → 形状缓存/原像素。
// M18：形状层无 pixels，样式作用于形状派生结果——PS「效果栅格化后的形状」语义）
std::shared_ptr<const TileGrid> basePixels(const Layer& l) {
    if (l.vectorMask != nullptr && l.vectorMask->enabled && l.vectorRender != nullptr) {
        return l.vectorRender;
    }
    if (l.mask != nullptr && l.mask->enabled && l.render != nullptr) {
        return l.render;
    }
    if (l.shape != nullptr) {
        return l.shapeCache;
    }
    return l.pixels;
}

// 网格局部坐标读 RGBA（缺瓦片返回 nullptr；调用方勿存指针）
const uint8_t* gridPixelAt(const TileGrid& g, uint32_t x, uint32_t y) {
    auto it = g.tiles.find((y / kTileSize) * g.cols + (x / kTileSize));
    if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
        return nullptr;
    }
    const uint8_t* base = it->second->pixels->mapCpu();
    return base + (static_cast<size_t>(y % kTileSize) * kTileSize + (x % kTileSize)) * 4;
}

// M18d 矢量蒙版重派生（enabled=false / 路径空 → vectorRender = null）。域同底片、
// origin 同底片（alpha × 路径覆盖度；空路径 = 全显不派生——PS「添加矢量蒙版」语义）。
void restyleVectorMask(Layer& l, uint64_t rev) {
    l.vectorRender = nullptr;
    if (l.vectorMask == nullptr || !l.vectorMask->enabled) {
        return;
    }
    const VectorPath& vp = l.vectorMask->path;
    if (vp.empty() && !vp.inverted) {
        return;  // 全显
    }
    const std::shared_ptr<const TileGrid> base =
        (l.mask != nullptr && l.mask->enabled && l.render != nullptr)
            ? l.render
            : (l.shape != nullptr ? l.shapeCache : l.pixels);
    if (base == nullptr) {
        return;
    }
    uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    if (!layerContentBBox(*base, bx, by, bw, bh)) {
        return;
    }
    const int gx0 = static_cast<int>(bx);
    const int gy0 = static_cast<int>(by);
    VectorRasterizer r = fillRasterizer(vp, kVectorTolerance);
    auto cov = r.render(gx0, gy0, gx0 + static_cast<int32_t>(bw), gy0 + static_cast<int32_t>(bh));
    // 底片 alpha × 覆盖度（域外覆盖度 = inverted ? 1 : 0）
    std::vector<uint8_t> strip(static_cast<size_t>(bw) * bh * 4, 0);
    const int gOriginXi = static_cast<int>(std::lround(l.shape != nullptr ? l.shapeOriginX : l.transform.originX));
    const int gOriginYi = static_cast<int>(std::lround(l.shape != nullptr ? l.shapeOriginY : l.transform.originY));
    (void)gOriginXi;
    (void)gOriginYi;
    for (uint32_t y = 0; y < bh; ++y) {
        for (uint32_t x = 0; x < bw; ++x) {
            const uint8_t* src = gridPixelAt(*base, bx + x, by + y);
            if (src == nullptr || src[3] == 0) {
                continue;
            }
            const float m = cov[static_cast<size_t>(y) * bw + x];
            uint8_t* p = strip.data() + (static_cast<size_t>(y) * bw + x) * 4;
            p[0] = src[0];
            p[1] = src[1];
            p[2] = src[2];
            p[3] = static_cast<uint8_t>(std::lround(src[3] * std::min(std::max(m, 0.0f), 1.0f)));
        }
    }
    l.vectorRender = filterStripToGrid(strip, bw, bh, rev);
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

// alpha 通道高斯（打包 A→RGBA strip 复用 M13 FGaussianBlur，RGB=0；σ 钳制与投影同款）
bool blurAlpha(std::vector<uint8_t>& a, int w, int h, double sigma, std::string& err) {
    if (sigma < 0.5) {
        return true;
    }
    const double s = std::min(kMaxBlurBudget, sigma);
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
    for (size_t i = 0; i < a.size(); ++i) {
        rgba[i * 4 + 3] = a[i];
    }
    filters::FilterParams p;
    p.kind = filters::FGaussianBlur;
    p.p0 = static_cast<float>(s);
    std::vector<uint8_t> out(rgba.size(), 0);
    if (!filters::applyFilter(rgba.data(), out.data(), static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h), p, err)) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        a[i] = out[i * 4 + 3];
    }
    return true;
}

// alpha 域内平移（photocraft Map::shifted 整数版：out(x,y)=src(x−dx,y−dy)，出界填 0）
void shiftAlpha(std::vector<uint8_t>& v, int w, int h, int dx, int dy) {
    std::vector<uint8_t> out(v.size(), 0);
    for (int y = 0; y < h; ++y) {
        const int sy = y - dy;
        if (sy < 0 || sy >= h) {
            continue;
        }
        for (int x = 0; x < w; ++x) {
            const int sx = x - dx;
            if (sx < 0 || sx >= w) {
                continue;
            }
            out[static_cast<size_t>(y) * w + x] = v[static_cast<size_t>(sy) * w + sx];
        }
    }
    v.swap(out);
}

// ---------- M17d 等高线（photocraft adjust.rs curve_lut / effects.rs contour_lut 移植） ----------

constexpr int kContourLutSize = 4096;

// 0..1 传递表插值取样（photocraft lut_at）
float contourLutAt(const std::vector<float>& l, float v) {
    const float x = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(l.size() - 1);
    const size_t i = static_cast<size_t>(x);
    const size_t j = std::min(i + 1, l.size() - 1);
    return l[i] + (l[j] - l[i]) * (x - static_cast<float>(i));
}

// 点列按 x 排序 + 近重合点去重（photocraft curve_lut 头部同款：|Δx|<1e-6 去重，保段距>0）
void contourSortDedup(std::vector<double>& xs, std::vector<double>& ys) {
    const size_t n = xs.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return xs[a] < xs[b]; });
    std::vector<double> sx, sy;
    sx.reserve(n);
    sy.reserve(n);
    for (size_t i : order) {
        if (!sx.empty() && std::fabs(xs[i] - sx.back()) < 1e-6) {
            continue;
        }
        sx.push_back(xs[i]);
        sy.push_back(ys[i]);
    }
    xs.swap(sx);
    ys.swap(sy);
}

// 自然样条 LUT（photocraft curve_lut 逐式对拍：三对角追赶解二阶导 m2，分段三次 Hermite
// 基函数求值，端点外平台，输出 clamp 0..1）
std::vector<float> contourCurveLut(const std::vector<double>& xsIn, const std::vector<double>& ysIn) {
    std::vector<double> xs = xsIn;
    std::vector<double> ys = ysIn;
    contourSortDedup(xs, ys);
    std::vector<float> out(static_cast<size_t>(kContourLutSize));
    if (xs.size() < 2) {
        for (int k = 0; k < kContourLutSize; ++k) {
            out[static_cast<size_t>(k)] = static_cast<float>(k) / (kContourLutSize - 1);
        }
        return out;
    }
    const size_t n = xs.size();
    std::vector<double> m2(n, 0.0);
    if (n > 2) {
        std::vector<double> c(n, 0.0);
        std::vector<double> d(n, 0.0);
        for (size_t i = 1; i + 1 < n; ++i) {
            const double h0 = xs[i] - xs[i - 1];
            const double h1 = xs[i + 1] - xs[i];
            const double a = h0 / 6.0;
            const double b = (h0 + h1) / 3.0;
            const double cc = h1 / 6.0;
            const double r = (ys[i + 1] - ys[i]) / h1 - (ys[i] - ys[i - 1]) / h0;
            const double denom = b - a * c[i - 1];
            c[i] = cc / denom;
            d[i] = (r - a * d[i - 1]) / denom;
        }
        for (size_t i = n - 2; i >= 1; --i) {
            m2[i] = d[i] - c[i] * m2[i + 1];
        }
    }
    for (int k = 0; k < kContourLutSize; ++k) {
        const double xv = static_cast<double>(k) / (kContourLutSize - 1);
        if (xv <= xs.front()) {
            out[static_cast<size_t>(k)] = static_cast<float>(std::clamp(ys.front(), 0.0, 1.0));
            continue;
        }
        if (xv >= xs[n - 1]) {
            out[static_cast<size_t>(k)] = static_cast<float>(std::clamp(ys[n - 1], 0.0, 1.0));
            continue;
        }
        size_t i = n - 2;
        for (size_t j = 0; j + 1 < n; ++j) {
            if (xv <= xs[j + 1]) {
                i = j;
                break;
            }
        }
        const double x0 = xs[i];
        const double x1 = xs[i + 1];
        const double y0 = ys[i];
        const double y1 = ys[i + 1];
        const double h = x1 - x0;
        const double t = xv;
        const double A = (x1 - t) / h;
        const double B = (t - x0) / h;
        const double y =
            A * y0 + B * y1 + ((A * A * A - A) * m2[i] + (B * B * B - B) * m2[i + 1]) * h * h / 6.0;
        out[static_cast<size_t>(k)] = static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
    return out;
}

// 覆盖度 uint8 图过 LUT（photocraft apply_contour：v→lut 线性插值；空表 = 恒等跳过）
void applyContourLut8(std::vector<uint8_t>& m, const std::vector<float>& l) {
    if (l.empty()) {
        return;
    }
    for (uint8_t& v : m) {
        v = static_cast<uint8_t>(std::lround(contourLutAt(l, v / 255.0f) * 255.0f));
    }
}

// M17b 斜面明暗图（photocraft bevel_height Smooth + bevel_maps shade_into）：
// 高度图 = 形状 alpha 高斯（近似 photocraft tent 双 box，登记 B 类差异）；中心差分梯度 ×
// depth（=depth×size×方向符号）；法线 (−gx,−gy,1)，光向量 (cosA·cosE, −sinA·cosE, sinE)；
// k=shade−sinE：>0 高光、<0 暗影。region：Inner=×alpha；Outer=非本体 × on_bevel（高度/4邻域>ε）。
bool bevelShade(const std::vector<uint8_t>& alpha, int w, int h, const LayerStyle* st,
                double angleDeg, std::vector<uint8_t>& hi, std::vector<uint8_t>& sh,
                std::string& err) {
    std::vector<uint8_t> h8 = alpha;
    if (!blurAlpha(h8, w, h, std::min(kMaxBlurBudget, std::max(0.0, st->bevelSize)), err)) {
        return false;
    }
    const size_t n = alpha.size();
    std::vector<float> hf(n);
    for (size_t i = 0; i < n; ++i) {
        hf[i] = h8[i] / 255.0f;
    }
    // M17d 等高线：高度轮廓 LUT 作用于高度图（photocraft bevel_contour_lut——Range 不拉伸
    // 斜面轮廓，photocraft 实测登记）；光泽等高线作用于明暗图（shade 后、量化前）。
    const std::vector<float> hcLut = buildContourLut(st->bevelContour);
    const std::vector<float> gcLut = buildContourLut(st->bevelGlossContour);
    if (!hcLut.empty()) {
        for (size_t i = 0; i < n; ++i) {
            hf[i] = contourLutAt(hcLut, hf[i]);
        }
    }
    const double az = angleDeg * 3.14159265358979323846 / 180.0;
    const double alt =
        std::clamp(st->bevelAltitudeDeg, 1.0, 89.0) * 3.14159265358979323846 / 180.0;
    const float lv0 = static_cast<float>(std::cos(az) * std::cos(alt));
    const float lv1 = static_cast<float>(-std::sin(az) * std::cos(alt));
    const float lv2 = static_cast<float>(std::sin(alt));
    const float se = static_cast<float>(std::sin(alt));
    const float depth = static_cast<float>(
        st->bevelDepth * std::min(kMaxBlurBudget, std::max(1.0, st->bevelSize)) *
        (st->bevelUp ? 1.0 : -1.0));
    const bool outer = st->bevelStyle == 1;
    hi.assign(n, 0);
    sh.assign(n, 0);
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            if (!outer) {
                if (alpha[i] == 0) {
                    continue;
                }
            } else {
                const bool onBevel = hf[i] > 1e-5f || hf[i - 1] > 1e-5f || hf[i + 1] > 1e-5f ||
                                     hf[i - w] > 1e-5f || hf[i + w] > 1e-5f;
                if (!(alpha[i] < 254 && onBevel)) {
                    continue;
                }
            }
            const float gx = (hf[i + 1] - hf[i - 1]) * 0.5f * depth;
            const float gy = (hf[i + w] - hf[i - w]) * 0.5f * depth;
            const float len = std::sqrt(gx * gx + gy * gy + 1.0f);
            const float shade = (-gx * lv0 - gy * lv1 + lv2) / len;
            const float k = shade - se;
            if (k > 0.0f) {
                float hv = std::min(1.0f, k / (1.0f - se));
                if (!gcLut.empty()) {
                    hv = contourLutAt(gcLut, hv);
                }
                hi[i] = static_cast<uint8_t>(std::lround(hv * 255.0f));
            } else {
                float sv = std::min(1.0f, -k / se);
                if (!gcLut.empty()) {
                    sv = contourLutAt(gcLut, sv);
                }
                sh[i] = static_cast<uint8_t>(std::lround(sv * 255.0f));
            }
        }
    }
    return true;
}

// 域内取底片 alpha 在展开段内直线推导（doc 坐标 = 网格 origin + 网格局部 = 域 origin + 域局部）

}  // namespace

// M17d 等高线 LUT（见 style_ops.h；photocraft contour_lut + contour_curve_lut 移植）。
std::vector<float> buildContourLut(const StyleContour& c) {
    if (!c.custom) {
        return {};
    }
    const size_t np = c.pts.size() / 2;
    if (np < 2) {
        return {};
    }
    bool anyCorner = false;
    for (uint8_t v : c.corners) {
        anyCorner = anyCorner || v != 0;
    }
    std::vector<double> xs(np);
    std::vector<double> ys(np);
    for (size_t i = 0; i < np; ++i) {
        xs[i] = std::clamp(c.pts[i * 2], 0.0f, 1.0f);
        ys[i] = std::clamp(c.pts[i * 2 + 1], 0.0f, 1.0f);
    }
    if (!anyCorner) {
        return contourCurveLut(xs, ys);
    }
    // 角点分段（photocraft contour_curve_lut）：按 x 排序后，角点之间的每段独立成自然样条
    //（全角点 = 折线）；后段覆盖 out 中 x ≥ 段首 x 的区间。
    std::vector<size_t> order(np);
    for (size_t i = 0; i < np; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return xs[a] < xs[b]; });
    std::vector<size_t> ptsIdx;
    ptsIdx.reserve(np);
    for (size_t i : order) {
        if (!ptsIdx.empty() && std::fabs(xs[i] - xs[ptsIdx.back()]) < 1e-6) {
            continue;
        }
        ptsIdx.push_back(i);
    }
    const auto isCorner = [&](size_t idx) -> bool {
        return idx < c.corners.size() && c.corners[idx] != 0;
    };
    std::vector<float> out;
    size_t start = 0;
    const size_t last = ptsIdx.size() - 1;
    for (size_t i = 1; i <= last; ++i) {
        const bool corner = isCorner(ptsIdx[i]);
        if (!(corner || i == last)) {
            continue;
        }
        std::vector<double> sx, sy;
        for (size_t j = start; j <= i; ++j) {
            sx.push_back(xs[ptsIdx[j]]);
            sy.push_back(ys[ptsIdx[j]]);
        }
        contourSortDedup(sx, sy);
        std::vector<float> lut = contourCurveLut(sx, sy);
        const double from = sx.front();
        if (out.empty()) {
            out = lut;
        } else {
            for (int k = 0; k < kContourLutSize; ++k) {
                if (static_cast<double>(k) / (kContourLutSize - 1) >= from) {
                    out[static_cast<size_t>(k)] = lut[static_cast<size_t>(k)];
                }
            }
        }
        start = i;
    }
    if (out.empty()) {
        return contourCurveLut(xs, ys);
    }
    return out;
}

// M17d 发光 Range（photocraft ranged_lut/glow_lut）：contour(min(v / range, 1))。
std::vector<float> buildRangedLut(const StyleContour& c, double range) {
    if (!std::isfinite(range)) {
        range = 1.0;
    }
    range = std::clamp(range, 0.01, 1.0);
    if (range >= 0.999) {
        return buildContourLut(c);
    }
    const std::vector<float> base = buildContourLut(c);
    std::vector<float> out(static_cast<size_t>(kContourLutSize));
    for (int k = 0; k < kContourLutSize; ++k) {
        const float v = std::min(
            static_cast<float>(static_cast<double>(k) / (kContourLutSize - 1) / range), 1.0f);
        out[static_cast<size_t>(k)] = base.empty() ? v : contourLutAt(base, v);
    }
    return out;
}

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
                           (st->dropShadow || st->stroke || st->colorOverlay || st->innerShadow ||
                            st->outerGlow || st->innerGlow || st->gradientOverlay || st->satin ||
                            st->bevel || st->patternOverlay);
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
        // M17b：useGL=true 用文档全局光角度（photocraft use_global_light）
        const double angDeg =
            st->shadowUseGL ? doc.globalLightAngle : st->shadowAngleDeg;
        const double rad = angDeg * 3.14159265358979323846 / 180.0;
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
    if (st->outerGlow) {
        // 外发光在层下向四周衰减（photocraft margin = size）
        pad = std::max(pad, std::min(kMaxBlurBudget, std::max(0.0, st->ogSize)) * 2.0);
    }
    if (st->bevel && st->bevelStyle == 1) {
        // M17b 外斜面在层下向外坡起（photocraft margin = size）
        pad = std::max(pad, std::min(kMaxBlurBudget, std::max(0.0, st->bevelSize)) * 2.0);
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
    // 1) 投影：alpha 纯覆盖度 → [M17b spread：dilate(size·spread) 先] → 高斯模糊
    // （M13 FGaussianBlur，σ=size·(1−spread)，photocraft spread_split）→ M17d 等高线 LUT
    //（photocraft shadow_map：contour 在 blur 后、作用于纯覆盖度；不透明度合成时才乘——
    // PS 语义 contour(contour(coverage)) 而非 contour(coverage×op)）→ 上色 → 偏移 src-over
    if (st->dropShadow) {
        std::vector<uint8_t> sh(static_cast<size_t>(w) * h * 4, 0);
        const float so = static_cast<float>(st->shadowOpacity);
        const double spread = std::clamp(st->shadowSpread, 0.0, 1.0);
        const double sizeEff = std::min(kMaxBlurBudget, std::max(0.0, st->shadowSize));
        {
            std::vector<uint8_t> shA = alpha;  // 纯覆盖度
            dilateAlpha(shA, static_cast<int>(w), static_cast<int>(h),
                        static_cast<int>(std::lround(sizeEff * spread)));
            if (sizeEff * (1.0 - spread) > 0.5) {
                filters::FilterParams p;
                p.kind = filters::FGaussianBlur;
                p.p0 = static_cast<float>(sizeEff * (1.0 - spread));
                std::vector<uint8_t> blurred(shA.size(), 0);
                if (!filters::applyFilter(shA.data(), blurred.data(),
                                          static_cast<uint32_t>(w), static_cast<uint32_t>(h), p,
                                          err)) {
                    return false;
                }
                shA.swap(blurred);
            }
            applyContourLut8(shA, buildContourLut(st->shadowContour));
            for (size_t i = 0; i < alpha.size(); ++i) {
                if (shA[i] == 0) {
                    continue;
                }
                sh[i * 4 + 0] = static_cast<uint8_t>(std::lround(st->shadowColor[0] * 255.0f));
                sh[i * 4 + 1] = static_cast<uint8_t>(std::lround(st->shadowColor[1] * 255.0f));
                sh[i * 4 + 2] = static_cast<uint8_t>(std::lround(st->shadowColor[2] * 255.0f));
                sh[i * 4 + 3] = static_cast<uint8_t>(
                    std::lround(shA[i] * so * st->shadowColor[3]));
            }
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
    // 2) 外发光（M17a/M17b，photocraft glow_map Softer：dilate(size·spread)+blur σ=size·(1−spread)
    // → M17d glow_lut = contour(min(v/range,1))；画在本体之下）
    if (st->outerGlow) {
        std::vector<uint8_t> cov = alpha;
        const double spread = std::clamp(st->ogSpread, 0.0, 1.0);
        const double sizeEff = std::min(kMaxBlurBudget, std::max(0.0, st->ogSize));
        dilateAlpha(cov, static_cast<int>(w), static_cast<int>(h),
                    static_cast<int>(std::lround(sizeEff * spread)));
        if (!blurAlpha(cov, static_cast<int>(w), static_cast<int>(h),
                       sizeEff * (1.0 - spread), err)) {
            return false;
        }
        applyContourLut8(cov, buildRangedLut(st->ogContour, st->ogRange));
        for (size_t i = 0; i < cov.size(); ++i) {
            if (cov[i] == 0) {
                continue;
            }
            float src[4] = {st->ogColor[0], st->ogColor[1], st->ogColor[2],
                            st->ogColor[3] * static_cast<float>(st->ogOpacity) *
                                (cov[i] / 255.0f)};
            srcOver(canvas.data() + i * 4, src);
        }
    }
    // 2b) 外斜面（M17b，photocraft BevelPaint::Outer：明暗画在层下、外发光之后）
    if (st->bevel && st->bevelStyle == 1) {
        const double angDeg = st->bevelUseGL ? doc.globalLightAngle : st->bevelAngleDeg;
        std::vector<uint8_t> hi, sh;
        if (!bevelShade(alpha, static_cast<int>(w), static_cast<int>(h), st, angDeg, hi, sh,
                        err)) {
            return false;
        }
        for (size_t i = 0; i < hi.size(); ++i) {
            if (hi[i] != 0) {
                float src[4] = {st->bevelHiColor[0], st->bevelHiColor[1], st->bevelHiColor[2],
                                st->bevelHiColor[3] * static_cast<float>(st->bevelHiOpacity) *
                                    (hi[i] / 255.0f)};
                srcOver(canvas.data() + i * 4, src);
            }
            if (sh[i] != 0) {
                float src[4] = {st->bevelShColor[0], st->bevelShColor[1], st->bevelShColor[2],
                                st->bevelShColor[3] * static_cast<float>(st->bevelShOpacity) *
                                    (sh[i] / 255.0f)};
                srcOver(canvas.data() + i * 4, src);
            }
        }
    }
    // 3) 本体：底片 RGB 按域坐标回取网格局部 src-over
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
    // 4) 渐变叠加（M17a，photocraft paint_fx Gradient：对齐图层=墨迹 bbox（gridInkBBox，
    // 非 tile 粒度域）；v1 双色线性；photocraft 顺序在颜色叠加之前）
    if (st->gradientOverlay) {
        uint32_t ix = bx, iy = by, iw = bw, ih = bh;
        gridInkBBox(*base, ix, iy, iw, ih);  // 失败保持 tile 粒度 bbox 兜底
        const double inkOX = l.transform.originX + static_cast<double>(ix);
        const double inkOY = l.transform.originY + static_cast<double>(iy);
        const double rad = st->gdAngleDeg * 3.14159265358979323846 / 180.0;
        const double gdx = std::cos(rad);
        const double gdy = std::sin(rad);
        const double cx = inkOX + static_cast<double>(iw) / 2.0;
        const double cy = inkOY + static_cast<double>(ih) / 2.0;
        const double halfLen =
            std::fabs(gdx) * static_cast<double>(iw) / 2.0 +
            std::fabs(gdy) * static_cast<double>(ih) / 2.0;
        for (size_t i = 0; i < alpha.size(); ++i) {
            if (alpha[i] == 0) {
                continue;
            }
            const double docX = static_cast<double>(x0 + static_cast<int>(i % w)) + 0.5;
            const double docY = static_cast<double>(y0 + static_cast<int>(i / w)) + 0.5;
            double t = halfLen > 0.0
                           ? (((docX - cx) * gdx + (docY - cy) * gdy) / halfLen + 1.0) / 2.0
                           : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const float ta = static_cast<float>(t);
            float src[4];
            for (int k = 0; k < 4; ++k) {
                src[k] = st->gdFrom[k] * (1.0f - ta) + st->gdTo[k] * ta;
            }
            src[3] *= static_cast<float>(st->gdOpacity) * (alpha[i] / 255.0f);
            srcOver(canvas.data() + i * 4, src);
        }
    }
    // 4b) 图案叠加（M17d，photocraft FxPaint::Pattern / PS「图案叠加」）：ink 墨迹内按图案
    // 平铺，锚点 = ink bbox 左上（PS「与图层对齐」默认语义），nearest × scale；渲染序在
    // 渐变叠加之后、颜色叠加之前（PS 效果栈序）。图案缺失/未选 = 不画。
    if (st->patternOverlay && !st->patternId.empty()) {
        const DocumentPattern* pat = nullptr;
        for (const DocumentPattern& p : doc.patterns) {
            if (p.id == st->patternId && p.pixels != nullptr && p.w > 0 && p.h > 0) {
                pat = &p;
                break;
            }
        }
        if (pat != nullptr) {
            uint32_t ix = bx, iy = by, iw = bw, ih = bh;
            gridInkBBox(*base, ix, iy, iw, ih);  // 失败保持 tile 粒度 bbox 兜底
            const double inkOX = l.transform.originX + static_cast<double>(ix);
            const double inkOY = l.transform.originY + static_cast<double>(iy);
            const double scale = std::clamp(st->poScale, 0.01, 10.0);
            const TileGrid& pg = *pat->pixels;
            const int pw = static_cast<int>(pat->w);
            const int phh = static_cast<int>(pat->h);
            auto patAt = [&](int px, int py) -> const uint8_t* {
                px %= pw;
                if (px < 0) {
                    px += pw;
                }
                py %= phh;
                if (py < 0) {
                    py += phh;
                }
                auto it = pg.tiles.find(
                    static_cast<uint32_t>(py) / kTileSize * pg.cols +
                    static_cast<uint32_t>(px) / kTileSize);
                if (it == pg.tiles.end() || it->second == nullptr ||
                    it->second->pixels == nullptr) {
                    return nullptr;
                }
                const uint8_t* p = it->second->pixels->mapCpu();
                if (p == nullptr) {
                    return nullptr;
                }
                return p + static_cast<size_t>(static_cast<uint32_t>(py) % kTileSize) *
                                  it->second->pixels->rowBytes() +
                           static_cast<size_t>(static_cast<uint32_t>(px) % kTileSize) * 4;
            };
            for (size_t i = 0; i < alpha.size(); ++i) {
                if (alpha[i] == 0) {
                    continue;
                }
                const double docX = static_cast<double>(x0 + static_cast<int>(i % w)) + 0.5;
                const double docY = static_cast<double>(y0 + static_cast<int>(i / w)) + 0.5;
                const int sx = static_cast<int>(std::floor((docX - inkOX) * scale));
                const int sy = static_cast<int>(std::floor((docY - inkOY) * scale));
                const uint8_t* q = patAt(sx, sy);
                if (q == nullptr || q[3] == 0) {
                    continue;
                }
                float src[4] = {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f,
                                q[3] / 255.0f * static_cast<float>(st->poOpacity) *
                                    (alpha[i] / 255.0f)};
                srcOver(canvas.data() + i * 4, src);
            }
        }
    }
    // 5) 颜色叠加：base alpha × overlay 色/不透明度 src-over（本体之上）
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
    // 6) 光泽（M17b，photocraft satin_map：形状双向平移(+d/−d) 各 σ=size 模糊 → |a−b| →
    // invert → M17d 等高线 → ×alpha；distance 下限 1px（PS Distance 0 画 1px，photocraft
    // satin_distance））
    if (st->satin) {
        const double rad = st->satinAngleDeg * 3.14159265358979323846 / 180.0;
        const double dist = std::max(1.0, st->satinDistance);
        const int dx = static_cast<int>(std::lround(-std::cos(rad) * dist));
        const int dy = static_cast<int>(std::lround(std::sin(rad) * dist));
        std::vector<uint8_t> sa = alpha;
        std::vector<uint8_t> sb = alpha;
        shiftAlpha(sa, static_cast<int>(w), static_cast<int>(h), dx, dy);
        shiftAlpha(sb, static_cast<int>(w), static_cast<int>(h), -dx, -dy);
        const double sizeEff = std::min(kMaxBlurBudget, std::max(0.0, st->satinSize));
        if (!blurAlpha(sa, static_cast<int>(w), static_cast<int>(h), sizeEff, err) ||
            !blurAlpha(sb, static_cast<int>(w), static_cast<int>(h), sizeEff, err)) {
            return false;
        }
        const std::vector<float> satinLut = buildContourLut(st->satinContour);
        for (size_t i = 0; i < alpha.size(); ++i) {
            if (alpha[i] == 0) {
                continue;
            }
            float m = std::abs(sa[i] - sb[i]) / 255.0f;
            if (st->satinInvert) {
                m = 1.0f - m;
            }
            if (!satinLut.empty()) {
                m = contourLutAt(satinLut, m);
            }
            m *= alpha[i] / 255.0f;
            if (m <= 0.0f) {
                continue;
            }
            float src[4] = {st->satinColor[0], st->satinColor[1], st->satinColor[2],
                            st->satinColor[3] * static_cast<float>(st->satinOpacity) * m};
            srcOver(canvas.data() + i * 4, src);
        }
    }
    // 7) 内发光（M17a/M17b，photocraft glow_map inner Softer：inv=1−a → dilate(choke) →
    // 模糊 → ×alpha 限层内）
    if (st->innerGlow) {
        std::vector<uint8_t> cov(alpha.size());
        for (size_t i = 0; i < cov.size(); ++i) {
            cov[i] = static_cast<uint8_t>(255 - alpha[i]);
        }
        const double spread = std::clamp(st->igSpread, 0.0, 1.0);
        const double sizeEff = std::min(kMaxBlurBudget, std::max(0.0, st->igSize));
        dilateAlpha(cov, static_cast<int>(w), static_cast<int>(h),
                    static_cast<int>(std::lround(sizeEff * spread)));
        if (!blurAlpha(cov, static_cast<int>(w), static_cast<int>(h),
                       sizeEff * (1.0 - spread), err)) {
            return false;
        }
        applyContourLut8(cov, buildRangedLut(st->igContour, st->igRange));
        for (size_t i = 0; i < cov.size(); ++i) {
            if (alpha[i] == 0) {
                continue;
            }
            const uint8_t m = static_cast<uint8_t>(
                std::lround(cov[i] * alpha[i] / 255.0f));
            if (m == 0) {
                continue;
            }
            float src[4] = {st->igColor[0], st->igColor[1], st->igColor[2],
                            st->igColor[3] * static_cast<float>(st->igOpacity) * (m / 255.0f)};
            srcOver(canvas.data() + i * 4, src);
        }
    }
    // 7) 内阴影（M17a/M17b，photocraft shadow_map inner：inv 平移(offset) 出界填满 →
    // dilate(choke) → 模糊 → ×alpha 限层内；offset=(round(−cos·d), round(sin·d)) 与投影同向）
    if (st->innerShadow) {
        const double angDeg = st->isUseGL ? doc.globalLightAngle : st->isAngleDeg;
        const double rad = angDeg * 3.14159265358979323846 / 180.0;
        const int dx = static_cast<int>(std::lround(-std::cos(rad) * st->isDistance));
        const int dy = static_cast<int>(std::lround(std::sin(rad) * st->isDistance));
        std::vector<uint8_t> inv(alpha.size());
        for (size_t i = 0; i < inv.size(); ++i) {
            inv[i] = static_cast<uint8_t>(255 - alpha[i]);
        }
        // photocraft 顺序：shift 后 dilate(choke)、再模糊——本实现 shift 在取样时做，
        // dilate 平移可交换（方形 SE 各向同性近似）
        const double spread = std::clamp(st->isSpread, 0.0, 1.0);
        const double sizeEff = std::min(kMaxBlurBudget, std::max(0.0, st->isSize));
        dilateAlpha(inv, static_cast<int>(w), static_cast<int>(h),
                    static_cast<int>(std::lround(sizeEff * spread)));
        if (!blurAlpha(inv, static_cast<int>(w), static_cast<int>(h),
                       sizeEff * (1.0 - spread), err)) {
            return false;
        }
        applyContourLut8(inv, buildContourLut(st->isContour));
        for (int yy = 0; yy < static_cast<int>(h); ++yy) {
            const int sy = yy - dy;
            for (int xx = 0; xx < static_cast<int>(w); ++xx) {
                const size_t i = static_cast<size_t>(yy) * w + xx;
                if (alpha[i] == 0) {
                    continue;
                }
                const int sx = xx - dx;
                const uint8_t v = (sx < 0 || sy < 0 || sx >= static_cast<int>(w) ||
                                   sy >= static_cast<int>(h))
                                      ? uint8_t{255}
                                      : inv[static_cast<size_t>(sy) * w + sx];
                const uint8_t m =
                    static_cast<uint8_t>(std::lround(v * alpha[i] / 255.0f));
                if (m == 0) {
                    continue;
                }
                float src[4] = {st->isColor[0], st->isColor[1], st->isColor[2],
                                st->isColor[3] * static_cast<float>(st->isOpacity) *
                                    (m / 255.0f)};
                srcOver(canvas.data() + i * 4, src);
            }
        }
    }
    // 7b) 内斜面（M17b，photocraft BevelPaint::Inner：明暗画在本体与全部内效之上、描边之下）
    if (st->bevel && st->bevelStyle == 0) {
        const double angDeg = st->bevelUseGL ? doc.globalLightAngle : st->bevelAngleDeg;
        std::vector<uint8_t> hi, sh;
        if (!bevelShade(alpha, static_cast<int>(w), static_cast<int>(h), st, angDeg, hi, sh,
                        err)) {
            return false;
        }
        for (size_t i = 0; i < hi.size(); ++i) {
            if (hi[i] != 0) {
                float src[4] = {st->bevelHiColor[0], st->bevelHiColor[1], st->bevelHiColor[2],
                                st->bevelHiColor[3] * static_cast<float>(st->bevelHiOpacity) *
                                    (hi[i] / 255.0f)};
                srcOver(canvas.data() + i * 4, src);
            }
            if (sh[i] != 0) {
                float src[4] = {st->bevelShColor[0], st->bevelShColor[1], st->bevelShColor[2],
                                st->bevelShColor[3] * static_cast<float>(st->bevelShOpacity) *
                                    (sh[i] / 255.0f)};
                srcOver(canvas.data() + i * 4, src);
            }
        }
    }
    // 8) 外描边（photocraft 顺序：stroke effect 在最顶——与外发光环带重叠时描边覆盖发光）
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
        if (l.smart != nullptr) {
            // M21：智能对象派生（先于形状/样式——样式底片吃 effectivePixels 含 smartCache）
            smart::renderLayer(l, err);
        }
        if (l.shape != nullptr) {
            // M18：形状缓存先重派生（带样式的形状层其底片 = shapeCache）
            shapeRenderLayer(doc, l, err);
        }
        if (l.vectorMask != nullptr) {
            // M18d：矢量蒙版派生（带样式的形状层其底片 = vectorRender）
            restyleVectorMask(l, rev.fetch_add(1));
        }
        if (l.style != nullptr || l.fill != nullptr) {
            restyleLayer(doc, l, rev.fetch_add(1), err);
        }
    }
}

void shiftLayerOrigins(Layer& l, double dx, double dy) {
    if (l.shape != nullptr) {
        // M18：形状层几何真源 = 路径点（translateShapeLayer 内含 live 参数域）。
        translateShapeLayer(l, dx, dy);
        return;
    }
    l.transform.originX += dx;
    l.transform.originY += dy;
    if (l.styledRender != nullptr) {
        l.styledOriginX += dx;
        l.styledOriginY += dy;
    }
}

}  // namespace montage
