// 内容感知填充实现（M19b）。photocraft algo/content_aware.rs 逐式翻译（MIT OR Apache-2.0）。

#include "content_aware_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "poisson_ops.h"

namespace montage {
namespace content_aware {
namespace {

// 双线性采样（photocraft sample）：任一 tap 不可用 = false。
bool sampleBilinear(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                    const std::vector<bool>& ok, float x, float y, float* out) {
    if (x < 0.0f || y < 0.0f || x > static_cast<float>(w - 1) || y > static_cast<float>(h - 1)) {
        return false;
    }
    const size_t x0 = static_cast<size_t>(std::floor(x)), y0 = static_cast<size_t>(std::floor(y));
    const size_t x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
    const size_t taps[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    for (const auto& t : taps) {
        if (!ok[t[1] * w + t[0]]) {
            return false;
        }
    }
    auto p = [&](size_t xx, size_t yy, size_t c) { return img[(yy * w + xx) * ch + c]; };
    for (size_t c = 0; c < ch; ++c) {
        const float top = p(x0, y0, c) + (p(x1, y0, c) - p(x0, y0, c)) * tx;
        const float bot = p(x0, y1, c) + (p(x1, y1, c) - p(x0, y1, c)) * tx;
        out[c] = top + (bot - top) * ty;
    }
    return true;
}

}  // namespace

FillOptions FillOptions::makeDefault() {
    FillOptions o;
    o.colorAdaptation = colorLevel("default");
    return o;
}

std::vector<float> rotationLevel(const char* level) {
    const std::string l = level ? level : "";
    if (l == "low") {
        return {-10.0f, 10.0f};
    }
    if (l == "medium") {
        return {-20.0f, 20.0f};
    }
    if (l == "high") {
        return {-35.0f, 35.0f};
    }
    if (l == "full") {
        return {90.0f, 180.0f, 270.0f};
    }
    return {};
}

inpaint::Adapt colorLevel(const char* level) {
    const std::string l = level ? level : "";
    if (l == "none") {
        return inpaint::Adapt::none();
    }
    if (l == "high") {
        return inpaint::Adapt{0.8f, 1.25f, 0.08f};
    }
    if (l == "veryHigh") {
        return inpaint::Adapt{0.67f, 1.5f, 0.14f};
    }
    return inpaint::Adapt{0.9f, 1.1f, 0.04f};
}

inpaint::Adapt colorAmount(uint8_t color) {
    if (color == 0) {
        return inpaint::Adapt::none();
    }
    const float c = static_cast<float>(std::min<uint8_t>(color, 10));
    return inpaint::Adapt{1.0f - 0.033f * c, 1.0f + 0.05f * c, 0.04f * c};
}

std::vector<std::pair<float, float>> levelTransform(size_t ch, const std::vector<float>& from,
                                                    const std::vector<float>& to,
                                                    inpaint::Adapt a) {
    auto moments = [&](const std::vector<float>& v, size_t c) {
        double s = 0.0, s2 = 0.0, n = 0.0;
        const size_t stride = std::max<size_t>(ch, 1);
        for (size_t i = 0; i + stride <= v.size(); i += stride) {
            const double x = v[i + c];
            s += x;
            s2 += x * x;
            n += 1.0;
        }
        return std::array<double, 3>{s, s2, n};
    };
    std::vector<std::pair<float, float>> out;
    for (size_t c = 0; c < ch; ++c) {
        const auto [fs, fs2, fn] = moments(from, c);
        const auto [ts, ts2, tn] = moments(to, c);
        if (a.isNone() || fn == 0.0 || tn == 0.0) {
            out.emplace_back(1.0f, 0.0f);
            continue;
        }
        // 两组样本按同数缩放后拟合（photocraft fit 同式，限幅内）。
        const double k = fn / tn;
        const float nf = static_cast<float>(fn);
        const float mt = static_cast<float>(ts * k) / nf, ms = static_cast<float>(fs) / nf;
        const float vt = std::max(static_cast<float>(ts2 * k) / nf - mt * mt, 0.0f);
        const float vs = std::max(static_cast<float>(fs2) / nf - ms * ms, 0.0f);
        float g = vs > 1e-10f ? std::sqrt(vt / vs) : 1.0f;
        g = std::clamp(g, a.gainLo, a.gainHi);
        out.emplace_back(g, std::clamp(mt - g * ms, -a.bias, a.bias));
    }
    return out;
}

Variant variantOf(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                  const std::vector<bool>& srcOk, float deg, float scale, bool mirror) {
    const float rad = -deg * 3.14159265358979f / 180.0f;
    const float s = std::sin(rad), c = std::cos(rad);
    const float fw = static_cast<float>(w) * scale, fh = static_cast<float>(h) * scale;
    const size_t ow = std::max<size_t>(static_cast<size_t>(std::llround(fw * std::abs(c) + fh * std::abs(s))), 1);
    const size_t oh = std::max<size_t>(static_cast<size_t>(std::llround(fw * std::abs(s) + fh * std::abs(c))), 1);
    const float icx = (static_cast<float>(w) - 1.0f) / 2.0f, icy = (static_cast<float>(h) - 1.0f) / 2.0f;
    const float ocx = (static_cast<float>(ow) - 1.0f) / 2.0f, ocy = (static_cast<float>(oh) - 1.0f) / 2.0f;
    Variant v;
    v.w = ow;
    v.h = oh;
    v.img.assign(ow * oh * ch, 0.0f);
    v.ok.assign(ow * oh, false);
    std::vector<float> px(ch);
    for (size_t y = 0; y < oh; ++y) {
        for (size_t x = 0; x < ow; ++x) {
            const float dx = (static_cast<float>(x) - ocx) / scale;
            const float dy = (static_cast<float>(y) - ocy) / scale;
            float sx = icx + dx * c - dy * s;
            float sy = icy + dx * s + dy * c;
            if (mirror) {
                sx = (static_cast<float>(w) - 1.0f) - sx;
            }
            if (sampleBilinear(w, h, ch, img, srcOk, sx, sy, px.data())) {
                v.ok[y * ow + x] = true;
                std::memcpy(&v.img[(y * ow + x) * ch], px.data(), ch * sizeof(float));
            }
        }
    }
    return v;
}

std::vector<float> fill(size_t w, size_t h, size_t ch, const std::vector<float>& img,
                        const std::vector<bool>& hole, const std::vector<bool>& source,
                        const FillOptions& opts, const CancelFn& cancel) {
    if (img.size() != w * h * ch || hole.size() != w * h || source.size() != w * h) {
        return {};
    }
    bool anyHole = false;
    for (bool b : hole) {
        anyHole = anyHole || b;
    }
    if (!anyHole) {
        return img;
    }
    auto stopped = [] { return std::vector<float>(); };
    inpaint::CompleteParams params;
    params.seed = opts.seed;
    params.adapt = opts.colorAdaptation;
    const size_t gap = 2 * static_cast<size_t>(std::max(params.patchRadius, 1)) + 2;
    std::vector<bool> ok(w * h);
    for (size_t i = 0; i < w * h; ++i) {
        ok[i] = !hole[i] && source[i];
    }
    auto role = [&](size_t i) {
        return hole[i] ? inpaint::Px::Hole : ok[i] ? inpaint::Px::Source : inpaint::Px::Known;
    };
    // 变体：恒等在最左，适配变体列在其右（镜像 → 旋转 → 缩放；photocraft 同序）。
    std::vector<Variant> extra;
    if (opts.mirror) {
        extra.push_back(variantOf(w, h, ch, img, ok, 0.0f, 1.0f, true));
        if (cancel && cancel()) {
            return stopped();
        }
    }
    for (float deg : opts.rotations) {
        extra.push_back(variantOf(w, h, ch, img, ok, deg, 1.0f, false));
        if (cancel && cancel()) {
            return stopped();
        }
    }
    if (opts.scale) {
        extra.push_back(variantOf(w, h, ch, img, ok, 0.0f, 0.8f, false));
        if (cancel && cancel()) {
            return stopped();
        }
        extra.push_back(variantOf(w, h, ch, img, ok, 0.0f, 1.25f, false));
        if (cancel && cancel()) {
            return stopped();
        }
    }
    if (extra.empty()) {
        // 无变体：本体直接补全（只动孔）；无源可拷回退膜填充。
        std::vector<inpaint::Px> roles(w * h);
        for (size_t i = 0; i < w * h; ++i) {
            roles[i] = role(i);
        }
        inpaint::CompleteResult r = inpaint::completeMasked(w, h, ch, img, roles, params, cancel);
        if (r.status == inpaint::CompleteStatus::Ok) {
            return r.img;
        }
        if (cancel && cancel()) {
            return stopped();
        }
        return poisson::membraneFillWith(w, h, ch, img, hole, cancel);
    }
    // 马赛克拼装：本体在左，变体列在右，gap = Void 隔离。
    size_t colW = 0, colH = 0;
    for (const auto& v : extra) {
        colW = std::max(colW, v.w);
        colH += v.h + gap;
    }
    const size_t mw = w + gap + colW;
    const size_t mh = std::max(h, colH);
    std::vector<float> mimg(mw * mh * ch, 0.0f);
    std::vector<inpaint::Px> roles(mw * mh, inpaint::Px::Void);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            const size_t si = y * w + x, di = y * mw + x;
            std::memcpy(&mimg[di * ch], &img[si * ch], ch * sizeof(float));
            roles[di] = role(si);
        }
    }
    size_t oy = 0;
    for (const auto& v : extra) {
        for (size_t y = 0; y < v.h; ++y) {
            for (size_t x = 0; x < v.w; ++x) {
                const size_t si = y * v.w + x;
                const size_t di = (oy + y) * mw + w + gap + x;
                std::memcpy(&mimg[di * ch], &v.img[si * ch], ch * sizeof(float));
                if (v.ok[si]) {
                    roles[di] = inpaint::Px::Source;
                }
            }
        }
        oy += v.h + gap;
        if (cancel && cancel()) {
            return stopped();
        }
    }
    inpaint::CompleteResult r = inpaint::completeMasked(mw, mh, ch, mimg, roles, params, cancel);
    if (r.status != inpaint::CompleteStatus::Ok) {
        if (cancel && cancel()) {
            return stopped();
        }
        return poisson::membraneFillWith(w, h, ch, img, hole, cancel);
    }
    // 只取回本体的孔。
    std::vector<float> out(img);
    for (size_t y = 0; y < h; ++y) {
        for (size_t x = 0; x < w; ++x) {
            if (hole[y * w + x]) {
                const size_t si = (y * mw + x) * ch, di = (y * w + x) * ch;
                std::memcpy(&out[di], &r.img[si], ch * sizeof(float));
            }
        }
    }
    return out;
}

}  // namespace content_aware
}  // namespace montage
