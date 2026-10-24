#ifndef MONTAGE_IO_CMS_TRANSFORM_H
#define MONTAGE_IO_CMS_TRANSFORM_H

// M22 色彩管理内核 · 经 PCS 连接两个 profile 的颜色变换：4 渲染 intent、黑点补偿（BPC）、
// 软打样链，以及像素缓冲的快速查找表求值。
// 翻译自 photocraft crates/cms/src/transform.rs（MIT OR Apache-2.0）。
// 平台化差异：单线程（rayon 并行段移除）；进程级变换缓存 v1 未移植（本批无合成器消费方）。

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "io/cms/clut.h"
#include "io/cms/pipeline.h"
#include "io/cms/profile.h"

namespace montage {
namespace cms {

// 字节缓冲的采样存储种类（convertBytesMany 用；v1 只提供逐缓冲入口）。
enum class SampleKind : uint8_t { U8, U16, F32 };

// Transform 构建选项。
struct TransformOptions {
    Intent intent = Intent::RelativeColorimetric;
    bool bpc = true;             // Adobe 式黑点补偿（绝对色度忽略）
    bool preciseFloat = true;    // true: float 缓冲走精确管线；false: 与整数同表（预览快路径）
};

// 设备链接表的格点数（整数与快速 float 求值用）。
inline constexpr size_t kGrid3D = 33;
inline constexpr size_t kGrid4D = 17;
inline constexpr size_t kTable1D = 4096;
// 输出整形表分辨率（按 x^(1/4) 索引）。
inline constexpr size_t kOutTable = 4096;

// 加速表示。
struct TransformCore {
    enum class Kind : uint8_t { Shaper, Grid, Exact };
    Kind kind = Kind::Exact;
    // Shaper：输入曲线（4096+1 项；8-bit 输入另有 256 精确项）+ 可选矩阵。
    std::vector<std::array<float, 256>> curves8;
    std::vector<std::vector<float>> curves;
    bool hasMatrix = false;
    size_t matRows = 0, matCols = 0;
    std::vector<float> matM;
    std::vector<float> matOffset;
    // Grid：输入空间上的设备链接表。
    Clut grid{};
};

// 输出逆曲线表（按 x^(1/4) 索引——纯 gamma 曲线在黑处斜率无穷也精确）。
struct TransformPost {
    std::vector<std::vector<float>> tables;

    inline float eval(size_t c, float x) const {
        const std::vector<float>& t = tables[c];
        const float u = std::sqrt(std::sqrt(std::min(std::max(x, 0.0f), 1.0f))) * static_cast<float>(kOutTable - 1);
        size_t i = static_cast<size_t>(u);
        if (i > kOutTable - 2) {
            i = kOutTable - 2;
        }
        const float f = u - static_cast<float>(i);
        return t[i] + (t[i + 1] - t[i]) * f;
    }
};

// 一个 profile 设备空间到另一个的颜色变换。构建一次（预算查找表）后可对任意多缓冲使用。
class Transform {
  public:
    Transform() = default;

    // src → dst（intent + 可选黑点补偿）。失败置 err。
    static std::optional<Transform> make(const Profile& src, const Profile& dst, Intent intent, bool bpc,
                                         std::string& err) {
        TransformOptions o;
        o.intent = intent;
        o.bpc = bpc;
        return withOptions(src, dst, o, err);
    }

    static std::optional<Transform> withOptions(const Profile& src, const Profile& dst, TransformOptions opts,
                                                std::string& err);

    // 软打样变换 src → proof → dst（PS「视图 › 校样设置」）：intent/bpc 作用于文档→打样步；
    // 打样→显示步为相对色度 + BPC（simulatePaper 时改为绝对色度无 BPC——纸色与黑墨同模拟）。
    static std::optional<Transform> proof(const Profile& src, const Profile& proofProf, const Profile& dst,
                                          Intent intent, bool bpc, bool simulatePaper, std::string& err);

    // 任意归一化值管线构建变换。
    static Transform fromPipeline(Pipeline pipeline, TransformOptions opts);

    size_t inputs() const { return inputs_; }
    size_t outputs() const { return outputs_; }
    const TransformOptions& options() const { return opts_; }
    const Pipeline& pipeline() const { return pipeline_; }

    // 单色精确求值（inputs → outputs 归一化值）。
    inline void eval(const float* input, float* out) const { pipeline_.eval(input, out); }

    // 单色查找表求值（整数缓冲路径）。
    void evalFast(const float* input, float* out) const;

    // float 像素转换。src/dst 各为整像素步进的连续缓冲（像素数 n）；
    // 颜色通道在前，copyExtra 时尾部通道（alpha 等）直拷。
    void convertF32(const float* src, size_t srcStride, float* dst, size_t dstStride, size_t nPixels,
                    bool copyExtra) const;

    // 就地 float 转换（stride ≥ max(inputs, outputs)）。
    void applyInPlace(float* buf, size_t stride, size_t nPixels) const;

    // 8-bit 像素转换（布局同 convertF32）。
    void convertU8(const uint8_t* src, size_t srcStride, uint8_t* dst, size_t dstStride, size_t nPixels,
                   bool copyExtra) const;

    // 16-bit 像素转换（布局同 convertF32；本机字节序 u16）。
    void convertU16(const uint16_t* src, size_t srcStride, uint16_t* dst, size_t dstStride, size_t nPixels,
                    bool copyExtra) const;

  private:
    size_t inputs_ = 0;
    size_t outputs_ = 0;
    TransformOptions opts_;
    Pipeline pipeline_{};
    TransformCore core_;
    std::optional<TransformPost> post_;
    // 每 8-bit 码预定位的格点坐标：(偏移贡献, 分数) × 每输入。
    struct GridCoord {
        uint32_t off;
        float frac;
    };
    std::vector<std::array<GridCoord, 256>> grid8_;

    void finish(const float* mid, float* out) const;
    void fixWhite(ColorSpace src, ColorSpace dst);
};

// src 设备值 → dst 设备值的阶段链（gamut.rs 也用）。
std::optional<std::vector<Stage>> linkStages(const Profile& src, const Profile& dst, Intent intent, bool bpc,
                                             std::string& err);

// intent 的 profile 黑点亮度（Y，D50 相对）。
// Adobe 公开 BPC 算法的结构：矩阵/TRC profile 用设备零；v4 LUT profile 在感知/饱和下用
// 感知参考黑；其余 LUT profile 用设备可达的最暗色（Lab 0 经 BToA/AToB 往返，或加色空间
// 的设备黑）。论文的目标侧曲线拟合未实现；直接用往返黑。
std::optional<double> blackPoint(const Profile& p, Intent intent, bool input, std::string& err);

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_TRANSFORM_H
