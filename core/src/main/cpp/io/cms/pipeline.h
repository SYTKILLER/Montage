#ifndef MONTAGE_IO_CMS_PIPELINE_H
#define MONTAGE_IO_CMS_PIPELINE_H

// M22 色彩管理内核 · float 求值管线：把归一化设备值映射到 PCS（或反向）的阶段序列。
// 精确参考路径；Transform 由此构建快速查找表。
// 翻译自 photocraft crates/cms/src/pipeline.rs（MIT OR Apache-2.0）。

#include <array>
#include <vector>

#include "io/cms/clut.h"
#include "io/cms/curve.h"
#include "io/cms/cms_math.h"

namespace montage {
namespace cms {

// 管线中流动的最大通道数。
inline constexpr size_t kPipelineMax = 16;

// 单个处理元素。
struct Stage {
    enum class Kind : uint8_t {
        Curves,      // 逐通道曲线
        InvCurves,   // 逐通道逆曲线
        Matrix,      // out[r] = Σ m[r][c]·in[c] + offset[r]，rows × cols
        Clut,
        LabToXyz,     // float CIE Lab（L 0–100）→ XYZ（D50, Y = 1）
        XyzToLab,
        DecodeLabV4,  // ICC v4 / 8-bit Lab 编码（归一化）→ float Lab，及反向
        EncodeLabV4,
        DecodeLabV2,  // ICC v2 旧式 16-bit Lab 编码（lut16Type 用）→ float Lab，及反向
        EncodeLabV2,
        DecodeXyz,    // u1Fixed15 XYZ 编码（0..=0xFFFF 归一化）→ XYZ，及反向
        EncodeXyz,
        Clamp01,      // 每通道钳制到 [0,1]
    };

    Kind kind = Kind::Clamp01;
    std::vector<Curve> curves;   // Curves/InvCurves
    size_t rows = 0, cols = 0;   // Matrix
    std::vector<double> m;       // Matrix（rows*cols 行主序）
    std::vector<double> offset;  // Matrix（rows 个）
    Clut clut{};                 // Clut

    static Stage curvesStage(std::vector<Curve> cs) {
        Stage s;
        s.kind = Kind::Curves;
        s.curves = std::move(cs);
        return s;
    }
    static Stage invCurvesStage(std::vector<Curve> cs) {
        Stage s;
        s.kind = Kind::InvCurves;
        s.curves = std::move(cs);
        return s;
    }
    static Stage matrixStage(size_t rows, size_t cols, std::vector<double> m, std::vector<double> offset) {
        Stage s;
        s.kind = Kind::Matrix;
        s.rows = rows;
        s.cols = cols;
        s.m = std::move(m);
        s.offset = std::move(offset);
        return s;
    }
    static Stage matrix3(const Mat3& m) {
        Stage s;
        s.kind = Kind::Matrix;
        s.rows = 3;
        s.cols = 3;
        s.m.reserve(9);
        s.offset.assign(3, 0.0);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                s.m.push_back(m[r][c]);
            }
        }
        return s;
    }
    static Stage clutStage(Clut c) {
        Stage s;
        s.kind = Kind::Clut;
        s.clut = std::move(c);
        return s;
    }

    // 给定 n 个输入时的输出通道数。
    size_t outputs(size_t n) const {
        switch (kind) {
            case Kind::Matrix: return rows;
            case Kind::Clut: return clut.outputs;
            default: return n;
        }
    }

    // 单像素求值：v 有 n 个值，返回输出个数。
    size_t run(float* v, size_t n) const;

    bool operator==(const Stage& o) const;
    bool operator!=(const Stage& o) const { return !(*this == o); }
};

// 固定输入/输出通道数的阶段序列。
struct Pipeline {
    size_t inputs = 0;
    size_t outputs = 0;
    std::vector<Stage> stages;

    Pipeline() = default;
    Pipeline(size_t inputs_, std::vector<Stage> stages_);

    // 单像素求值：input 有 inputs 个值，out 接收 outputs 个值。
    void eval(const float* input, float* out) const;

    // 去除空转阶段、抵消相邻逆对（Lab↔XYZ、encode/decode）、合并相邻 3×3 矩阵。
    void optimize();

    bool operator==(const Pipeline& o) const {
        return inputs == o.inputs && outputs == o.outputs && stages == o.stages;
    }
};

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_PIPELINE_H
