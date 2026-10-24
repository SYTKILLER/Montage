#include "io/cms/pipeline.h"

#include <algorithm>
#include <cmath>

namespace montage {
namespace cms {

namespace {

// u1Fixed15 XYZ 缩放与 v2 Lab 编码常数（对拍 pipeline.rs）。
constexpr float kXyzScale = 65535.0f / 32768.0f;
constexpr float kV2L = 100.0f * 65535.0f / 65280.0f;
constexpr float kV2Ab = 255.0f * 65535.0f / 65280.0f;

inline float clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

}  // namespace

size_t Stage::run(float* v, size_t n) const {
    switch (kind) {
        case Kind::Curves:
            for (size_t i = 0; i < n && i < curves.size(); ++i) {
                v[i] = curves[i].eval(v[i]);
            }
            return n;
        case Kind::InvCurves:
            for (size_t i = 0; i < n && i < curves.size(); ++i) {
                v[i] = curves[i].evalInverse(v[i]);
            }
            return n;
        case Kind::Matrix: {
            float o[kPipelineMax] = {0};
            for (size_t r = 0; r < rows; ++r) {
                double s = r < offset.size() ? offset[r] : 0.0;
                for (size_t c = 0; c < cols; ++c) {
                    s += m[r * cols + c] * static_cast<double>(v[c]);
                }
                o[r] = static_cast<float>(s);
            }
            for (size_t r = 0; r < rows; ++r) {
                v[r] = o[r];
            }
            return rows;
        }
        case Kind::Clut: {
            float in[kPipelineMax] = {0};
            float o[kPipelineMax] = {0};
            for (size_t i = 0; i < n; ++i) {
                in[i] = v[i];
            }
            clut.eval(in, o);
            for (size_t i = 0; i < clut.outputs; ++i) {
                v[i] = o[i];
            }
            return clut.outputs;
        }
        case Kind::LabToXyz: {
            const auto x = labToXyzF32({v[0], v[1], v[2]});
            v[0] = x[0];
            v[1] = x[1];
            v[2] = x[2];
            return 3;
        }
        case Kind::XyzToLab: {
            const auto l = xyzToLabF32({std::max(v[0], 0.0f), std::max(v[1], 0.0f), std::max(v[2], 0.0f)});
            v[0] = l[0];
            v[1] = l[1];
            v[2] = l[2];
            return 3;
        }
        case Kind::DecodeLabV4:
            v[0] *= 100.0f;
            v[1] = v[1] * 255.0f - 128.0f;
            v[2] = v[2] * 255.0f - 128.0f;
            return 3;
        case Kind::EncodeLabV4:
            v[0] = clamp01(v[0] / 100.0f);
            v[1] = clamp01((v[1] + 128.0f) / 255.0f);
            v[2] = clamp01((v[2] + 128.0f) / 255.0f);
            return 3;
        case Kind::DecodeLabV2:
            v[0] *= kV2L;
            v[1] = v[1] * kV2Ab - 128.0f;
            v[2] = v[2] * kV2Ab - 128.0f;
            return 3;
        case Kind::EncodeLabV2:
            v[0] = clamp01(v[0] / kV2L);
            v[1] = clamp01((v[1] + 128.0f) / kV2Ab);
            v[2] = clamp01((v[2] + 128.0f) / kV2Ab);
            return 3;
        case Kind::DecodeXyz:
            for (int i = 0; i < 3; ++i) {
                v[i] *= kXyzScale;
            }
            return 3;
        case Kind::EncodeXyz:
            for (int i = 0; i < 3; ++i) {
                v[i] = clamp01(v[i] / kXyzScale);
            }
            return 3;
        case Kind::Clamp01:
            for (size_t i = 0; i < n; ++i) {
                v[i] = clamp01(v[i]);
            }
            return n;
    }
    return n;
}

bool Stage::operator==(const Stage& o) const {
    if (kind != o.kind) {
        return false;
    }
    switch (kind) {
        case Kind::Curves:
        case Kind::InvCurves:
            return curves == o.curves;
        case Kind::Matrix:
            return rows == o.rows && cols == o.cols && m == o.m && offset == o.offset;
        case Kind::Clut:
            return clut == o.clut;
        default:
            return true;
    }
}

Pipeline::Pipeline(size_t inputs_, std::vector<Stage> stages_) : inputs(inputs_), stages(std::move(stages_)) {
    outputs = inputs;
    for (const Stage& s : stages) {
        outputs = s.outputs(outputs);
    }
}

void Pipeline::eval(const float* input, float* out) const {
    float v[kPipelineMax] = {0};
    for (size_t i = 0; i < inputs; ++i) {
        v[i] = input[i];
    }
    size_t n = inputs;
    for (const Stage& s : stages) {
        n = s.run(v, n);
    }
    for (size_t i = 0; i < outputs; ++i) {
        out[i] = v[i];
    }
}

void Pipeline::optimize() {
    std::vector<Stage> out;
    out.reserve(stages.size());
    for (Stage& s : stages) {
        bool noop = false;
        if (s.kind == Stage::Kind::Curves || s.kind == Stage::Kind::InvCurves) {
            noop = true;
            for (const Curve& c : s.curves) {
                if (!c.isIdentity()) {
                    noop = false;
                    break;
                }
            }
        } else if (s.kind == Stage::Kind::Matrix && s.rows == 3 && s.cols == 3) {
            noop = true;
            for (double o : s.offset) {
                if (std::abs(o) >= 1e-12) {
                    noop = false;
                    break;
                }
            }
            if (noop) {
                for (size_t i = 0; i < s.m.size(); ++i) {
                    const double want = (i % 4 == 0) ? 1.0 : 0.0;
                    if (std::abs(s.m[i] - want) >= 1e-12) {
                        noop = false;
                        break;
                    }
                }
            }
        }
        if (noop) {
            continue;
        }
        // 相邻逆对抵消（Lab↔XYZ、encode/decode 同族）。
        const Stage* last = out.empty() ? nullptr : &out.back();
        auto cancels = [&]() {
            if (last == nullptr) {
                return false;
            }
            const auto a = last->kind, b = s.kind;
            if ((a == Stage::Kind::LabToXyz && b == Stage::Kind::XyzToLab) ||
                (a == Stage::Kind::XyzToLab && b == Stage::Kind::LabToXyz) ||
                (a == Stage::Kind::EncodeLabV4 && b == Stage::Kind::DecodeLabV4) ||
                (a == Stage::Kind::EncodeLabV2 && b == Stage::Kind::DecodeLabV2) ||
                (a == Stage::Kind::EncodeXyz && b == Stage::Kind::DecodeXyz)) {
                return true;
            }
            // 同编码 decode→encode 只在编码值域内精确——表输出必然在值域内，同抵。
            if ((a == Stage::Kind::DecodeLabV4 && b == Stage::Kind::EncodeLabV4) ||
                (a == Stage::Kind::DecodeLabV2 && b == Stage::Kind::EncodeLabV2) ||
                (a == Stage::Kind::DecodeXyz && b == Stage::Kind::EncodeXyz)) {
                return true;
            }
            return false;
        };
        if (cancels()) {
            out.pop_back();
            continue;
        }
        // 合并相邻 3×3 矩阵。
        if (last != nullptr && last->kind == Stage::Kind::Matrix && last->rows == 3 && last->cols == 3 &&
            s.kind == Stage::Kind::Matrix && s.rows == 3 && s.cols == 3) {
            const std::vector<double>& a = last->m;
            const std::vector<double>& oa = last->offset;
            std::vector<double> m(9, 0.0);
            std::vector<double> off(3, 0.0);
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    double sum = 0.0;
                    for (int k = 0; k < 3; ++k) {
                        sum += s.m[r * 3 + k] * a[k * 3 + c];
                    }
                    m[r * 3 + c] = sum;
                }
                double sum = 0.0;
                for (int k = 0; k < 3; ++k) {
                    sum += s.m[r * 3 + k] * oa[k];
                }
                off[r] = sum + s.offset[r];
            }
            out.pop_back();
            out.push_back(Stage::matrixStage(3, 3, std::move(m), std::move(off)));
            continue;
        }
        out.push_back(std::move(s));
    }
    stages = std::move(out);
}

}  // namespace cms
}  // namespace montage
