// 形状层渲染核实现（photocraft crates/vector/src/lib.rs CompiledShape 对拍，MIT OR Apache-2.0）。

#include "engine/vector_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/filter_ops.h"
#include "engine/vector_flatten.h"
#include "engine/vector_raster.h"
#include "engine/vector_shapes.h"
#include "engine/vector_stroke.h"

namespace montage {

namespace {
constexpr double kMaxDomainPixels = 64e6;  // 派生域像素预算（style_ops 同量级）

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void mix(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void mixD(double v) { mix(&v, sizeof(v)); }
    void mixU64(uint64_t v) { mix(&v, sizeof(v)); }
};

void hashPath(Fnv& f, const VectorPath& p) {
    f.mixU64(p.subpaths.size());
    f.mixU64(static_cast<uint64_t>(p.fillRule));
    f.mixU64(p.inverted ? 1 : 0);
    for (const Subpath& sp : p.subpaths) {
        f.mixU64(sp.closed ? 1 : 0);
        f.mixU64(static_cast<uint64_t>(sp.op));
        f.mixU64(sp.knots.size());
        for (const Knot& k : sp.knots) {
            f.mixD(k.anchor.x);
            f.mixD(k.anchor.y);
            f.mixD(k.inCtrl.x);
            f.mixD(k.inCtrl.y);
            f.mixD(k.outCtrl.x);
            f.mixD(k.outCtrl.y);
            f.mixU64(k.smooth ? 1 : 0);
        }
    }
}
}  // namespace

uint64_t shapeCacheHash(const ShapeLayerState& s, uint32_t docW, uint32_t docH) {
    Fnv f;
    hashPath(f, s.path);
    f.mixU64(static_cast<uint64_t>(s.fillKind));
    f.mix(s.fillColor, sizeof(s.fillColor));
    f.mixU64(s.hasStroke ? 1 : 0);
    if (s.hasStroke) {
        f.mixD(s.stroke.width);
        f.mix(s.stroke.color, sizeof(s.stroke.color));
        f.mixD(s.stroke.opacity);
        f.mixU64(static_cast<uint64_t>(s.stroke.align));
        f.mixU64(static_cast<uint64_t>(s.stroke.cap));
        f.mixU64(static_cast<uint64_t>(s.stroke.join));
        f.mixD(s.stroke.miterLimit);
        f.mixU64(s.stroke.dashes.size());
        for (double d : s.stroke.dashes) {
            f.mixD(d);
        }
        f.mixD(s.stroke.dashOffset);
    }
    f.mixU64(docW);
    f.mixU64(docH);
    return f.h;
}

namespace {

// 填充/描边覆盖度到 strip（直 alpha RGBA8888）。返回 false = 域为空。
bool renderShapeStrip(const ShapeLayerState& s, const VectorPath& path, int32_t x0, int32_t y0,
                      uint32_t w, uint32_t h, std::vector<uint8_t>& strip) {
    const size_t n = static_cast<size_t>(w) * h;
    strip.assign(n * 4, 0);
    const double tol = kVectorTolerance;
    bool hasFill = s.fillKind == 1 && (s.fillColor[3] > 0.0f || path.inverted);
    std::vector<float> fillCov;
    if (hasFill) {
        VectorRasterizer r = fillRasterizer(path, tol);
        fillCov = r.render(x0, y0, x0 + static_cast<int32_t>(w), y0 + static_cast<int32_t>(h));
    }
    bool hasStroke = s.hasStroke && s.stroke.width > 0.0 && s.stroke.opacity > 0.0 &&
                     s.stroke.color[3] > 0.0f;
    std::vector<float> strokeCov;
    std::vector<float> alignCov;  // inside/outside 裁剪用填充覆盖度
    bool alignInside = false;
    if (hasStroke) {
        bool allClosed = true;
        for (const Subpath& sp : path.subpaths) {
            if (!sp.closed) {
                allClosed = false;
            }
        }
        double width = s.stroke.width;
        if (allClosed && s.stroke.align != StrokeAlign::Center) {
            // 内/外描边 = 双宽中心描边 × 填充区裁剪（photocraft CompiledShape 同法）。
            width *= 2.0;
            alignInside = s.stroke.align == StrokeAlign::Inside;
        }
        VectorRasterizer r = strokeRasterizer(path, width, s.stroke.cap, s.stroke.join,
                                              std::max(1.0, s.stroke.miterLimit), s.stroke.dashes,
                                              s.stroke.dashOffset * s.stroke.width, tol);
        strokeCov = r.render(x0, y0, x0 + static_cast<int32_t>(w), y0 + static_cast<int32_t>(h));
        if (!alignCov.empty() || alignInside || (!alignInside && allClosed)) {
            VectorRasterizer rf = fillRasterizer(path, tol);
            alignCov = rf.render(x0, y0, x0 + static_cast<int32_t>(w), y0 + static_cast<int32_t>(h));
        }
    }
    if (!hasFill && !hasStroke) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        // 累积色（直 alpha 域 src-over：填充先、描边后 = photocraft render_rgba）。
        double cr = 0, cg = 0, cb = 0, ca = 0;
        if (hasFill) {
            float cov = fillCov[i];
            if (cov > 0.0f) {
                double sa = static_cast<double>(s.fillColor[3]) * cov;
                cr = s.fillColor[0];
                cg = s.fillColor[1];
                cb = s.fillColor[2];
                ca = sa;
            }
        }
        if (hasStroke) {
            float cov = strokeCov[i];
            if (!alignCov.empty()) {
                float fa = alignCov[i];
                cov *= alignInside ? fa : (1.0f - fa);
            }
            if (cov > 0.0f) {
                double sa = static_cast<double>(s.stroke.color[3]) * cov * s.stroke.opacity;
                double da = ca;
                double a = sa + da * (1.0 - sa);
                if (a > 0.0) {
                    cr = (s.stroke.color[0] * sa + cr * da * (1.0 - sa)) / a;
                    cg = (s.stroke.color[1] * sa + cg * da * (1.0 - sa)) / a;
                    cb = (s.stroke.color[2] * sa + cb * da * (1.0 - sa)) / a;
                }
                ca = a;
            }
        }
        if (ca > 0.0) {
            uint8_t* p = strip.data() + i * 4;
            p[0] = static_cast<uint8_t>(std::lround(std::clamp(cr, 0.0, 1.0) * 255.0));
            p[1] = static_cast<uint8_t>(std::lround(std::clamp(cg, 0.0, 1.0) * 255.0));
            p[2] = static_cast<uint8_t>(std::lround(std::clamp(cb, 0.0, 1.0) * 255.0));
            p[3] = static_cast<uint8_t>(std::lround(std::clamp(ca, 0.0, 1.0) * 255.0));
        }
    }
    return true;
}

}  // namespace

bool shapeRenderLayer(Document& doc, Layer& l, std::string& err) {
    ShapeLayerState& s = *l.shape;
    if (doc.width == 0 || doc.height == 0) {
        err = "no document";
        return false;
    }
    VectorPath path = s.live.kind == ShapeLiveKind::None ? s.path : liveShapePath(s.live);
    const uint64_t hash = shapeCacheHash(s, doc.width, doc.height);
    if (hash == s.cacheHash && l.shapeCache != nullptr) {
        return true;  // 缓存命中
    }
    // 编译期预算闸（photocraft validate_shape）：防文件提供的病态几何打爆渲染。
    const ShapeStroke* strokeForValidate = s.hasStroke ? &s.stroke : nullptr;
    if (!validateShape(path, strokeForValidate, err)) {
        return false;
    }
    // 域 = 形状包围盒 ∩ 画布；inverted 填充覆盖全画布。
    int32_t x0 = 0, y0 = 0;
    int32_t x1 = static_cast<int32_t>(doc.width), y1 = static_cast<int32_t>(doc.height);
    const bool inverted = path.inverted;
    if (!inverted) {
        double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        bool any = false;
        if (s.fillKind == 1 || s.hasStroke) {
            VectorRasterizer r = fillRasterizer(path, kVectorTolerance);
            int32_t fx0, fy0, fx1, fy1;
            if (r.pixelBounds(fx0, fy0, fx1, fy1)) {
                bx0 = fx0;
                by0 = fy0;
                bx1 = fx1;
                by1 = fy1;
                any = true;
            }
        }
        if (s.hasStroke && s.stroke.width > 0.0 && s.stroke.opacity > 0.0) {
            VectorRasterizer r = strokeRasterizer(path, s.stroke.width, s.stroke.cap, s.stroke.join,
                                                  std::max(1.0, s.stroke.miterLimit), s.stroke.dashes,
                                                  s.stroke.dashOffset * s.stroke.width,
                                                  kVectorTolerance);
            int32_t sx0, sy0, sx1, sy1;
            if (r.pixelBounds(sx0, sy0, sx1, sy1)) {
                if (!any) {
                    bx0 = sx0;
                    by0 = sy0;
                    bx1 = sx1;
                    by1 = sy1;
                    any = true;
                } else {
                    bx0 = std::min(bx0, static_cast<double>(sx0));
                    by0 = std::min(by0, static_cast<double>(sy0));
                    bx1 = std::max(bx1, static_cast<double>(sx1));
                    by1 = std::max(by1, static_cast<double>(sy1));
                }
            }
        }
        if (!any) {
            l.shapeCache = nullptr;
            s.cacheHash = hash;
            return true;
        }
        x0 = std::max(x0, static_cast<int32_t>(std::floor(bx0)));
        y0 = std::max(y0, static_cast<int32_t>(std::floor(by0)));
        x1 = std::min(x1, static_cast<int32_t>(std::ceil(bx1)));
        y1 = std::min(y1, static_cast<int32_t>(std::ceil(by1)));
    }
    if (x0 >= x1 || y0 >= y1) {
        l.shapeCache = nullptr;
        s.cacheHash = hash;
        return true;
    }
    const uint32_t w = static_cast<uint32_t>(x1 - x0);
    const uint32_t h = static_cast<uint32_t>(y1 - y0);
    if (static_cast<double>(w) * h > kMaxDomainPixels) {
        err = "shape domain over budget";
        return false;
    }
    std::vector<uint8_t> strip;
    if (!renderShapeStrip(s, path, x0, y0, w, h, strip)) {
        l.shapeCache = nullptr;
        s.cacheHash = hash;
        return true;
    }
    // 空结果（全部透明）→ 不留缓存。
    bool anyAlpha = false;
    for (size_t i = 3; i < strip.size(); i += 4) {
        if (strip[i] != 0) {
            anyAlpha = true;
            break;
        }
    }
    if (!anyAlpha) {
        l.shapeCache = nullptr;
        s.cacheHash = hash;
        return true;
    }
    l.shapeCache = filterStripToGrid(strip, w, h, nextGridRevision());
    l.shapeOriginX = static_cast<double>(x0);
    l.shapeOriginY = static_cast<double>(y0);
    s.cacheHash = hash;
    return true;
}

int shapeRenderDocument(Document& doc, std::string& firstErr) {
    int count = 0;
    for (Layer& l : doc.layers) {
        if (l.shape != nullptr) {
            std::string err;
            if (!shapeRenderLayer(doc, l, err) && firstErr.empty()) {
                firstErr = err;
            }
            count++;
        }
    }
    return count;
}

void translateShapeLayer(Layer& l, double dx, double dy) {
    if (l.shape == nullptr) {
        return;
    }
    ShapeLayerState& s = *l.shape;
    for (Subpath& sp : s.path.subpaths) {
        for (Knot& k : sp.knots) {
            k.anchor.x += dx;
            k.anchor.y += dy;
            k.inCtrl.x += dx;
            k.inCtrl.y += dy;
            k.outCtrl.x += dx;
            k.outCtrl.y += dy;
        }
    }
    if (s.live.kind != ShapeLiveKind::None) {
        // live 形状平移其参数域（矩形对角/线段端点）。
        if (s.live.kind == ShapeLiveKind::Line) {
            s.live.from[0] += dx;
            s.live.from[1] += dy;
            s.live.to[0] += dx;
            s.live.to[1] += dy;
        } else {
            s.live.rect[0] += dx;
            s.live.rect[1] += dy;
        }
    }
}

void affineShapeLayer(Layer& l, double a, double b, double c, double d, double e, double f) {
    if (l.shape == nullptr) {
        return;
    }
    ShapeLayerState& s = *l.shape;
    auto xf = [&](VPt& p) { p = vpt(a * p.x + c * p.y + e, b * p.x + d * p.y + f); };
    for (Subpath& sp : s.path.subpaths) {
        for (Knot& k : sp.knots) {
            xf(k.anchor);
            xf(k.inCtrl);
            xf(k.outCtrl);
        }
    }
    // live 形状仿射后不再保证参数可表示 → 退化为纯路径（可继续编辑锚点）。
    s.live.kind = ShapeLiveKind::None;
}

void rasterizeShapeLayerToPixels(Layer& l) {
    if (l.shape == nullptr) {
        return;
    }
    l.pixels = l.shapeCache;
    l.transform.originX = l.shapeOriginX;
    l.transform.originY = l.shapeOriginY;
    l.shape = nullptr;
    l.shapeCache = nullptr;
    l.shapeOriginX = 0.0;
    l.shapeOriginY = 0.0;
}

std::string shapeLayerDisplayName(const ShapeLayerState& s) {
    switch (s.live.kind) {
        case ShapeLiveKind::Rect:
            return "矩形";
        case ShapeLiveKind::Ellipse:
            return "椭圆";
        case ShapeLiveKind::Polygon:
            return s.live.starRatio < 1.0 ? "星形" : "多边形";
        case ShapeLiveKind::Line:
            return "直线";
        default:
            break;
    }
    return "形状";
}

std::shared_ptr<const TileGrid> coverageToMaskGrid(const std::vector<float>& cov, int32_t x0,
                                                   int32_t y0, int32_t w, int32_t h, uint32_t docW,
                                                   uint32_t docH) {
    if (docW == 0 || docH == 0 || w <= 0 || h <= 0 || cov.size() != static_cast<size_t>(w) * h) {
        return nullptr;
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    const int32_t x1 = std::min(x0 + w, static_cast<int32_t>(docW));
    const int32_t y1 = std::min(y0 + h, static_cast<int32_t>(docH));
    if (x0 >= x1 || y0 >= y1) {
        return nullptr;
    }
    TileGridBuilder builder((docW + kTileSize - 1u) / kTileSize, (docH + kTileSize - 1u) / kTileSize);
    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            const float g = cov[static_cast<size_t>(y - y0) * w + (x - x0)];
            if (g > 0.0f) {
                const uint8_t gray = static_cast<uint8_t>(std::lround(std::min(g, 1.0f) * 255.0f));
                uint8_t* px = builder.ensureTile(static_cast<uint32_t>(x) / kTileSize,
                                                 static_cast<uint32_t>(y) / kTileSize)
                                  .mapCpuWrite();
                uint8_t* p =
                    px + static_cast<size_t>(static_cast<uint32_t>(y) % kTileSize) * kTileSize * 4 +
                    static_cast<size_t>(static_cast<uint32_t>(x) % kTileSize) * 4;
                p[0] = gray;
                p[1] = gray;
                p[2] = gray;
                p[3] = 255;
            }
        }
    }
    return builder.publish(nextGridRevision());
}

std::shared_ptr<const TileGrid> pathToSelectionMask(const VectorPath& path, uint32_t docW,
                                                    uint32_t docH) {
    if (docW == 0 || docH == 0 || path.empty()) {
        return nullptr;
    }
    VectorRasterizer r = fillRasterizer(path, kVectorTolerance);
    int32_t x0, y0, x1, y1;
    if (!r.pixelBounds(x0, y0, x1, y1)) {
        return nullptr;
    }
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, static_cast<int32_t>(docW));
    y1 = std::min(y1, static_cast<int32_t>(docH));
    if (x0 >= x1 || y0 >= y1) {
        return nullptr;
    }
    auto cov = r.render(x0, y0, x1, y1);
    return coverageToMaskGrid(cov, x0, y0, x1 - x0, y1 - y0, docW, docH);
}

// ---- 选区 → 路径（M18c trace v1） ----

namespace {

// 掩码二值化（>127 = 内）。
inline bool maskOn(const TileGrid& g, int x, int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(g.cols) * kTileSize ||
        y >= static_cast<int>(g.rows) * kTileSize) {
        return false;
    }
    auto it = g.tiles.find((static_cast<uint32_t>(y) / kTileSize) * g.cols +
                           static_cast<uint32_t>(x) / kTileSize);
    if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
        return false;
    }
    const uint8_t* base = it->second->pixels->mapCpu();
    return base[static_cast<size_t>(static_cast<uint32_t>(y) % kTileSize) * kTileSize * 4 +
                static_cast<size_t>(static_cast<uint32_t>(x) % kTileSize) * 4] > 127;
}

// 像素 → 单位方块轮廓段集（marching-squares 变体，M7b selection_outline 同思路）：
// 段 = (x0,y0,x1,y1)，沿边界逆时针（y 向下域外圈为逆时针视觉）。
void traceBoundary(const TileGrid& g, std::vector<std::array<double, 4>>& segs) {
    const int w = static_cast<int>(g.cols) * kTileSize;
    const int h = static_cast<int>(g.rows) * kTileSize;
    for (int y = -1; y < h; y++) {
        for (int x = -1; x < w; x++) {
            const bool a = maskOn(g, x, y);          // 当前
            const bool b = maskOn(g, x + 1, y);      // 右
            const bool c = maskOn(g, x, y + 1);      // 下
            const bool d = maskOn(g, x + 1, y + 1);  // 右下
            const int code = (a ? 1 : 0) | (b ? 2 : 0) | (c ? 4 : 0) | (d ? 8 : 0);
            if (code == 0 || code == 15) {
                continue;
            }
            const double L = x, R = x + 1, T = y, B = y + 1;
            switch (code) {
                case 1: case 14: segs.push_back({L, T, L, B}); break;      // 左边
                case 2: case 13: segs.push_back({L, T, R, T}); break;      // 顶边
                case 4: case 11: segs.push_back({R, T, R, B}); break;      // 右边
                case 8: case 7: segs.push_back({R, B, L, B}); break;       // 底边
                case 3: segs.push_back({L, B, R, T}); break;   // 鞍：对角连接
                case 12: segs.push_back({L, T, R, B}); break;
                case 5: segs.push_back({L, T, L, B}); segs.push_back({R, T, R, B}); break;
                case 10: segs.push_back({L, T, R, T}); segs.push_back({R, B, L, B}); break;
                case 6: segs.push_back({R, B, L, T}); break;
                case 9: segs.push_back({R, T, L, B}); break;
                default: break;
            }
        }
    }
}

// 段接链成闭合环（端点 1e-6 吸附）。
std::vector<std::vector<VPt>> chainSegs(const std::vector<std::array<double, 4>>& segs) {
    std::vector<std::array<double, 4>> pool = segs;
    std::vector<std::vector<VPt>> loops;
    auto close = [](VPt a, VPt b) { return std::fabs(a.x - b.x) < 1e-6 && std::fabs(a.y - b.y) < 1e-6; };
    while (!pool.empty()) {
        std::array<double, 4> cur = pool.back();
        pool.pop_back();
        std::vector<VPt> loop = {vpt(cur[0], cur[1]), vpt(cur[2], cur[3])};
        bool extended = true;
        while (extended && !pool.empty()) {
            extended = false;
            for (size_t i = 0; i < pool.size(); i++) {
                if (close(loop.back(), vpt(pool[i][0], pool[i][1]))) {
                    loop.push_back(vpt(pool[i][2], pool[i][3]));
                } else if (close(loop.back(), vpt(pool[i][2], pool[i][3]))) {
                    loop.push_back(vpt(pool[i][0], pool[i][1]));
                } else {
                    continue;
                }
                pool[i] = pool.back();
                pool.pop_back();
                extended = true;
                break;
            }
            if (close(loop.front(), loop.back())) {
                loop.pop_back();
                break;
            }
        }
        if (loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }
    return loops;
}

// RDP 折线简化（闭合环：以最远两点分两段）。
void rdpSimplify(const std::vector<VPt>& pts, double eps, std::vector<bool>& keep) {
    // 迭代式 RDP（栈），pts 为开放折线。
    std::vector<std::pair<size_t, size_t>> stack = {{0, pts.size() - 1}};
    keep.assign(pts.size(), false);
    keep[0] = keep.back() = true;
    while (!stack.empty()) {
        auto [s, e] = stack.back();
        stack.pop_back();
        if (e <= s + 1) {
            continue;
        }
        double maxD = -1.0;
        size_t idx = s;
        const VPt& a = pts[s];
        const VPt& b = pts[e];
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double len = std::hypot(dx, dy);
        for (size_t i = s + 1; i < e; i++) {
            double d;
            if (len < 1e-12) {
                d = std::hypot(pts[i].x - a.x, pts[i].y - a.y);
            } else {
                d = std::fabs(dy * pts[i].x - dx * pts[i].y + b.x * a.y - b.y * a.x) / len;
            }
            if (d > maxD) {
                maxD = d;
                idx = i;
            }
        }
        if (maxD > eps) {
            keep[idx] = true;
            stack.push_back({s, idx});
            stack.push_back({idx, e});
        }
    }
}

// 角点检测：邻域夹角 < 100° = 角（采样半径 = 平均段长）。
std::vector<bool> detectCorners(const std::vector<VPt>& pts) {
    const size_t n = pts.size();
    std::vector<bool> corner(n, false);
    if (n < 3) {
        return corner;
    }
    double avg = 0.0;
    for (size_t i = 0; i < n; i++) {
        const VPt& a = pts[i];
        const VPt& b = pts[(i + 1) % n];
        avg += std::hypot(b.x - a.x, b.y - a.y);
    }
    avg /= static_cast<double>(n);
    const double r = std::max(2.0, avg);
    for (size_t i = 0; i < n; i++) {
        // 沿两侧走 r 距离取参考点。
        VPt prev = pts[i], next = pts[i];
        double acc = 0.0;
        for (size_t k = 1; k < n && acc < r; k++) {
            prev = pts[(i + n - k) % n];
            acc += std::hypot(pts[(i + n - k + 1) % n].x - prev.x, pts[(i + n - k + 1) % n].y - prev.y);
        }
        acc = 0.0;
        for (size_t k = 1; k < n && acc < r; k++) {
            next = pts[(i + k) % n];
            acc += std::hypot(pts[(i + k - 1) % n].x - next.x, pts[(i + k - 1) % n].y - next.y);
        }
        const double v1x = prev.x - pts[i].x, v1y = prev.y - pts[i].y;
        const double v2x = next.x - pts[i].x, v2y = next.y - pts[i].y;
        const double l1 = std::hypot(v1x, v1y), l2 = std::hypot(v2x, v2y);
        if (l1 < 1e-9 || l2 < 1e-9) {
            continue;
        }
        const double cosang = (v1x * v2x + v1y * v2y) / (l1 * l2);
        if (cosang < std::cos(100.0 * 3.14159265358979323846 / 180.0)) {
            corner[i] = true;
        }
    }
    return corner;
}

}  // namespace

bool traceSelectionToPath(const TileGrid& mask, VectorPath& out, size_t maxPoints) {
    out = VectorPath{};
    std::vector<std::array<double, 4>> segs;
    traceBoundary(mask, segs);
    if (segs.empty()) {
        return false;
    }
    std::vector<std::vector<VPt>> loops = chainSegs(segs);
    if (loops.empty()) {
        return false;
    }
    // 锚点预算按环周长比例分配。
    double totalLen = 0.0;
    for (auto& loop : loops) {
        for (size_t i = 0; i < loop.size(); i++) {
            const VPt& a = loop[i];
            const VPt& b = loop[(i + 1) % loop.size()];
            totalLen += std::hypot(b.x - a.x, b.y - a.y);
        }
    }
    size_t budget = std::min(std::max(maxPoints, static_cast<size_t>(64)), static_cast<size_t>(4096));
    // 全部环按 eps 自适应简化：eps 二分，记录可行（≤ 预算）的最细结果。
    double lo = 0.25, hi = 64.0;
    std::vector<std::vector<VPt>> simplified;
    std::vector<std::vector<VPt>> best;
    for (int iter = 0; iter < 24; iter++) {
        const double eps = 0.5 * (lo + hi);
        simplified.clear();
        size_t count = 0;
        for (auto& loop : loops) {
            std::vector<bool> keep;
            rdpSimplify(loop, eps, keep);
            std::vector<VPt> pts;
            for (size_t i = 0; i < loop.size(); i++) {
                if (keep[i]) {
                    pts.push_back(loop[i]);
                }
            }
            simplified.push_back(pts);
            count += pts.size();
        }
        if (count <= budget) {
            hi = eps;
            best = simplified;
        } else {
            lo = eps;
        }
    }
    for (auto& pts : best) {
        if (pts.size() < 3) {
            continue;
        }
        Subpath sp;
        sp.closed = true;
        sp.op = PathOp::Combine;
        sp.knots.reserve(pts.size());
        std::vector<bool> corners = detectCorners(pts);
        for (size_t i = 0; i < pts.size(); i++) {
            Knot k;
            k.anchor = pts[i];
            k.inCtrl = pts[i];
            k.outCtrl = pts[i];
            k.smooth = false;
            sp.knots.push_back(k);
        }
        out.subpaths.push_back(std::move(sp));
    }
    return !out.subpaths.empty();
}

}  // namespace montage
