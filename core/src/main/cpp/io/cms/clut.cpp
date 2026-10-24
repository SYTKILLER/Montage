#include "io/cms/clut.h"

#include <algorithm>
#include <cmath>

namespace montage {
namespace cms {

Clut Clut::make(std::vector<size_t> grid, size_t outputs, std::vector<float> data) {
    const size_t inputs = grid.size();
    assert(inputs >= 1 && inputs <= kClutMaxChannels && outputs >= 1 && outputs <= kClutMaxChannels);
    std::vector<size_t> strides(inputs, 0);
    size_t s = outputs;
    for (size_t i = inputs; i-- > 0;) {
        strides[i] = s;
        s *= grid[i] > 0 ? grid[i] : 1;
    }
    assert(data.size() == s);  // CLUT 数据长度
    Clut c;
    c.inputs = inputs;
    c.outputs = outputs;
    c.grid = std::move(grid);
    c.data = std::move(data);
    c.strides = std::move(strides);
    return c;
}

void Clut::eval(const float* input, float* out) const {
    switch (inputs) {
        case 1:
            eval1(input[0], out);
            return;
        case 3: {
            size_t b0, b1, b2, s0, s1, s2;
            float r0, r1, r2;
            locate(0, input[0], b0, r0, s0);
            locate(1, input[1], b1, r1, s1);
            locate(2, input[2], b2, r2, s2);
            const size_t s[3] = {s0, s1, s2};
            const float r[3] = {r0, r1, r2};
            tetra(b0 + b1 + b2, s, r, out);
            return;
        }
        case 4: {
            size_t b0, b1, b2, b3, s0, s1, s2, s3;
            float r0, r1, r2, r3;
            locate(0, input[0], b0, r0, s0);
            locate(1, input[1], b1, r1, s1);
            locate(2, input[2], b2, r2, s2);
            locate(3, input[3], b3, r3, s3);
            const size_t base = b0 + b1 + b2 + b3;
            std::vector<float> lo(outputs);
            std::vector<float> hi(outputs);
            const size_t s[3] = {s1, s2, s3};
            const float r[3] = {r1, r2, r3};
            tetra(base, s, r, lo.data());
            if (r0 > 0.0f && s0 > 0) {
                tetra(base + s0, s, r, hi.data());
                for (size_t o = 0; o < outputs; ++o) {
                    out[o] = lo[o] + (hi[o] - lo[o]) * r0;
                }
            } else {
                for (size_t o = 0; o < outputs; ++o) {
                    out[o] = lo[o];
                }
            }
            return;
        }
        default:
            evalMultilinear(input, out);
            return;
    }
}

void Clut::eval1(float v, float* out) const {
    size_t b, s;
    float r;
    locate(0, v, b, r, s);
    for (size_t k = 0; k < outputs; ++k) {
        const float a = data[b + k];
        const float c = s > 0 ? data[b + s + k] : a;
        out[k] = a + (c - a) * r;
    }
}

void Clut::evalTrilinear(const float* input, float* out) const {
    // assert(inputs == 3);
    evalMultilinear(input, out);
}

void Clut::evalMultilinear(const float* input, float* out) const {
    const size_t n = inputs;
    size_t base = 0;
    float r[kClutMaxChannels] = {0};
    size_t s[kClutMaxChannels] = {0};
    for (size_t d = 0; d < n; ++d) {
        size_t b, ss;
        float rr;
        locate(d, input[d], b, rr, ss);
        base += b;
        r[d] = rr;
        s[d] = ss;
    }
    const size_t o = outputs;
    for (size_t k = 0; k < o; ++k) {
        out[k] = 0.0f;
    }
    for (size_t corner = 0; corner < (static_cast<size_t>(1) << n); ++corner) {
        float w = 1.0f;
        size_t off = base;
        for (size_t d = 0; d < n; ++d) {
            if ((corner & (static_cast<size_t>(1) << d)) != 0) {
                w *= r[d];
                off += s[d];
            } else {
                w *= 1.0f - r[d];
            }
        }
        if (w == 0.0f) {
            continue;
        }
        for (size_t k = 0; k < o; ++k) {
            out[k] += w * data[off + k];
        }
    }
}

}  // namespace cms
}  // namespace montage
