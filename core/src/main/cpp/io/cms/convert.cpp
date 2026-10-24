#include "io/cms/convert.h"

#include <cmath>
#include <mutex>
#include <vector>

#include "io/cms/builtin.h"
#include "io/cms/transform.h"

namespace montage {
namespace cms {

namespace {
// 按内容哈希缓存已构建空间（同文档重复查询廉价）。
std::mutex& cacheMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::pair<uint64_t, std::shared_ptr<const CmykSpace>>>& cacheStore() {
    static std::vector<std::pair<uint64_t, std::shared_ptr<const CmykSpace>>> c;
    return c;
}

// 内置 profile 的变换（内置必可链接；空只防 cms 回归——此时退回朴素公式）。
const Transform* cmykToSrgbTransform() {
    static const std::shared_ptr<Transform> t = [] {
        std::string err;
        auto r = Transform::make(builtinProfile(Builtin::CoatedCmyk), builtinProfile(Builtin::Srgb),
                                 Intent::RelativeColorimetric, true, err);
        return r ? std::make_shared<Transform>(std::move(*r)) : nullptr;
    }();
    return t.get();
}

const Transform* srgbToCmykTransform() {
    static const std::shared_ptr<Transform> t = [] {
        std::string err;
        auto r = Transform::make(builtinProfile(Builtin::Srgb), builtinProfile(Builtin::CoatedCmyk),
                                 Intent::RelativeColorimetric, true, err);
        return r ? std::make_shared<Transform>(std::move(*r)) : nullptr;
    }();
    return t.get();
}

thread_local std::shared_ptr<const CmykSpace> tActiveCmyk;
}  // namespace

std::shared_ptr<const CmykSpace> activeCmykSpace() {
    return tActiveCmyk;
}

CmykScope::CmykScope(const std::shared_ptr<const CmykSpace>& space) : prev_(tActiveCmyk) {
    tActiveCmyk = space;
}

CmykScope::~CmykScope() {
    tActiveCmyk = std::move(prev_);
}

std::shared_ptr<const CmykSpace> CmykSpace::forProfile(const std::vector<uint8_t>* icc) {
    if (icc == nullptr) {
        return nullptr;
    }
    // 内容哈希做键（文档可能各自持有字节副本）。
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : *icc) {
        h ^= b;
        h *= 1099511628211ull;
    }
    {
        std::lock_guard<std::mutex> lk(cacheMutex());
        for (const auto& e : cacheStore()) {
            if (e.first == h) {
                return e.second;
            }
        }
    }
    std::shared_ptr<const CmykSpace> space;
    {
        std::string err;
        auto p = Profile::parse(*icc, err);
        if (p && p->colorSpace == ColorSpace::Cmyk) {
            const Profile& coated = builtinProfile(Builtin::CoatedCmyk);
            if (p->contentHash() != coated.contentHash()) {
                const Profile& srgb = builtinProfile(Builtin::Srgb);
                auto to = Transform::make(*p, srgb, Intent::RelativeColorimetric, true, err);
                auto from = Transform::make(srgb, *p, Intent::RelativeColorimetric, true, err);
                if (to && from) {
                    auto s = std::make_shared<CmykSpace>();
                    s->toSrgb_ = std::make_shared<Transform>(std::move(*to));
                    s->fromSrgb_ = std::make_shared<Transform>(std::move(*from));
                    s->id = p->contentHash();
                    space = std::move(s);
                }
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(cacheMutex());
        auto& store = cacheStore();
        for (const auto& e : store) {
            if (e.first == h) {
                return e.second;  // 竞态下已有
            }
        }
        store.emplace_back(h, space);
        if (store.size() > 16) {
            store.clear();
        }
    }
    return space;
}

std::array<float, 3> CmykSpace::toSrgb(const std::array<float, 4>& c) const {
    std::array<float, 3> o = {0.0f, 0.0f, 0.0f};
    if (toSrgb_ != nullptr) {
        toSrgb_->evalFast(c.data(), o.data());
    }
    return o;
}

std::array<float, 4> CmykSpace::fromSrgb(const std::array<float, 3>& rgb) const {
    std::array<float, 4> o = {0.0f, 0.0f, 0.0f, 0.0f};
    if (fromSrgb_ != nullptr) {
        fromSrgb_->evalFast(rgb.data(), o.data());
    }
    return o;
}

std::array<float, 3> cmykToRgb(const std::array<float, 4>& c) {
    std::array<float, 3> o = {0.0f, 0.0f, 0.0f};
    if (tActiveCmyk != nullptr) {
        return tActiveCmyk->toSrgb(c);
    }
    const Transform* t = cmykToSrgbTransform();
    if (t == nullptr) {
        return cmykToRgbNaive(c);
    }
    t->evalFast(c.data(), o.data());
    return o;
}

std::array<float, 4> rgbToCmyk(const std::array<float, 3>& rgb) {
    const std::array<float, 3> v = {std::min(std::max(rgb[0], 0.0f), 1.0f),
                                    std::min(std::max(rgb[1], 0.0f), 1.0f),
                                    std::min(std::max(rgb[2], 0.0f), 1.0f)};
    if (tActiveCmyk != nullptr) {
        return tActiveCmyk->fromSrgb(v);
    }
    const Transform* t = srgbToCmykTransform();
    if (t == nullptr) {
        return rgbToCmykNaive(rgb);
    }
    std::array<float, 4> o = {0.0f, 0.0f, 0.0f, 0.0f};
    t->evalFast(v.data(), o.data());
    return o;
}

std::array<float, 3> cmykToRgbNaive(const std::array<float, 4>& c) {
    const float k = 1.0f - c[3];
    return {(1.0f - c[0]) * k, (1.0f - c[1]) * k, (1.0f - c[2]) * k};
}

std::array<float, 4> rgbToCmykNaive(const std::array<float, 3>& rgb) {
    const float k = 1.0f - std::max(std::max(rgb[0], rgb[1]), rgb[2]);
    if (k >= 1.0f - 1e-6f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float d = 1.0f - k;
    return {(1.0f - rgb[0] - k) / d, (1.0f - rgb[1] - k) / d, (1.0f - rgb[2] - k) / d, k};
}

std::array<float, 3> labToSrgb(const std::array<float, 3>& lab) {
    const float l = lab[0], a = lab[1], b = lab[2];
    const float fy = (l + 16.0f) / 116.0f;
    const float fx = fy + a / 500.0f;
    const float fz = fy - b / 200.0f;
    auto finv = [](float t) {
        return t > 6.0f / 29.0f ? t * t * t : 3.0f * (6.0f / 29.0f) * (6.0f / 29.0f) * (t - 4.0f / 29.0f);
    };
    const float xyz[3] = {finv(fx) * kD50F32[0], finv(fy) * kD50F32[1], finv(fz) * kD50F32[2]};
    std::array<float, 3> out{};
    for (int r = 0; r < 3; ++r) {
        const float lin = kXyzD50ToSrgb[r][0] * xyz[0] + kXyzD50ToSrgb[r][1] * xyz[1] +
                          kXyzD50ToSrgb[r][2] * xyz[2];
        out[r] = linearToSrgb(std::min(std::max(lin, 0.0f), 1.0f));
    }
    return out;
}

std::array<float, 3> srgbToLab(const std::array<float, 3>& rgb) {
    const float lin[3] = {srgbToLinear(rgb[0]), srgbToLinear(rgb[1]), srgbToLinear(rgb[2])};
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    for (int r = 0; r < 3; ++r) {
        xyz[r] = kSrgbToXyzD50[r][0] * lin[0] + kSrgbToXyzD50[r][1] * lin[1] + kSrgbToXyzD50[r][2] * lin[2];
    }
    constexpr float kE = 6.0f / 29.0f * (6.0f / 29.0f) * (6.0f / 29.0f);
    auto f = [](float t) {
        return t > kE ? std::cbrt(t) : t / (3.0f * (6.0f / 29.0f) * (6.0f / 29.0f)) + 4.0f / 29.0f;
    };
    const float fx = f(xyz[0] / kD50F32[0]);
    const float fy = f(xyz[1] / kD50F32[1]);
    const float fz = f(xyz[2] / kD50F32[2]);
    return {116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz)};
}

}  // namespace cms
}  // namespace montage
