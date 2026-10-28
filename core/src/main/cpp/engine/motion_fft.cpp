// P5① 动感模糊 FFT（photocraft blur/motion_fft.rs 语义翻译）。架构：
//   taps(angle,dist)   —— 行核几何的合并系数（f32 计算与首见合并序逐位对拍，
//                         norm = 1/(steps+1)；photocraft PR #902 语义）
//   fftSize(needed)    —— 2^a·3^b·5^c ≥ needed 的最小值（≤8192；混合基数变换）
//   fftMixed           —— 递归 Cooley-Tukey（因子 2/3/5；f64 复数）
//   motionBlurFft      —— 预乘 4 平面 → overlap-save 512 瓦片：源块 B 以 (oy+minY,
//                         ox+minX) 为原点采集（域外零 = getPxT 越界透明同语义）、核
//                         谱由 taps 直接合成 H=Σ w·e^{-2πi(ux+vy)/N}、输出 = 循环
//                         相关 IFFT(FFT(B)·conj(H)) 的 [0..rh)×[0..rw) 区 → 直 alpha
//                         还原。求和序与行核不同（photocraft 同注）——oracle 容差对拍。
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/motion_fft.h"
#include "engine/parallel.h"

namespace montage {
namespace filters {

namespace {

using Cd = std::complex<double>;

constexpr double kPiD = 3.14159265358979323846;
constexpr float kPiF = 3.14159265358979f;
constexpr size_t kMaxSide = 8192;
constexpr size_t kMaxPoints = 4u << 20;      // 单瓦片变换点数上限（photocraft 同值）
constexpr size_t kMaxBytes = 128u << 20;     // 单缓冲字节上限（photocraft 同值）
constexpr size_t kTile = 512;                // overlap-save 输出瓦片边
constexpr int kFftThreads = 2;               // f64 复数缓冲 64MB/瓦片档——并行度收敛 2

struct Tap {
    int32_t x = 0;
    int32_t y = 0;
    double weight = 0.0;
};

// 合并系数核（photocraft taps() 逐位语义）。
bool buildTaps(float angleDeg, float distance, std::vector<Tap>& out) {
    distance = std::abs(distance);
    if (!std::isfinite(angleDeg) || distance < 1.0f || distance > 2000.0f) {
        return false;
    }
    const int steps = static_cast<int>(std::ceil(distance));
    const float rad = angleDeg * kPiF / 180.0f;
    const float cosv = std::cos(rad);
    const float sinv = std::sin(rad);
    const float norm = 1.0f / (static_cast<float>(steps) + 1.0f);
    std::vector<int32_t> sx;
    std::vector<int32_t> sy;
    std::vector<float> sw;
    sx.reserve(static_cast<size_t>(steps + 1) * 4u);
    sy.reserve(sx.capacity());
    sw.reserve(sx.capacity());
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps) - 0.5f;
        const float x = cosv * distance * t;
        const float y = -sinv * distance * t;
        const int ix = static_cast<int>(std::floor(x));
        const int iy = static_cast<int>(std::floor(y));
        const float fx = x - static_cast<float>(ix);
        const float fy = y - static_cast<float>(iy);
        const float wts[4] = {(1.0f - fx) * (1.0f - fy), fx * (1.0f - fy),
                              (1.0f - fx) * fy, fx * fy};
        const int xs[4] = {ix, ix + 1, ix, ix + 1};
        const int ys[4] = {iy, iy, iy + 1, iy + 1};
        for (int k = 0; k < 4; ++k) {
            if (wts[k] <= 0.0f) {
                continue;
            }
            size_t slot = sx.size();
            for (size_t s = 0; s < sx.size(); ++s) {
                if (sx[s] == xs[k] && sy[s] == ys[k]) {
                    slot = s;
                    break;
                }
            }
            if (slot == sx.size()) {
                sx.push_back(xs[k]);
                sy.push_back(ys[k]);
                sw.push_back(0.0f);
            }
            sw[slot] += wts[k] * norm;
        }
    }
    out.clear();
    out.reserve(sx.size());
    for (size_t s = 0; s < sx.size(); ++s) {
        out.push_back(Tap{sx[s], sy[s], static_cast<double>(sw[s])});
    }
    return !out.empty();
}

// 2^a·3^b·5^c ≥ needed 的最小值（photocraft fft_size 同序枚举；>kMaxSide 拒绝）。
size_t fftSize(size_t needed) {
    if (needed == 0 || needed > kMaxSide) {
        return 0;
    }
    size_t best = kMaxSide;
    for (size_t a = 1; a <= kMaxSide; a *= 2) {
        for (size_t b = a; b <= kMaxSide; b *= 3) {
            for (size_t c = b; c <= kMaxSide; c *= 5) {
                if (c >= needed) {
                    best = std::min(best, c);
                }
            }
        }
    }
    return best;
}

// 混合基数递归 FFT（sign=-1 正变换 / +1 逆方向；scratch = 递归工作区，不得与 buf 同向量）。
void fftMixed(Cd* buf, size_t n, int sign, std::vector<Cd>& scratch) {
    if (n <= 1) {
        return;
    }
    if (n == 2) {
        const Cd a = buf[0];
        const Cd b = buf[1];
        buf[0] = a + b;
        buf[1] = a - b;
        return;
    }
    if (n == 3) {
        constexpr double k2Pi3 = 2.0943951023931954923084289221863;  // 2π/3
        const Cd w = std::polar(1.0, sign * k2Pi3);
        const Cd a = buf[0];
        const Cd b = buf[1];
        const Cd c = buf[2];
        buf[0] = a + b + c;
        buf[1] = a + b * w + c * (w * w);
        buf[2] = a + b * (w * w) + c * w;
        return;
    }
    if (n == 5) {
        Cd tmp[5];
        for (size_t k = 0; k < 5; ++k) {
            Cd acc = 0.0;
            for (size_t j = 0; j < 5; ++j) {
                acc += buf[j] * std::polar(1.0, sign * 2.0 * kPiD * static_cast<double>(k) *
                                                static_cast<double>(j) / 5.0);
            }
            tmp[k] = acc;
        }
        for (size_t k = 0; k < 5; ++k) {
            buf[k] = tmp[k];
        }
        return;
    }
    const size_t p = (n % 2 == 0) ? 2 : (n % 3 == 0) ? 3 : 5;
    const size_t q = n / p;
    // 子序列 DFT：x[p*i + j] → gather / recurse / scatter（独立 sub，避免 scratch 别名）
    std::vector<Cd> sub(q);
    for (size_t j = 0; j < p; ++j) {
        for (size_t i = 0; i < q; ++i) {
            sub[i] = buf[p * i + j];
        }
        fftMixed(sub.data(), q, sign, scratch);
        for (size_t i = 0; i < q; ++i) {
            buf[p * i + j] = sub[i];
        }
    }
    // 合并：X[k] = Σ_j W_n^{kj} · A_j[k mod q]
    scratch.resize(n);
    for (size_t k = 0; k < n; ++k) {
        Cd acc = 0.0;
        for (size_t j = 0; j < p; ++j) {
            const double ph = sign * 2.0 * kPiD * static_cast<double>(k) *
                              static_cast<double>(j) / static_cast<double>(n);
            acc += buf[(k % q) * p + j] * std::polar(1.0, ph);
        }
        scratch[k] = acc;
    }
    std::memcpy(buf, scratch.data(), n * sizeof(Cd));
}

// 行列 2D FFT（in place，n1×n2 行主序）。
void fft2d(std::vector<Cd>& buf, size_t n1, size_t n2, int sign) {
    std::vector<Cd> scratch;
    scratch.reserve(std::max(n1, n2));
    for (size_t r = 0; r < n1; ++r) {
        fftMixed(buf.data() + r * n2, n2, sign, scratch);
    }
    std::vector<Cd> col(n1);
    for (size_t c = 0; c < n2; ++c) {
        for (size_t r = 0; r < n1; ++r) {
            col[r] = buf[r * n2 + c];
        }
        fftMixed(col.data(), n1, sign, scratch);
        for (size_t r = 0; r < n1; ++r) {
            buf[r * n2 + c] = col[r];
        }
    }
}

}  // namespace

bool motionBlurFft(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, float angleDeg,
                   float distance, std::string& err) {
    std::vector<Tap> taps;
    if (!buildTaps(angleDeg, distance, taps)) {
        err = "motion fft: bad taps";
        return false;
    }
    int32_t minX = 0, maxX = 0, minY = 0, maxY = 0;
    for (const Tap& t : taps) {
        minX = std::min(minX, t.x);
        maxX = std::max(maxX, t.x);
        minY = std::min(minY, t.y);
        maxY = std::max(maxY, t.y);
    }
    const size_t sx = static_cast<size_t>(maxX - minX);
    const size_t sy = static_cast<size_t>(maxY - minY);
    // 变换尺寸须容纳卷积读出偏移（块原点在 min 角 → 有效区在块内 -2·min 处，
    // 见下方读出注释）→ 尺寸按 tile + 2·包络 取
    const size_t n1 = fftSize(kTile + 2 * sy);
    const size_t n2 = fftSize(kTile + 2 * sx);
    if (n1 == 0 || n2 == 0 || n1 * n2 > kMaxPoints) {
        err = "motion fft: transform too large";
        return false;
    }
    if (n1 * n2 * sizeof(Cd) > kMaxBytes) {
        err = "motion fft: buffer over budget";
        return false;
    }
    const size_t n = n1 * n2;
    // 核谱：H[v,u] = Σ w·e^{-2πi(u·t'.x + v·t'.y)/N}，t' = tap - (minX,minY) ∈ [0,sx]×[0,sy]
    std::vector<Cd> kernel(n, Cd(0.0, 0.0));
    for (const Tap& t : taps) {
        const size_t kx = static_cast<size_t>(t.x - minX);
        const size_t ky = static_cast<size_t>(t.y - minY);
        kernel[ky * n2 + kx] += Cd(t.weight, 0.0);
    }
    fft2d(kernel, n1, n2, -1);
    for (size_t i = 0; i < n; ++i) {
        kernel[i] = std::conj(kernel[i]);  // 相关 = IFFT(FFT(B)·conj(H))（核实数）
    }
    // 预乘 4 平面（0..1）
    const size_t np = static_cast<size_t>(w) * h;
    std::vector<float> premul(np * 4u);
    for (size_t i = 0; i < np; ++i) {
        const float a = src[i * 4u + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) {
            premul[i * 4u + c] = src[i * 4u + c] / 255.0f * a;
        }
        premul[i * 4u + 3] = a;
    }
    std::vector<float> outp(np * 4u, 0.0f);
    const int32_t tilesY = static_cast<int32_t>((h + kTile - 1) / kTile);
    const int32_t tilesX = static_cast<int32_t>((w + kTile - 1) / kTile);
    std::vector<std::vector<float>> tileOut(static_cast<size_t>(tilesY) * tilesX);
    auto runTile = [&](size_t key) {
        const int32_t ty = static_cast<int32_t>(key / static_cast<size_t>(tilesX));
        const int32_t tx = static_cast<int32_t>(key % static_cast<size_t>(tilesX));
        const int32_t ox = tx * static_cast<int32_t>(kTile);
        const int32_t oy = ty * static_cast<int32_t>(kTile);
        const int32_t rw = static_cast<int32_t>(
            std::min<uint64_t>(kTile, w - static_cast<uint32_t>(ox)));
        const int32_t rh = static_cast<int32_t>(
            std::min<uint64_t>(kTile, h - static_cast<uint32_t>(oy)));
        std::vector<float> res(static_cast<size_t>(rw) * rh * 4u, 0.0f);
        std::vector<Cd> local(n);
        for (int ch = 0; ch < 4; ++ch) {
            std::fill(local.begin(), local.end(), Cd(0.0, 0.0));
            // 源块 B：原点 (oy+minY, ox+minX)，域外零填充
            for (size_t v = 0; v < n1; ++v) {
                const int64_t gy = static_cast<int64_t>(oy) + static_cast<int64_t>(v) + minY;
                if (gy < 0 || gy >= static_cast<int64_t>(h)) {
                    continue;
                }
                const float* row = premul.data() + static_cast<size_t>(gy) * w * 4u + ch;
                for (size_t u = 0; u < n2; ++u) {
                    const int64_t gx = static_cast<int64_t>(ox) + static_cast<int64_t>(u) + minX;
                    if (gx < 0 || gx >= static_cast<int64_t>(w)) {
                        continue;
                    }
                    local[v * n2 + u] = Cd(row[static_cast<size_t>(gx) * 4u], 0.0);
                }
            }
            fft2d(local, n1, n2, -1);
            for (size_t i = 0; i < n; ++i) {
                local[i] *= kernel[i];
            }
            fft2d(local, n1, n2, 1);
            const double inv = 1.0 / static_cast<double>(n);
            // 相关读出：conj(H) 乘法 = 循环相关 corr[m] = Σ B[m+j]·K[j]
            // = Σ w·P(oy+r+t.y, ox+c+t.x) = 图像位置 (oy+r, ox+c) 的模糊值
            // → 有效区就是块首 [0,rh)×[0,rw)（块原点 (oy+minY, ox+minX) 已补偿
            // 核包络的负偏移；核铺 [0,sy]×[0,sx] 与包络原点对齐）。
            for (int32_t r = 0; r < rh; ++r) {
                for (int32_t c = 0; c < rw; ++c) {
                    res[(static_cast<size_t>(r) * rw + c) * 4u + ch] =
                        static_cast<float>(local[static_cast<size_t>(r) * n2 +
                                                 static_cast<size_t>(c)].real() * inv);
                }
            }
        }
        tileOut[key] = std::move(res);
    };
    parallelFor(0, tileOut.size(), 1, kFftThreads, [&](size_t lo, size_t hi) {
        for (size_t k = lo; k < hi; ++k) {
            runTile(k);
        }
    });
    // 回写 + 直 alpha 还原
    for (int32_t ty = 0; ty < tilesY; ++ty) {
        for (int32_t tx = 0; tx < tilesX; ++tx) {
            const int32_t ox = tx * static_cast<int32_t>(kTile);
            const int32_t oy = ty * static_cast<int32_t>(kTile);
            const int32_t rw = static_cast<int32_t>(
                std::min<uint64_t>(kTile, w - static_cast<uint32_t>(ox)));
            const int32_t rh = static_cast<int32_t>(
                std::min<uint64_t>(kTile, h - static_cast<uint32_t>(oy)));
            const std::vector<float>& res =
                tileOut[static_cast<size_t>(ty) * tilesX + static_cast<size_t>(tx)];
            for (int32_t r = 0; r < rh; ++r) {
                for (int32_t c = 0; c < rw; ++c) {
                    const size_t gi = (static_cast<size_t>(oy + r) * w + ox + c) * 4u;
                    const float a = std::clamp(res[(static_cast<size_t>(r) * rw + c) * 4u + 3],
                                               0.0f, 1.0f);
                    const float inv = a > 1e-6f ? 1.0f / a : 0.0f;
                    for (int ch = 0; ch < 3; ++ch) {
                        const float v = res[(static_cast<size_t>(r) * rw + c) * 4u + ch] * inv;
                        dst[gi + ch] =
                            static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
                    }
                    dst[gi + 3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
                }
            }
        }
    }
    return true;
}

}  // namespace filters
}  // namespace montage
