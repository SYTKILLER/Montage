#include "io/cms/transform.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace cms {

namespace {

// ICC v4 感知参考介质黑（ICC.1:2010 §6.3.4.4），XYZ。
constexpr double kPerceptualBlack[3] = {0.00336, 0.0034731, 0.00287};

// 设备空间白（归一化设备值）。
std::optional<std::vector<float>> deviceWhite(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Rgb: return std::vector<float>{1.0f, 1.0f, 1.0f};
        case ColorSpace::Gray: return std::vector<float>{1.0f};
        case ColorSpace::Cmyk: return std::vector<float>{0.0f, 0.0f, 0.0f, 0.0f};
        case ColorSpace::Cmy: return std::vector<float>{0.0f, 0.0f, 0.0f};
        case ColorSpace::Lab: return std::vector<float>{1.0f, 0.5f, 0.5f};
        default: return std::nullopt;
    }
}

inline float clamp01(float v) { return std::min(std::max(v, 0.0f), 1.0f); }

// [0,1] 上 TABLE_1D 段采样的表求值；域外从端段线性外推（HDR float 快路径可用）。
inline float tableLookup(const std::vector<float>& t, float x) {
    const size_t n = t.size() - 1;
    const float p = x * static_cast<float>(n);
    size_t i = p < 0.0f ? 0 : static_cast<size_t>(p);
    if (i > n - 1) {
        i = n - 1;
    }
    const float f = p - static_cast<float>(i);
    return t[i] + (t[i + 1] - t[i]) * f;
}

void applyCoreMatrix(const TransformCore& core, const float* v, float* out, size_t n) {
    if (!core.hasMatrix) {
        for (size_t i = 0; i < n; ++i) {
            out[i] = v[i];
        }
        return;
    }
    for (size_t r = 0; r < core.matRows; ++r) {
        float s = core.matOffset[r];
        for (size_t c = 0; c < core.matCols; ++c) {
            s += core.matM[r * core.matCols + c] * v[c];
        }
        out[r] = s;
    }
}

TransformCore shaperCore(const std::vector<Curve>& cs, bool hasMatrix, size_t rows, size_t cols,
                         const std::vector<double>* m, const std::vector<double>* offset) {
    TransformCore core;
    core.kind = TransformCore::Kind::Shaper;
    core.curves8.reserve(cs.size());
    core.curves.reserve(cs.size());
    for (const Curve& c : cs) {
        std::array<float, 256> t8{};
        for (int i = 0; i < 256; ++i) {
            t8[i] = static_cast<float>(c.eval64(static_cast<double>(i) / 255.0));
        }
        core.curves8.push_back(t8);
        std::vector<float> t(kTable1D + 1);
        for (size_t i = 0; i <= kTable1D; ++i) {
            t[i] = static_cast<float>(c.eval64(static_cast<double>(i) / static_cast<double>(kTable1D)));
        }
        core.curves.push_back(std::move(t));
    }
    core.hasMatrix = hasMatrix;
    core.matRows = rows;
    core.matCols = cols;
    if (m != nullptr) {
        core.matM.reserve(m->size());
        for (double v : *m) {
            core.matM.push_back(static_cast<float>(v));
        }
    }
    if (offset != nullptr) {
        core.matOffset.reserve(offset->size());
        for (double v : *offset) {
            core.matOffset.push_back(static_cast<float>(v));
        }
    }
    return core;
}

}  // namespace

Transform Transform::fromPipeline(Pipeline pipeline, TransformOptions opts) {
    pipeline.optimize();
    Transform t;
    t.inputs_ = pipeline.inputs;
    t.outputs_ = pipeline.outputs;
    t.opts_ = opts;
    t.pipeline_ = std::move(pipeline);

    // 把尾部逆曲线拆成精确输出表。
    std::vector<Stage> head = t.pipeline_.stages;
    std::optional<TransformPost> post;
    if (!head.empty() && head.back().kind == Stage::Kind::InvCurves &&
        head.back().curves.size() == t.outputs_) {
        TransformPost p;
        p.tables.reserve(head.back().curves.size());
        for (const Curve& c : head.back().curves) {
            std::vector<float> table(kOutTable);
            for (size_t i = 0; i < kOutTable; ++i) {
                const double u = static_cast<double>(i) / static_cast<double>(kOutTable - 1);
                table[i] = clamp01(static_cast<float>(c.evalInverse64(u * u * u * u)));
            }
            p.tables.push_back(std::move(table));
        }
        post = std::move(p);
        head.pop_back();
        if (!head.empty() && head.back().kind == Stage::Kind::Clamp01) {
            head.pop_back();
        }
    }
    t.post_ = std::move(post);

    Pipeline headPipe(t.inputs_, std::move(head));
    const size_t mid = headPipe.outputs;
    const std::vector<Stage>& hs = headPipe.stages;
    bool hasClut = false;
    for (const Stage& s : hs) {
        if (s.kind == Stage::Kind::Clut) {
            hasClut = true;
            break;
        }
    }
    if (hs.empty()) {
        t.core_.kind = TransformCore::Kind::Shaper;
        t.core_.curves8.clear();
        t.core_.curves.clear();
        t.core_.hasMatrix = false;
    } else if (hs.size() == 1 && hs[0].kind == Stage::Kind::Curves &&
               hs[0].curves.size() == t.inputs_) {
        t.core_ = shaperCore(hs[0].curves, false, 0, 0, nullptr, nullptr);
    } else if (hs.size() == 1 && hs[0].kind == Stage::Kind::Matrix) {
        t.core_ = shaperCore({}, true, hs[0].rows, hs[0].cols, &hs[0].m, &hs[0].offset);
    } else if (hs.size() == 2 && hs[0].kind == Stage::Kind::Curves && hs[0].curves.size() == t.inputs_ &&
               hs[1].kind == Stage::Kind::Matrix) {
        t.core_ = shaperCore(hs[0].curves, true, hs[1].rows, hs[1].cols, &hs[1].m, &hs[1].offset);
    } else if (!hasClut) {
        // 纯解析头（Lab/XYZ 数学、矩阵、曲线）精确求值：网格会把插值误差经陡峭输出曲线放大。
        t.core_.kind = TransformCore::Kind::Exact;
    } else {
        switch (t.inputs_) {
            case 1:
                t.core_.kind = TransformCore::Kind::Grid;
                t.core_.grid = Clut::sample(std::vector<size_t>{kTable1D}, mid,
                                            [&](const float* i, float* o) { headPipe.eval(i, o); });
                break;
            case 3:
                t.core_.kind = TransformCore::Kind::Grid;
                t.core_.grid = Clut::sample(std::vector<size_t>{kGrid3D, kGrid3D, kGrid3D}, mid,
                                            [&](const float* i, float* o) { headPipe.eval(i, o); });
                break;
            case 4:
                t.core_.kind = TransformCore::Kind::Grid;
                t.core_.grid = Clut::sample(std::vector<size_t>{kGrid4D, kGrid4D, kGrid4D, kGrid4D}, mid,
                                            [&](const float* i, float* o) { headPipe.eval(i, o); });
                break;
            case 2:
                t.core_.kind = TransformCore::Kind::Grid;
                t.core_.grid = Clut::sample(std::vector<size_t>{kGrid3D, kGrid3D}, mid,
                                            [&](const float* i, float* o) { headPipe.eval(i, o); });
                break;
            default:
                t.core_.kind = TransformCore::Kind::Exact;
                break;
        }
    }
    // grid8：每输入通道按 8-bit 码预定位。
    if (t.core_.kind == TransformCore::Kind::Grid && t.core_.grid.inputs >= 2) {
        const Clut& c = t.core_.grid;
        for (size_t d = 0; d < c.inputs; ++d) {
            const size_t n = c.grid[d];
            // stride = 该维度上一个节点步的输出值数（同 Rust 推导：总数据 / (前缀积) × outputs）。
            size_t prefix = 1;
            for (size_t k = 0; k <= d; ++k) {
                prefix *= c.grid[k];
            }
            const size_t stride = c.data.size() / c.outputs / prefix * c.outputs;
            std::array<Transform::GridCoord, 256> coords{};
            for (int v = 0; v < 256; ++v) {
                const float p = static_cast<float>(v) / 255.0f * static_cast<float>(n - 1);
                size_t i = static_cast<size_t>(p);
                if (i > n - 2) {
                    i = n - 2;
                }
                coords[v].off = static_cast<uint32_t>(i * stride);
                coords[v].frac = p - static_cast<float>(i);
            }
            t.grid8_.push_back(coords);
        }
    }
    return t;
}

std::optional<Transform> Transform::withOptions(const Profile& src, const Profile& dst, TransformOptions opts,
                                                std::string& err) {
    auto stages = linkStages(src, dst, opts.intent, opts.bpc, err);
    if (!stages) {
        return std::nullopt;
    }
    Transform t = fromPipeline(Pipeline(src.channels(), std::move(*stages)), opts);
    if (opts.intent != Intent::AbsoluteColorimetric) {
        t.fixWhite(src.colorSpace, dst.colorSpace);
    }
    return t;
}

std::optional<Transform> Transform::proof(const Profile& src, const Profile& proofProf, const Profile& dst,
                                          Intent intent, bool bpc, bool simulatePaper, std::string& err) {
    auto first = linkStages(src, proofProf, intent, bpc, err);
    if (!first) {
        return std::nullopt;
    }
    Stage clamp;
    clamp.kind = Stage::Kind::Clamp01;
    first->push_back(clamp);
    const Intent i2 = simulatePaper ? Intent::AbsoluteColorimetric : Intent::RelativeColorimetric;
    const bool b2 = !simulatePaper;
    auto second = linkStages(proofProf, dst, i2, b2, err);
    if (!second) {
        return std::nullopt;
    }
    first->insert(first->end(), second->begin(), second->end());
    TransformOptions o;
    o.intent = intent;
    o.bpc = bpc;
    o.preciseFloat = true;
    return fromPipeline(Pipeline(src.channels(), std::move(*first)), o);
}

// 白点保持：源设备白处的设备链接节点恰取目标设备白，使纸白/白点在表量化下存活
//（如 PCS 白不是旧式 Lab 编码网格的节点）。
void Transform::fixWhite(ColorSpace src, ColorSpace dst) {
    const auto swOpt = deviceWhite(src);
    auto dwOpt = deviceWhite(dst);
    if (!swOpt || !dwOpt) {
        return;
    }
    std::vector<float> sw = *swOpt;
    std::vector<float> dw = *dwOpt;
    if (post_.has_value()) {
        // 输出曲线对加色空间 1→1；网格存的是曲线前值。
        if (dst != ColorSpace::Rgb && dst != ColorSpace::Gray) {
            return;
        }
        for (float& v : dw) {
            v = 1.0f;
        }
    }
    if (core_.kind != TransformCore::Kind::Grid) {
        return;
    }
    Clut& c = core_.grid;
    if (c.inputs != sw.size() || c.outputs != dw.size()) {
        return;
    }
    size_t off = 0;
    size_t stride = c.outputs;
    for (size_t d = c.inputs; d-- > 0;) {
        const size_t n = c.grid[d];
        const float pos = sw[d] * static_cast<float>(n - 1);
        if (std::abs(pos - std::round(pos)) > 1e-6f) {
            return;
        }
        off += static_cast<size_t>(std::round(pos)) * stride;
        stride *= n;
    }
    for (size_t k = 0; k < c.outputs; ++k) {
        c.data[off + k] = dw[k];
    }
}

void Transform::evalFast(const float* input, float* out) const {
    float mid[kPipelineMax] = {0};
    switch (core_.kind) {
        case TransformCore::Kind::Exact:
            pipeline_.eval(input, out);
            return;
        case TransformCore::Kind::Grid: {
            float inp[kPipelineMax] = {0};
            for (size_t d = 0; d < inputs_; ++d) {
                inp[d] = clamp01(input[d]);
            }
            core_.grid.eval(inp, mid);
            break;
        }
        case TransformCore::Kind::Shaper: {
            float v[kPipelineMax] = {0};
            for (size_t k = 0; k < inputs_; ++k) {
                v[k] = core_.curves.empty() ? input[k] : tableLookup(core_.curves[k], input[k]);
            }
            applyCoreMatrix(core_, v, mid, inputs_);
            break;
        }
    }
    finish(mid, out);
}

void Transform::finish(const float* mid, float* out) const {
    if (post_.has_value()) {
        for (size_t k = 0; k < outputs_; ++k) {
            out[k] = post_->eval(k, mid[k]);
        }
    } else {
        for (size_t k = 0; k < outputs_; ++k) {
            out[k] = mid[k];
        }
    }
}

void Transform::convertF32(const float* src, size_t srcStride, float* dst, size_t dstStride, size_t nPixels,
                           bool copyExtra) const {
    const bool precise = opts_.preciseFloat || core_.kind == TransformCore::Kind::Exact;
    const size_t extra =
        copyExtra ? std::min(srcStride - inputs_, dstStride - outputs_) : 0;
    float out[kPipelineMax] = {0};
    for (size_t px = 0; px < nPixels; ++px) {
        const float* sp = src + px * srcStride;
        float* dp = dst + px * dstStride;
        if (precise) {
            pipeline_.eval(sp, dp);
        } else {
            evalFast(sp, dp);
        }
        for (size_t e = 0; e < extra; ++e) {
            dp[outputs_ + e] = sp[inputs_ + e];
        }
    }
    (void)out;
}

void Transform::applyInPlace(float* buf, size_t stride, size_t nPixels) const {
    const bool precise = opts_.preciseFloat || core_.kind == TransformCore::Kind::Exact;
    float out[kPipelineMax] = {0};
    for (size_t px = 0; px < nPixels; ++px) {
        float* p = buf + px * stride;
        if (precise) {
            pipeline_.eval(p, out);
        } else {
            evalFast(p, out);
        }
        for (size_t k = 0; k < outputs_; ++k) {
            p[k] = out[k];
        }
    }
}

void Transform::convertU8(const uint8_t* src, size_t srcStride, uint8_t* dst, size_t dstStride, size_t nPixels,
                          bool copyExtra) const {
    const size_t extra = copyExtra ? std::min(srcStride - inputs_, dstStride - outputs_) : 0;
    auto q = [](float v) { return static_cast<uint8_t>(clamp01(v) * 255.0f + 0.5f); };
    float mid[kPipelineMax] = {0};
    float out[kPipelineMax] = {0};
    if (core_.kind == TransformCore::Kind::Grid && core_.grid.inputs == 3) {
        const Clut& c = core_.grid;
        const auto& g0 = grid8_[0];
        const auto& g1 = grid8_[1];
        const auto& g2 = grid8_[2];
        const size_t s1 = c.outputs * c.grid[2];
        const size_t s0 = s1 * c.grid[1];
        for (size_t px = 0; px < nPixels; ++px) {
            const uint8_t* sp = src + px * srcStride;
            uint8_t* dp = dst + px * dstStride;
            const GridCoord& a = g0[sp[0]];
            const GridCoord& b = g1[sp[1]];
            const GridCoord& cc = g2[sp[2]];
            const size_t s[3] = {s0, s1, c.outputs};
            const float r[3] = {a.frac, b.frac, cc.frac};
            c.tetra(static_cast<size_t>(a.off + b.off + cc.off), s, r, mid);
            finish(mid, out);
            for (size_t k = 0; k < outputs_; ++k) {
                dp[k] = q(out[k]);
            }
            for (size_t e = 0; e < extra; ++e) {
                dp[outputs_ + e] = sp[inputs_ + e];
            }
        }
        return;
    }
    if (core_.kind == TransformCore::Kind::Grid && core_.grid.inputs == 4) {
        const Clut& c = core_.grid;
        const size_t s3 = c.outputs;
        const size_t s2 = s3 * c.grid[3];
        const size_t s1 = s2 * c.grid[2];
        const size_t s0 = s1 * c.grid[1];
        float hi[kPipelineMax] = {0};
        for (size_t px = 0; px < nPixels; ++px) {
            const uint8_t* sp = src + px * srcStride;
            uint8_t* dp = dst + px * dstStride;
            const GridCoord& a = grid8_[0][sp[0]];
            const GridCoord& b = grid8_[1][sp[1]];
            const GridCoord& cc = grid8_[2][sp[2]];
            const GridCoord& k4 = grid8_[3][sp[3]];
            const size_t base = static_cast<size_t>(a.off + b.off + cc.off + k4.off);
            const size_t s[3] = {s1, s2, s3};
            const float r[3] = {b.frac, cc.frac, k4.frac};
            c.tetra(base, s, r, mid);
            if (a.frac > 0.0f) {
                c.tetra(base + s0, s, r, hi);
                for (size_t k = 0; k < c.outputs; ++k) {
                    mid[k] += (hi[k] - mid[k]) * a.frac;
                }
            }
            finish(mid, out);
            for (size_t k = 0; k < outputs_; ++k) {
                dp[k] = q(out[k]);
            }
            for (size_t e = 0; e < extra; ++e) {
                dp[outputs_ + e] = sp[inputs_ + e];
            }
        }
        return;
    }
    if (core_.kind == TransformCore::Kind::Shaper && !core_.curves8.empty()) {
        float v[kPipelineMax] = {0};
        for (size_t px = 0; px < nPixels; ++px) {
            const uint8_t* sp = src + px * srcStride;
            uint8_t* dp = dst + px * dstStride;
            for (size_t k = 0; k < inputs_; ++k) {
                v[k] = core_.curves8[k][sp[k]];
            }
            applyCoreMatrix(core_, v, mid, inputs_);
            finish(mid, out);
            for (size_t k = 0; k < outputs_; ++k) {
                dp[k] = q(out[k]);
            }
            for (size_t e = 0; e < extra; ++e) {
                dp[outputs_ + e] = sp[inputs_ + e];
            }
        }
        return;
    }
    // 兜底：evalFast 逐像素。
    float inp[kPipelineMax] = {0};
    for (size_t px = 0; px < nPixels; ++px) {
        const uint8_t* sp = src + px * srcStride;
        uint8_t* dp = dst + px * dstStride;
        for (size_t k = 0; k < inputs_; ++k) {
            inp[k] = sp[k] / 255.0f;
        }
        evalFast(inp, out);
        for (size_t k = 0; k < outputs_; ++k) {
            dp[k] = q(out[k]);
        }
        for (size_t e = 0; e < extra; ++e) {
            dp[outputs_ + e] = sp[inputs_ + e];
        }
    }
}

void Transform::convertU16(const uint16_t* src, size_t srcStride, uint16_t* dst, size_t dstStride, size_t nPixels,
                           bool copyExtra) const {
    const size_t extra = copyExtra ? std::min(srcStride - inputs_, dstStride - outputs_) : 0;
    float inp[kPipelineMax] = {0};
    float out[kPipelineMax] = {0};
    for (size_t px = 0; px < nPixels; ++px) {
        const uint16_t* sp = src + px * srcStride;
        uint16_t* dp = dst + px * dstStride;
        for (size_t k = 0; k < inputs_; ++k) {
            inp[k] = sp[k] / 65535.0f;
        }
        evalFast(inp, out);
        for (size_t k = 0; k < outputs_; ++k) {
            dp[k] = static_cast<uint16_t>(clamp01(out[k]) * 65535.0f + 0.5f);
        }
        for (size_t e = 0; e < extra; ++e) {
            dp[outputs_ + e] = sp[inputs_ + e];
        }
    }
}

// ------------------------------------------------------------ linking

std::optional<std::vector<Stage>> linkStages(const Profile& src, const Profile& dst, Intent intent, bool bpc,
                                             std::string& err) {
    for (const Profile* p : {&src, &dst}) {
        // Lab DeviceLink 只在同样终于 Lab 时可替代 abstract profile。
        bool labToLab = p->colorSpace == ColorSpace::Lab;
        if (labToLab) {
            for (const auto& l : p->a2b) {
                if (l && l->outputs != 3) {
                    labToLab = false;
                }
            }
            for (const auto& l : p->b2a) {
                if (l && l->inputs != 3) {
                    labToLab = false;
                }
            }
        }
        if ((p->klass == ProfileClass::DeviceLink || p->klass == ProfileClass::NamedColor ||
             p->klass == ProfileClass::Abstract) &&
            !labToLab) {
            err = std::string("unsupported ICC feature: ") + profileClassName(p->klass) +
                  " profiles cannot be used as a source or destination";
            return std::nullopt;
        }
    }
    auto srcStages = src.deviceToPcs(intent, err);
    if (!srcStages) {
        return std::nullopt;
    }
    auto dstStages = dst.pcsToDevice(intent, err);
    if (!dstStages) {
        return std::nullopt;
    }
    std::vector<Stage> stages = std::move(srcStages->first);
    const Pcs sPcs = srcStages->second;
    const Pcs dPcs = dstStages->second;

    // PCS 中的逐分量 XYZ (scale, offset)：绝对色度介质白缩放或黑点补偿。空 = 恒等。
    std::optional<std::pair<std::array<double, 3>, std::array<double, 3>>> adjust;
    if (intent == Intent::AbsoluteColorimetric) {
        const auto ws = src.mediaWhite();
        const auto wd = dst.mediaWhite();
        // wtpt 来自文件。零、负或非有限分量会把缩放变 NaN/无穷并毒化全部颜色——
        // 这种介质白拒绝绝对色度 intent（其余 intent 不除以白，仍可用）。
        auto usable = [](const std::array<double, 3>& w) {
            for (double v : w) {
                if (!std::isfinite(v) || v <= 0.0) {
                    return false;
                }
            }
            return true;
        };
        if (!usable(ws) || !usable(wd)) {
            err = "invalid ICC profile: absolute colorimetric needs a positive media white point";
            return std::nullopt;
        }
        std::array<double, 3> s = {ws[0] / wd[0], ws[1] / wd[1], ws[2] / wd[2]};
        bool identity = true;
        for (double v : s) {
            if (std::abs(v - 1.0) >= 1e-6) {
                identity = false;
                break;
            }
        }
        if (!identity) {
            adjust = std::make_pair(s, std::array<double, 3>{0.0, 0.0, 0.0});
        }
    } else if (bpc) {
        const auto bs = blackPoint(src, intent, true, err);
        if (!bs) {
            return std::nullopt;
        }
        const auto bd = blackPoint(dst, intent, false, err);
        if (!bd) {
            return std::nullopt;
        }
        if (std::abs(*bs - *bd) >= 1e-6) {
            // 中性黑点（仅亮度，同 Adobe BPC），XYZ 中线性映射：源黑落目标黑、白点不动。
            const auto w = kD50;
            std::array<double, 3> scale = {1.0, 1.0, 1.0};
            std::array<double, 3> offset = {0.0, 0.0, 0.0};
            for (int i = 0; i < 3; ++i) {
                const double s = *bs * w[i];
                const double d = *bd * w[i];
                const double a = (w[i] - d) / (w[i] - s);
                scale[i] = a;
                offset[i] = w[i] * (1.0 - a);
            }
            adjust = std::make_pair(scale, offset);
        }
    }

    if (adjust) {
        if (sPcs == Pcs::Lab) {
            Stage s;
            s.kind = Stage::Kind::LabToXyz;
            stages.push_back(s);
        }
        const auto& sc = adjust->first;
        const auto& of = adjust->second;
        stages.push_back(Stage::matrixStage(3, 3,
                                            std::vector<double>{sc[0], 0.0, 0.0, 0.0, sc[1], 0.0, 0.0, 0.0, sc[2]},
                                            std::vector<double>{of[0], of[1], of[2]}));
        if (dPcs == Pcs::Lab) {
            Stage s;
            s.kind = Stage::Kind::XyzToLab;
            stages.push_back(s);
        }
    } else if (sPcs == Pcs::Lab && dPcs == Pcs::Xyz) {
        Stage s;
        s.kind = Stage::Kind::LabToXyz;
        stages.push_back(s);
    } else if (sPcs == Pcs::Xyz && dPcs == Pcs::Lab) {
        Stage s;
        s.kind = Stage::Kind::XyzToLab;
        stages.push_back(s);
    }
    stages.insert(stages.end(), dstStages->first.begin(), dstStages->first.end());
    return stages;
}

std::optional<double> blackPoint(const Profile& p, Intent intent, bool /*input*/, std::string& err) {
    if (p.colorSpace == ColorSpace::Lab || p.colorSpace == ColorSpace::Xyz) {
        return 0.0;
    }
    bool lutBased = false;
    for (const auto& l : p.a2b) {
        if (l) {
            lutBased = true;
            break;
        }
    }
    if (lutBased && p.versionMajor >= 4 &&
        (intent == Intent::Perceptual || intent == Intent::Saturation)) {
        return kPerceptualBlack[1];
    }
    auto pcsY = [&](const std::vector<Stage>& st, Pcs pcs, const float* dev) -> double {
        Pipeline pipe(p.channels(), st);
        float o[kPipelineMax] = {0};
        pipe.eval(dev, o);
        double y = 0.0;
        if (pcs == Pcs::Xyz) {
            y = o[1];
        } else {
            y = labToXyz({static_cast<double>(o[0]), 0.0, 0.0}, kD50)[1];
        }
        return std::min(std::max(y, 0.0), 0.5);
    };
    auto toPcs = p.deviceToPcs(intent, err);
    if (!toPcs) {
        return std::nullopt;
    }
    const bool additive = p.colorSpace == ColorSpace::Rgb || p.colorSpace == ColorSpace::Gray;
    if (additive || !lutBased) {
        std::vector<float> zero(p.channels(), 0.0f);
        return pcsY(toPcs->first, toPcs->second, zero.data());
    }
    // 减色 LUT profile：可达的最暗色。
    const Intent colorimetric = intent == Intent::Saturation ? Intent::Saturation : Intent::RelativeColorimetric;
    auto fromPcs = p.pcsToDevice(colorimetric, err);
    if (fromPcs) {
        std::vector<Stage> st;
        if (fromPcs->second == Pcs::Xyz) {
            Stage s;
            s.kind = Stage::Kind::LabToXyz;
            st.push_back(s);
        }
        st.insert(st.end(), fromPcs->first.begin(), fromPcs->first.end());
        Pipeline pipe(3, st);
        float dev[kPipelineMax] = {0};
        const float labBlack[3] = {0.0f, 0.0f, 0.0f};
        pipe.eval(labBlack, dev);
        return pcsY(toPcs->first, toPcs->second, dev);
    }
    err.clear();
    std::vector<float> ones(p.channels(), 1.0f);
    return pcsY(toPcs->first, toPcs->second, ones.data());
}

}  // namespace cms
}  // namespace montage
