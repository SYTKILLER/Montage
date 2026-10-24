#ifndef MONTAGE_IO_CMS_CLUT_H
#define MONTAGE_IO_CMS_CLUT_H

// M22 色彩管理内核 · 多维颜色查找表（ICC CLUT）与四面体插值。
// 翻译自 photocraft crates/cms/src/clut.rs（MIT OR Apache-2.0）。
// 网格布局遵循 ICC.1：第一输入通道变化最慢、同一节点的输出值连续存放。3 输入表用四面体
// 插值（Sakamoto 1980 / Kasson et al. 1995）；4 输入表沿第一输入在两次四面体求值间线性
// 插值；其余输入数用多线性插值。

#include <cassert>
#include <cstdint>
#include <vector>

namespace montage {
namespace cms {

// CLUT 输入/输出通道数上限。
inline constexpr size_t kClutMaxChannels = 15;

class Clut {
  public:
    size_t inputs = 0;
    size_t outputs = 0;
    std::vector<size_t> grid;  // 每输入维度的格点数（各 ≥2，或退化维 1）
    std::vector<float> data;   // 归一化节点值，每节点 outputs 个
    std::vector<size_t> strides;

    // 从节点数据构建表（data 长度不匹配 = 未定义，调用方保证）。
    static Clut make(std::vector<size_t> grid, size_t outputs, std::vector<float> data);

    // 在每个节点采样 f（输入归一化到 [0,1]）构建表。
    template <typename F>
    static Clut sample(std::vector<size_t> grid, size_t outputs, F f) {
        const size_t inputs = grid.size();
        size_t total = 1;
        for (size_t g : grid) {
            total *= g;
        }
        std::vector<float> data(total * outputs, 0.0f);
        std::vector<size_t> idx(inputs, 0);
        std::vector<float> inp(inputs, 0.0f);
        for (size_t node = 0; node < total; ++node) {
            size_t rem = node;
            for (size_t d = inputs; d-- > 0;) {
                idx[d] = rem % grid[d];
                rem /= grid[d];
            }
            for (size_t d = 0; d < inputs; ++d) {
                inp[d] = grid[d] > 1 ? static_cast<float>(idx[d]) / static_cast<float>(grid[d] - 1) : 0.0f;
            }
            f(inp.data(), &data[node * outputs]);
        }
        return make(std::move(grid), outputs, std::move(data));
    }

    // 节点数。
    size_t nodes() const { return data.size() / outputs; }

    // 在 input（inputs 个值）插值到 out（outputs 个值）。
    void eval(const float* input, float* out) const;

    // 三线性插值（3 输入；精度对照参考路径）。
    void evalTrilinear(const float* input, float* out) const;

    // 全 2^n 角点多线性插值。
    void evalMultilinear(const float* input, float* out) const;

    // 单立方体内四面体插值（transform u8 快路径用；base = 三维偏移和）。
    inline void tetra(size_t base, const size_t (&s)[3], const float (&r)[3], float* out) const {
        const float rx = r[0], ry = r[1], rz = r[2];
        const size_t sx = s[0], sy = s[1], sz = s[2];
        const size_t c000 = base;
        const size_t c100 = base + sx;
        const size_t c010 = base + sy;
        const size_t c001 = base + sz;
        const size_t c110 = base + sx + sy;
        const size_t c101 = base + sx + sz;
        const size_t c011 = base + sy + sz;
        const size_t c111 = base + sx + sy + sz;
        // 选取包含 (rx, ry, rz) 的四面体及穿过立方体角的路径。
        size_t p1, p2, p3;
        float w1, w2, w3;
        if (rx >= ry) {
            if (ry >= rz) {
                p1 = c100; p2 = c110; p3 = c111; w1 = rx; w2 = ry; w3 = rz;
            } else if (rx >= rz) {
                p1 = c100; p2 = c101; p3 = c111; w1 = rx; w2 = rz; w3 = ry;
            } else {
                p1 = c001; p2 = c101; p3 = c111; w1 = rz; w2 = rx; w3 = ry;
            }
        } else if (rx >= rz) {
            p1 = c010; p2 = c110; p3 = c111; w1 = ry; w2 = rx; w3 = rz;
        } else if (ry >= rz) {
            p1 = c010; p2 = c011; p3 = c111; w1 = ry; w2 = rz; w3 = rx;
        } else {
            p1 = c001; p2 = c011; p3 = c111; w1 = rz; w2 = ry; w3 = rx;
        }
        for (size_t k = 0; k < outputs; ++k) {
            const float v0 = data[c000 + k];
            const float v1 = data[p1 + k];
            const float v2 = data[p2 + k];
            const float v3 = data[p3 + k];
            out[k] = v0 + (v1 - v0) * w1 + (v2 - v1) * w2 + (v3 - v2) * w3;
        }
    }

    bool operator==(const Clut& o) const {
        return inputs == o.inputs && outputs == o.outputs && grid == o.grid && data == o.data;
    }
    bool operator!=(const Clut& o) const { return !(*this == o); }

  private:
    // 定位维度 d 上的 v：(偏移贡献, 分数, 步长)。
    inline void locate(size_t d, float v, size_t& baseOut, float& fracOut, size_t& strideOut) const {
        const size_t n = grid[d];
        if (n < 2) {
            baseOut = 0;
            fracOut = 0.0f;
            strideOut = 0;
            return;
        }
        const float p = std::min(std::max(v, 0.0f), 1.0f) * static_cast<float>(n - 1);
        size_t i = static_cast<size_t>(p);
        if (i > n - 2) {
            i = n - 2;
        }
        baseOut = i * strides[d];
        fracOut = p - static_cast<float>(i);
        strideOut = strides[d];
    }

    void eval1(float v, float* out) const;
};

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_CLUT_H
