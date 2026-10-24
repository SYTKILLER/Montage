#ifndef MONTAGE_IO_CMS_CURVE_H
#define MONTAGE_IO_CMS_CURVE_H

// M22 色彩管理内核 · 一维色调曲线（ICC `curv`/`para` 标签类型）及其逆。
// 翻译自 photocraft crates/cms/src/curve.rs（MIT OR Apache-2.0）。
// Gamma/参数曲线解析求值（含 [0,1] 之外：负输入镜像、>1 延伸公式，HDR float 不截断）；
// 采样表把输入钳制到 [0,1]。

#include <cmath>
#include <cstdint>
#include <vector>

namespace montage {
namespace cms {

class Curve {
  public:
    enum class Kind : uint8_t { Identity, Gamma, Parametric, Table };

    Kind kind = Kind::Identity;
    double gamma = 1.0;              // Gamma
    uint16_t paraKind = 0;           // Parametric：ICC parametricCurveType 函数种类 0–4
    double p[7] = {0, 0, 0, 0, 0, 0, 0};  // [g, a, b, c, d, e, f]（未用者 0）
    std::vector<float> table;        // Table：[0,1] 均匀采样、归一化输出

    static Curve identity() { return Curve{}; }
    static Curve gammaValue(double g) {
        Curve c;
        c.kind = Kind::Gamma;
        c.gamma = g;
        return c;
    }
    static Curve parametric(uint16_t kind, const double (&params)[7]) {
        Curve c;
        c.kind = Kind::Parametric;
        c.paraKind = kind;
        for (int i = 0; i < 7; ++i) {
            c.p[i] = params[i];
        }
        return c;
    }
    static Curve fromTable(std::vector<float> t) {
        Curve c;
        c.kind = Kind::Table;
        c.table = std::move(t);
        return c;
    }

    // sRGB 传递函数（IEC 61966-2.1），ICC 参数类型 3。
    static Curve srgbTrc();

    // 参数函数 kind 存储的参数个数。
    static int paramCount(uint16_t kind);

    bool isIdentity() const;

    // f64 参考路径求值。
    double eval64(double x) const;
    // f32 快路径。
    float eval(float x) const;
    // 逆曲线求值。
    double evalInverse64(double y) const;
    float evalInverse(float y) const;

    // [0,1] 上均匀采样 n 点。
    std::vector<float> sample(size_t n) const;

    // 曲线在 [0,1] 上是否单调（增或减）。
    bool isMonotonic() const;

    bool operator==(const Curve& o) const;
    bool operator!=(const Curve& o) const { return !(*this == o); }
};

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_CURVE_H
