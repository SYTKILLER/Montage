#include "io/cms/gamut.h"

#include <cmath>

#include "io/cms/builtin.h"
#include "io/cms/transform.h"

namespace montage {
namespace cms {

std::optional<GamutCheck> GamutCheck::make(const Profile& src, const Profile& proof, float threshold,
                                           std::string& err) {
    const Profile& lab = builtinProfile(Builtin::LabD50);
    auto directStages = linkStages(src, lab, Intent::RelativeColorimetric, false, err);
    if (!directStages) {
        return std::nullopt;
    }
    auto viaStages = linkStages(src, proof, Intent::RelativeColorimetric, false, err);
    if (!viaStages) {
        return std::nullopt;
    }
    Stage clamp;
    clamp.kind = Stage::Kind::Clamp01;
    viaStages->push_back(clamp);
    auto back = linkStages(proof, lab, Intent::RelativeColorimetric, false, err);
    if (!back) {
        return std::nullopt;
    }
    viaStages->insert(viaStages->end(), back->begin(), back->end());
    GamutCheck g;
    g.inputs_ = src.channels();
    g.threshold_ = threshold;
    g.direct_ = Pipeline(src.channels(), std::move(*directStages));
    g.via_ = Pipeline(src.channels(), std::move(*viaStages));
    auto de = [&](const float* p, float* o) {
        float a[kPipelineMax] = {0}, b[kPipelineMax] = {0};
        g.direct_.eval(p, a);
        g.via_.eval(p, b);
        const double d0 = (a[0] - b[0]) * 100.0f;
        const double d1 = (a[1] - b[1]) * 255.0f;
        const double d2 = (a[2] - b[2]) * 255.0f;
        o[0] = static_cast<float>(std::sqrt(d0 * d0 + d1 * d1 + d2 * d2));
    };
    const size_t n = src.channels();
    if (n == 1) {
        g.grid_ = Clut::sample(std::vector<size_t>{256}, 1, de);
    } else if (n == 3) {
        g.grid_ = Clut::sample(std::vector<size_t>{33, 33, 33}, 1, de);
    } else if (n == 4) {
        g.grid_ = Clut::sample(std::vector<size_t>{17, 17, 17, 17}, 1, de);
    }
    return g;
}

float GamutCheck::deltaE(const float* px) const {
    float o[1] = {0.0f};
    if (grid_.has_value()) {
        float c[kPipelineMax] = {0};
        for (size_t d = 0; d < inputs_; ++d) {
            c[d] = std::min(std::max(px[d], 0.0f), 1.0f);
        }
        grid_->eval(c, o);
    } else {
        float a[kPipelineMax] = {0}, b[kPipelineMax] = {0};
        direct_.eval(px, a);
        via_.eval(px, b);
        o[0] = std::hypot((a[0] - b[0]) * 100.0f, std::hypot((a[1] - b[1]) * 255.0f, (a[2] - b[2]) * 255.0f));
    }
    return o[0];
}

}  // namespace cms
}  // namespace montage
