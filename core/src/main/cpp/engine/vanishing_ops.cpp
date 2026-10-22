// M20d 消失点 —— photocraft crates/algo/src/vanishing.rs C++ 翻译（MIT OR Apache-2.0）。

#include "engine/vanishing_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace vanishing {

namespace {
constexpr double kPi = 3.14159265358979323846;

V3 sub3(const V3& a, const V3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 add3(const V3& a, const V3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
V3 mul3(const V3& a, double k) {
    return {a[0] * k, a[1] * k, a[2] * k};
}
double dot3(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross3(const V3& a, const V3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 norm3(const V3& a) {
    return mul3(a, 1.0 / std::max(std::sqrt(dot3(a, a)), 1e-300));
}

// 齐次线 (a,b)×(c,d) 交点
V3 meet(const std::array<double, 2>& a, const std::array<double, 2>& b,
        const std::array<double, 2>& c, const std::array<double, 2>& d) {
    const V3 l1 = cross3({a[0], a[1], 1.0}, {b[0], b[1], 1.0});
    const V3 l2 = cross3({c[0], c[1], 1.0}, {d[0], d[1], 1.0});
    return cross3(l1, l2);
}

bool finiteV(const V3& v) {
    return std::fabs(v[2]) > 1e-9 * std::max(std::fabs(v[0]) + std::fabs(v[1]), 1.0);
}
}  // namespace

bool VpPlane::homography(perspective::Homography& out) const {
    const double rect[4] = {0.0, 0.0, 1.0, 1.0};
    return perspective::Homography::rectToQuad(rect, corners, out);
}

std::optional<double> VpPlane::focal(const std::array<double, 2>& c) const {
    const auto& q = corners;
    const V3 v1 = meet(q[0], q[1], q[3], q[2]);
    const V3 v2 = meet(q[0], q[3], q[1], q[2]);
    if (std::fabs(v1[2]) < 1e-9 || std::fabs(v2[2]) < 1e-9) {
        return std::nullopt;
    }
    const double a[2] = {v1[0] / v1[2] - c[0], v1[1] / v1[2] - c[1]};
    const double b[2] = {v2[0] / v2[2] - c[0], v2[1] / v2[2] - c[1]};
    const double f2 = -(a[0] * b[0] + a[1] * b[1]);
    if (f2 <= 1.0) {
        return std::nullopt;
    }
    return std::sqrt(f2);
}

bool VpPlane::turnsImpl(std::array<double, 4>& out) const {
    const auto& q = corners;
    for (const auto& p : q) {
        for (const double v : p) {
            if (!std::isfinite(v) || std::fabs(v) > 1e9) {
                return false;
            }
        }
    }
    auto edge = [&](int i) -> std::array<double, 2> {
        return {q[(i + 1) % 4][0] - q[i][0], q[(i + 1) % 4][1] - q[i][1]};
    };
    for (int i = 0; i < 4; ++i) {
        const auto a = edge(i);
        const auto b = edge((i + 1) % 4);
        const double la = std::hypot(a[0], a[1]);
        const double lb = std::hypot(b[0], b[1]);
        if (la < 1.0 || lb < 1.0) {
            return false;
        }
        out[i] = (a[0] * b[1] - a[1] * b[0]) / (la * lb);
    }
    return true;
}

Lifted VpPlane::lift(double f, const std::array<double, 2>& c) const {
    Lifted lf;
    lf.focal = f;
    lf.center = c;
    auto ray = [&](const std::array<double, 2>& p) -> V3 {
        return {(p[0] - c[0]) / f, (p[1] - c[1]) / f, 1.0};
    };
    const auto& q = corners;
    auto dir = [&](const std::array<double, 2>& a, const std::array<double, 2>& b,
                   const std::array<double, 2>& cc, const std::array<double, 2>& d) -> V3 {
        const V3 v = meet(a, b, cc, d);
        if (std::fabs(v[2]) < 1e-9 * std::max(std::fabs(v[0]) + std::fabs(v[1]), 1.0)) {
            return norm3({v[0], v[1], 0.0});
        }
        return norm3(ray({v[0] / v[2], v[1] / v[2]}));
    };
    const V3 d1 = dir(q[0], q[1], q[3], q[2]);
    const V3 d2 = dir(q[0], q[3], q[1], q[2]);
    V3 n = norm3(cross3(d1, d2));
    if (n[2] < 0.0) {
        n = mul3(n, -1.0);
    }
    for (int k = 0; k < 4; ++k) {
        const V3 r = ray(q[k]);
        const double lam = 1.0 / std::max(std::fabs(dot3(n, r)), 1e-9);
        lf.pts[k] = mul3(r, lam);
    }
    return lf;
}

int VpPlane::check(const std::array<double, 2>& center) const {
    std::array<double, 4> t{0, 0, 0, 0};
    if (!turnsImpl(t)) {
        return 2;  // Invalid
    }
    const bool sameSign = (t[0] > 0 && t[1] > 0 && t[2] > 0 && t[3] > 0) ||
                          (t[0] < 0 && t[1] < 0 && t[2] < 0 && t[3] < 0);
    double mn = 1e308;
    for (const double v : t) {
        mn = std::min(mn, std::fabs(v));
    }
    perspective::Homography h;
    if (!sameSign || mn < std::sin(1.0 * kPi / 180.0) || !homography(h)) {
        return 2;
    }
    if (mn < std::sin(10.0 * kPi / 180.0)) {
        return 1;  // Questionable
    }
    const auto& q = corners;
    const V3 v1 = meet(q[0], q[1], q[3], q[2]);
    const V3 v2 = meet(q[0], q[3], q[1], q[2]);
    if (finiteV(v1) && finiteV(v2) && !focal(center).has_value()) {
        return 1;
    }
    return 0;  // Valid
}

VpPlane VpPlane::normalized() const {
    std::array<std::array<double, 2>, 4> q = corners;
    std::array<double, 4> t{0, 0, 0, 0};
    if (turnsImpl(t) && (t[0] + t[1] + t[2] + t[3]) < 0.0) {
        q = {q[0], q[3], q[2], q[1]};
    }
    int first = 0;
    double best = 1e308;
    for (int i = 0; i < 4; ++i) {
        const double s = q[i][0] + q[i][1];
        if (s < best) {
            best = s;
            first = i;
        }
    }
    VpPlane out;
    for (int i = 0; i < 4; ++i) {
        out.corners[i] = q[(first + i) % 4];
    }
    return out;
}

std::optional<std::array<double, 2>> Lifted::project(const V3& x) const {
    if (x[2] <= 1e-9) {
        return std::nullopt;
    }
    return std::array<double, 2>{center[0] + focal * x[0] / x[2], center[1] + focal * x[1] / x[2]};
}

void Lifted::lengths(double& lu, double& lv) const {
    auto l = [](const V3& a, const V3& b) { return std::sqrt(dot3(sub3(a, b), sub3(a, b))); };
    lu = (l(pts[0], pts[1]) + l(pts[3], pts[2])) / 2.0;
    lv = (l(pts[0], pts[3]) + l(pts[1], pts[2])) / 2.0;
}

Scene::Scene(const VpPlane& first, const std::array<double, 2>& c, double focalIn, double fallback) {
    planes.push_back(first);
    center = c;
    focal = fallback;
    if (focalIn > 1.0) {
        focal = focalIn;
    } else if (const auto f = first.focal(c)) {
        focal = *f;
    }
}

Lifted Scene::lifted(int i) const {
    const auto& q = planes[i].corners;
    Lifted lf;
    lf.focal = focal;
    lf.center = center;
    auto ray = [&](const std::array<double, 2>& p) -> V3 {
        return {(p[0] - center[0]) / focal, (p[1] - center[1]) / focal, 1.0};
    };
    auto dir = [&](const std::array<double, 2>& a, const std::array<double, 2>& b,
                   const std::array<double, 2>& cc, const std::array<double, 2>& d) -> V3 {
        const V3 v = meet(a, b, cc, d);
        if (std::fabs(v[2]) < 1e-9 * std::max(std::fabs(v[0]) + std::fabs(v[1]), 1.0)) {
            return norm3({v[0], v[1], 0.0});
        }
        return norm3(ray({v[0] / v[2], v[1] / v[2]}));
    };
    const V3 d1 = dir(q[0], q[1], q[3], q[2]);
    const V3 d2 = dir(q[0], q[3], q[1], q[2]);
    V3 n = norm3(cross3(d1, d2));
    if (n[2] < 0.0) {
        n = mul3(n, -1.0);
    }
    for (int k = 0; k < 4; ++k) {
        const V3 r = ray(q[k]);
        const double lam = 1.0 / std::max(std::fabs(dot3(n, r)), 1e-9);
        lf.pts[k] = mul3(r, lam);
    }
    return lf;
}

std::optional<int> Scene::tearOff(int from, Edge edge, double angle, double depth) {
    const Lifted lf = lifted(from);
    const V3* p = lf.pts;
    int ia = 0, ib = 0, ic = 0;
    switch (edge) {
        case Edge::Top: ia = 0; ib = 1; ic = 3; break;
        case Edge::Right: ia = 1; ib = 2; ic = 0; break;
        case Edge::Bottom: ia = 2; ib = 3; ic = 1; break;
        case Edge::Left: ia = 3; ib = 0; ic = 2; break;
    }
    const V3 a = p[ia];
    const V3 b = p[ib];
    const V3 axis = norm3(sub3(b, a));
    const V3 inward = sub3(p[ic], p[ia]);
    const double len = std::sqrt(dot3(inward, inward)) * std::max(depth, 0.05);
    const V3 out = mul3(norm3(inward), -1.0);
    const double th = (180.0 - angle) * kPi / 180.0;
    double bestScore = -1e308;
    bool have = false;
    std::array<std::array<double, 2>, 4> bestPts{};
    for (const double sgn : {1.0, -1.0}) {
        const double s = std::sin(sgn * th);
        const double c = std::cos(sgn * th);
        const V3 rot = add3(add3(mul3(out, c), mul3(cross3(axis, out), s)),
                            mul3(axis, dot3(axis, out) * (1.0 - c)));
        const V3 a2 = add3(a, mul3(rot, len));
        const V3 b2 = add3(b, mul3(rot, len));
        const auto pa = lf.project(a);
        const auto pb = lf.project(b);
        const auto pa2 = lf.project(a2);
        const auto pb2 = lf.project(b2);
        if (!pa || !pb || !pa2 || !pb2) {
            continue;
        }
        V3 n2 = norm3(cross3(sub3(b, a), sub3(a2, a)));
        if (dot3(n2, a) > 0.0) {
            n2 = mul3(n2, -1.0);
        }
        const double score = dot3(n2, inward);
        if (!have || score > bestScore) {
            have = true;
            bestScore = score;
            bestPts = {*pa, *pb, *pa2, *pb2};
        }
    }
    if (!have) {
        return std::nullopt;
    }
    const auto& [pa, pb, pa2, pb2] = bestPts;
    VpPlane np;
    switch (edge) {
        case Edge::Top: np.corners = {pa2, pb2, pb, pa}; break;
        case Edge::Right: np.corners = {pa, pa2, pb2, pb}; break;
        case Edge::Bottom: np.corners = {pb, pa, pa2, pb2}; break;
        case Edge::Left: np.corners = {pb2, pb, pa, pa2}; break;
    }
    planes.push_back(np);
    return static_cast<int>(planes.size() - 1);
}

bool Scene::frame(int i, perspective::Homography& h, double& lu, double& lv) const {
    if (!planes[i].homography(h)) {
        return false;
    }
    lifted(i).lengths(lu, lv);
    return true;
}

std::optional<int> Scene::planeAt(const std::array<double, 2>& p) const {
    for (int i = static_cast<int>(planes.size()) - 1; i >= 0; --i) {
        perspective::Homography h;
        if (!planes[i].homography(h)) {
            continue;
        }
        perspective::Homography inv;
        if (!h.invert(inv)) {
            continue;
        }
        const auto uv = inv.apply(p[0], p[1]);
        if (uv[0] >= 0.0 && uv[0] <= 1.0 && uv[1] >= 0.0 && uv[1] <= 1.0) {
            return i;
        }
    }
    return std::nullopt;
}

namespace {
// 平面覆盖 0/1 蒙版
std::vector<float> quadMask(const perspective::Homography& h, const RectI& area) {
    perspective::Homography inv;
    std::vector<float> out(static_cast<size_t>(std::max(0, area.width())) * area.height(), 0.0f);
    if (!h.invert(inv)) {
        return out;
    }
    const int w = area.width();
    for (size_t i = 0; i < out.size(); ++i) {
        const double x = area.x0 + static_cast<double>(i % w) + 0.5;
        const double y = area.y0 + static_cast<double>(i / w) + 0.5;
        const auto uv = inv.apply(x, y);
        if (uv[0] >= 0.0 && uv[0] <= 1.0 && uv[1] >= 0.0 && uv[1] <= 1.0) {
            out[i] = 1.0f;
        }
    }
    return out;
}

// 双三次采样（frame 钳）
float bicubicAt(const Img& img, double x, double y, int c) {
    const double cx = std::clamp(x, 0.0, static_cast<double>(img.w - 1));
    const double cy = std::clamp(y, 0.0, static_cast<double>(img.h - 1));
    const double xi = std::floor(cx);
    const double yi = std::floor(cy);
    const int ix = static_cast<int>(xi);
    const int iy = static_cast<int>(yi);
    const double tx = cx - xi;
    const double ty = cy - yi;
    auto row = [&](int j) {
        const int yy = std::clamp(iy + j, 0, img.h - 1);
        return &img.px[(static_cast<size_t>(yy) * img.w) * 4u];
    };
    auto w0 = [](double t) { return -0.5 * t * t * t + t * t - 0.5 * t; };
    auto w1 = [](double t) { return 1.5 * t * t * t - 2.5 * t * t + 1.0; };
    auto w2 = [](double t) { return -1.5 * t * t * t + 2.0 * t * t + 0.5 * t; };
    auto w3 = [](double t) { return 0.5 * t * t * t - 0.5 * t * t; };
    const double wx[4] = {w0(tx), w1(tx), w2(tx), w3(tx)};
    const double wy[4] = {w0(ty), w1(ty), w2(ty), w3(ty)};
    double acc = 0.0;
    for (int j = 0; j < 4; ++j) {
        const int yy = std::clamp(iy - 1 + j, 0, img.h - 1);
        const float* r = &img.px[(static_cast<size_t>(yy) * img.w) * 4u];
        for (int i = 0; i < 4; ++i) {
            const int xx = std::clamp(ix - 1 + i, 0, img.w - 1);
            acc += wx[i] * wy[j] * r[static_cast<size_t>(xx) * 4u + c];
        }
    }
    return static_cast<float>(acc);
}
}  // namespace

std::optional<PasteResult> paste(const Scene& sc, int i, const Img& src, const RectI& srcRect,
                                 const std::array<double, 2>& at, double width) {
    perspective::Homography h;
    double lu = 0.0, lv = 0.0;
    if (!sc.frame(i, h, lu, lv)) {
        return std::nullopt;
    }
    const double sw = srcRect.width();
    const double sh = srcRect.height();
    if (sw <= 0.0 || sh <= 0.0) {
        return std::nullopt;
    }
    const double su = width / sw;
    const double sv = width * lu / lv / sw;
    perspective::Homography toPlane;
    toPlane.h[0] = su;
    toPlane.h[1] = 0.0;
    toPlane.h[2] = at[0] - srcRect.x0 * su;
    toPlane.h[3] = 0.0;
    toPlane.h[4] = sv;
    toPlane.h[5] = at[1] - srcRect.y0 * sv;
    const perspective::Homography full = h.mul(toPlane);
    // 输出 bbox = srcRect 四角经 full 的包围
    double x0 = 1e308, y0 = 1e308, x1 = -1e308, y1 = -1e308;
    const std::array<std::array<double, 2>, 4> cs = {
        {{static_cast<double>(srcRect.x0), static_cast<double>(srcRect.y0)},
         {static_cast<double>(srcRect.x1), static_cast<double>(srcRect.y0)},
         {static_cast<double>(srcRect.x1), static_cast<double>(srcRect.y1)},
         {static_cast<double>(srcRect.x0), static_cast<double>(srcRect.y1)}}};
    for (const auto& c : cs) {
        const auto p = full.apply(c[0], c[1]);
        x0 = std::min(x0, p[0]);
        y0 = std::min(y0, p[1]);
        x1 = std::max(x1, p[0]);
        y1 = std::max(y1, p[1]);
    }
    if (!(x1 > x0 && y1 > y0)) {
        return std::nullopt;
    }
    PasteResult res;
    res.originX = std::floor(x0);
    res.originY = std::floor(y0);
    res.img.w = static_cast<int>(std::ceil(x1)) - static_cast<int>(res.originX);
    res.img.h = static_cast<int>(std::ceil(y1)) - static_cast<int>(res.originY);
    res.img.px.assign(static_cast<size_t>(res.img.w) * res.img.h * 4u, 0.0f);
    perspective::Homography inv;
    if (!full.invert(inv)) {
        return std::nullopt;
    }
    // 预乘源
    Img pre = src;
    for (size_t k = 0; k < pre.px.size(); k += 4u) {
        const float a = pre.px[k + 3u];
        pre.px[k] *= a;
        pre.px[k + 1u] *= a;
        pre.px[k + 2u] *= a;
    }
    (void)pre;
    for (int y = 0; y < res.img.h; ++y) {
        for (int x = 0; x < res.img.w; ++x) {
            const double dx = res.originX + x + 0.5;
            const double dy = res.originY + y + 0.5;
            const auto uv = inv.apply(dx, dy);
            const double sx = uv[0];
            const double sy = uv[1];
            float* o = &res.img.px[(static_cast<size_t>(y) * res.img.w + x) * 4u];
            if (sx < -1.0 || sy < -1.0 || sx > src.w + 1.0 || sy > src.h + 1.0) {
                continue;
            }
            float rgba[4];
            for (int c = 0; c < 4; ++c) {
                rgba[c] = bicubicAt(src, sx - 0.5 + 0.5, sy - 0.5 + 0.5, c);
            }
            const float a = std::clamp(rgba[3], 0.0f, 1.0f);
            o[3] = a;
            for (int c = 0; c < 3; ++c) {
                o[c] = a > 1e-6f ? std::clamp(rgba[c] / a, 0.0f, 1.0f) : 0.0f;
            }
        }
    }
    // 裁剪到平面
    const std::vector<float> mask = quadMask(h, RectI{static_cast<int>(res.originX),
                                                      static_cast<int>(res.originY),
                                                      static_cast<int>(res.originX) + res.img.w,
                                                      static_cast<int>(res.originY) + res.img.h});
    for (size_t k = 0; k < mask.size(); ++k) {
        res.img.px[k * 4u + 3u] *= mask[k];
    }
    return res;
}

int cloneStroke(const Scene& sc, Img& surf, const std::array<double, 2>& source,
                const std::vector<std::array<double, 2>>& points, double size, double hardness,
                float opacity) {
    if (points.empty()) {
        return 0;
    }
    const auto psOpt = sc.planeAt(source);
    const auto pdOpt = sc.planeAt(points[0]);
    if (!psOpt || !pdOpt) {
        return 0;
    }
    perspective::Homography hs, hd;
    double lus = 0, lvs = 0, lud = 0, lvd = 0;
    if (!sc.frame(*psOpt, hs, lus, lvs) || !sc.frame(*pdOpt, hd, lud, lvd)) {
        return 0;
    }
    perspective::Homography hsInv, hdInv;
    if (!hs.invert(hsInv) || !hd.invert(hdInv)) {
        return 0;
    }
    auto metric = [](const perspective::Homography& inv, double lu, double lv,
                     const std::array<double, 2>& p) {
        const auto uv = inv.apply(p[0], p[1]);
        return std::array<double, 2>{uv[0] * lu, uv[1] * lv};
    };
    const auto ms = metric(hsInv, lus, lvs, source);
    const auto md0 = metric(hdInv, lud, lvd, points[0]);
    const double offset[2] = {ms[0] - md0[0], ms[1] - md0[1]};
    const auto probe = metric(hdInv, lud, lvd, {points[0][0] + size / 2.0, points[0][1]});
    const double rM = std::max(std::hypot(probe[0] - md0[0], probe[1] - md0[1]), 1e-9);
    const Img srcCopy = surf;
    const double hard = std::clamp(hardness, 0.0, 0.99);
    int dabs = 0;
    for (const auto& p : points) {
        const auto mc = metric(hdInv, lud, lvd, p);
        double bb[4] = {1e308, 1e308, -1e308, -1e308};
        for (int k = 0; k < 16; ++k) {
            const double a = k / 16.0 * 2.0 * kPi;
            const double u = (mc[0] + rM * std::cos(a)) / lud;
            const double v = (mc[1] + rM * std::sin(a)) / lvd;
            const auto xy = hd.apply(u, v);
            bb[0] = std::min(bb[0], xy[0]);
            bb[1] = std::min(bb[1], xy[1]);
            bb[2] = std::max(bb[2], xy[0]);
            bb[3] = std::max(bb[3], xy[1]);
        }
        RectI area;
        area.x0 = static_cast<int>(std::floor(bb[0]));
        area.y0 = static_cast<int>(std::floor(bb[1]));
        area.x1 = static_cast<int>(std::ceil(bb[2])) + 1;
        area.y1 = static_cast<int>(std::ceil(bb[3])) + 1;
        if (area.width() <= 0 || area.height() <= 0 || area.width() > 4096 || area.height() > 4096) {
            continue;
        }
        const int x0 = std::max(0, area.x0);
        const int y0 = std::max(0, area.y0);
        const int x1 = std::min(surf.w, area.x1);
        const int y1 = std::min(surf.h, area.y1);
        bool changed = false;
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const auto m = metric(hdInv, lud, lvd, {x + 0.5, y + 0.5});
                const double d = std::hypot(m[0] - mc[0], m[1] - mc[1]) / rM;
                if (d >= 1.0) {
                    continue;
                }
                const double fall = d <= hard ? 1.0 : 1.0 - (d - hard) / (1.0 - hard);
                const float k = static_cast<float>(fall * fall * (3.0 - 2.0 * fall)) * opacity;
                const double sm[2] = {m[0] + offset[0], m[1] + offset[1]};
                const auto sxy = hs.apply(sm[0] / lus, sm[1] / lvs);
                float* px = &surf.px[(static_cast<size_t>(y) * surf.w + x) * 4u];
                for (int c = 0; c < 4; ++c) {
                    const float s = bicubicAt(srcCopy, sxy[0] - 0.5, sxy[1] - 0.5, c);
                    px[c] += (s - px[c]) * k;
                }
                changed = true;
            }
        }
        if (changed) {
            ++dabs;
        }
    }
    return dabs;
}

}  // namespace vanishing

}  // namespace montage
