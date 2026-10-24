#include "io/codecs/codec_util.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace io {
namespace codecs {

float halfToFloat(uint16_t h) {
    // IEEE 754 binary16 → float32（查表免分支版本位拼装）
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;  // ±0
        } else {
            // 次正规：规格化（5 位指数偏移 15 → float 偏移 127）
            uint32_t m = man;
            int e = -1;
            do {
                m <<= 1;
                ++e;
            } while ((m & 0x400u) == 0);
            bits = sign | ((127u - 15u - static_cast<uint32_t>(e) + 1u) << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);  // ±inf / nan
    } else {
        bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint8_t linearToSrgb8(float v) {
    // 线性 → sRGB 编码（IEC 61966-2-1），输入允许略出界（-0.05..1.05 钳制）
    if (!(v > 0.0f)) {
        return 0;
    }
    if (v >= 1.0f) {
        return 255;
    }
    const double s = v > 0.0031308 ? 1.055 * std::pow(static_cast<double>(v), 1.0 / 2.4) - 0.055
                                   : 12.92 * static_cast<double>(v);
    int x = static_cast<int>(s * 255.0 + 0.5);
    if (x < 0) x = 0;
    if (x > 255) x = 255;
    return static_cast<uint8_t>(x);
}

void rgbeToLinear(uint8_t r, uint8_t g, uint8_t b, uint8_t e, float out[3]) {
    // Radiance RGBE：f = m / 256 * 2^(e-128)（m = 通道/256）
    if (e == 0) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    const float scale = std::ldexp(1.0f, static_cast<int>(e) - (128 + 8));
    out[0] = static_cast<float>(r) * scale;
    out[1] = static_cast<float>(g) * scale;
    out[2] = static_cast<float>(b) * scale;
}

// ---------- 中位切分量化 ----------

namespace {

struct Box {
    int begin = 0;
    int end = 0;       // [begin, end) 索引区间（指向 mSamples）
    int64_t score = 0; // 拆分收益 = 长轴范围 × 像素数（近似体量）
};

struct Sample {
    uint8_t r, g, b;
    int count;
};

// 采样收集：步进取样防大图全量（>1M 像素步进取样使样本数 ≤ ~1M；
// 直方图 5bit/通道聚簇——中位切分质量足够，步长不破坏代表性）
void collectSamples(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<Sample>& out) {
    const uint64_t total = static_cast<uint64_t>(w) * h;
    const uint64_t stride = total > 1024ull * 1024 ? (total + (1024ull * 1024 - 1)) / (1024ull * 1024) : 1;
    // 32768 桶 5bit 直方图聚簇
    std::vector<int32_t> hist(32768, 0);
    for (uint64_t i = 0; i < total; i += stride) {
        const uint8_t* p = rgba + i * 4;
        if (p[3] == 0) {
            continue;
        }
        ++hist[((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3)];
    }
    out.clear();
    out.reserve(32768);
    for (int i = 0; i < 32768; ++i) {
        if (hist[i] > 0) {
            out.push_back({static_cast<uint8_t>(((i >> 10) << 3) + 4),
                           static_cast<uint8_t>(((i >> 5) & 31) << 3 | 4),
                           static_cast<uint8_t>((i & 31) << 3 | 4),
                           hist[i]});
        }
    }
    if (out.empty()) {
        out.push_back({0, 0, 0, 1});
    }
}

void boxBounds(const std::vector<Sample>& s, const Box& b, int lo[3], int hi[3], int64_t vol[3]) {
    for (int c = 0; c < 3; ++c) {
        lo[c] = 255;
        hi[c] = 0;
    }
    for (int i = b.begin; i < b.end; ++i) {
        const uint8_t ch[3] = {s[i].r, s[i].g, s[i].b};
        for (int c = 0; c < 3; ++c) {
            if (ch[c] < lo[c]) lo[c] = ch[c];
            if (ch[c] > hi[c]) hi[c] = ch[c];
        }
    }
    for (int c = 0; c < 3; ++c) {
        vol[c] = static_cast<int64_t>(hi[c] - lo[c]);
    }
}

}  // namespace

void medianCutPalette(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors,
                      std::vector<uint8_t>& paletteOut, std::vector<uint8_t>& indexOut) {
    std::vector<Sample> samples;
    collectSamples(rgba, w, h, samples);
    // 样本按 R 排序起步
    std::sort(samples.begin(), samples.end(),
              [](const Sample& a, const Sample& b) { return a.r < b.r; });

    if (static_cast<int>(samples.size()) <= maxColors) {
        // 样本数不足调色板上限：每样本一直接成色（无量化损失，索引 1:1）
        paletteOut.clear();
        indexOut.assign(static_cast<size_t>(w) * h, 0);
        // 需要逐像素索引：建 5bit 直方图 → 色号映射
        std::vector<int32_t> keyToIdx(32768, -1);
        for (size_t i = 0; i < samples.size(); ++i) {
            const Sample& s = samples[i];
            paletteOut.push_back(s.r);
            paletteOut.push_back(s.g);
            paletteOut.push_back(s.b);
            keyToIdx[((s.r >> 3) << 10) | ((s.g >> 3) << 5) | (s.b >> 3)] = static_cast<int32_t>(i);
        }
        for (uint64_t i = 0; i < static_cast<uint64_t>(w) * h; ++i) {
            const uint8_t* p = rgba + i * 4;
            const int k = ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3);
            indexOut[i] = static_cast<uint8_t>(keyToIdx[k] >= 0 ? keyToIdx[k] : 0);
        }
        return;
    }

    Box root;
    root.begin = 0;
    root.end = static_cast<int>(samples.size());
    std::vector<Box> boxes;
    boxes.push_back(root);
    while (static_cast<int>(boxes.size()) < maxColors) {
        // 取 score 最大盒拆分（score = 长轴范围 × 像素数）
        int best = -1;
        int64_t bestScore = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].end - boxes[i].begin < 2) {
                continue;
            }
            int lo[3], hi[3];
            int64_t vol[3];
            boxBounds(samples, boxes[i], lo, hi, vol);
            int axis = 0;
            for (int c = 1; c < 3; ++c) {
                if (vol[c] > vol[axis]) axis = c;
            }
            int64_t pop = 0;
            for (int i2 = boxes[i].begin; i2 < boxes[i].end; ++i2) {
                pop += samples[i2].count;
            }
            const int64_t sc = vol[axis] * pop;
            if (sc > bestScore) {
                bestScore = sc;
                best = static_cast<int>(i);
            }
        }
        if (best < 0 || bestScore == 0) {
            break;
        }
        // 沿长轴按样本排序 + 中位（按像素计数加权）拆分
        int lo[3], hi[3];
        int64_t vol[3];
        boxBounds(samples, boxes[best], lo, hi, vol);
        int axis = 0;
        for (int c = 1; c < 3; ++c) {
            if (vol[c] > vol[axis]) axis = c;
        }
        const uint8_t key[3] = {samples[0].r, samples[0].g, samples[0].b};  // 哑元防未用警告
        (void)key;
        const int b0 = boxes[best].begin;
        const int b1 = boxes[best].end;
        std::sort(samples.begin() + b0, samples.begin() + b1,
                  [axis](const Sample& a, const Sample& b) {
                      const uint8_t ac = axis == 0 ? a.r : (axis == 1 ? a.g : a.b);
                      const uint8_t bc = axis == 0 ? b.r : (axis == 1 ? b.g : b.b);
                      return ac < bc;
                  });
        int64_t half = 0;
        for (int i = b0; i < b1; ++i) {
            half += samples[i].count;
        }
        half /= 2;
        int64_t acc = 0;
        int mid = b0;
        for (int i = b0; i < b1 - 1; ++i) {
            acc += samples[i].count;
            mid = i + 1;
            if (acc >= half) {
                break;
            }
        }
        Box a;
        a.begin = b0;
        a.end = mid;
        Box b;
        b.begin = mid;
        b.end = b1;
        boxes[best] = a;
        boxes.push_back(b);
    }

    paletteOut.clear();
    paletteOut.reserve(boxes.size() * 3);
    std::vector<int> boxIndex(samples.size(), 0);
    for (size_t bi = 0; bi < boxes.size(); ++bi) {
        int64_t sr = 0, sg = 0, sb = 0, pop = 0;
        for (int i = boxes[bi].begin; i < boxes[bi].end; ++i) {
            sr += static_cast<int64_t>(samples[i].r) * samples[i].count;
            sg += static_cast<int64_t>(samples[i].g) * samples[i].count;
            sb += static_cast<int64_t>(samples[i].b) * samples[i].count;
            pop += samples[i].count;
            boxIndex[i] = static_cast<int>(bi);
        }
        if (pop == 0) {
            pop = 1;
        }
        paletteOut.push_back(static_cast<uint8_t>(sr / pop));
        paletteOut.push_back(static_cast<uint8_t>(sg / pop));
        paletteOut.push_back(static_cast<uint8_t>(sb / pop));
    }
    // 像素 → 最近调色板号（经 5bit 键直方图二次映射：样本代表像素簇）
    // 逐像素最近邻在 maxColors ≤256 × 全像素可接受（导出一次性成本）
    indexOut.assign(static_cast<size_t>(w) * h, 0);
    for (uint64_t i = 0; i < static_cast<uint64_t>(w) * h; ++i) {
        const uint8_t* p = rgba + i * 4;
        if (p[3] == 0) {
            continue;
        }
        // 在对应 5bit 桶内二分定位样本（samples 已被排序打乱——直接线性最近调色板）
        int bestIdx = 0;
        int bestDist = 1 << 30;
        const int n = static_cast<int>(paletteOut.size() / 3);
        for (int pi = 0; pi < n; ++pi) {
            const int dr = p[0] - paletteOut[pi * 3];
            const int dg = p[1] - paletteOut[pi * 3 + 1];
            const int db = p[2] - paletteOut[pi * 3 + 2];
            const int d = dr * dr + dg * dg + db * db;
            if (d < bestDist) {
                bestDist = d;
                bestIdx = pi;
            }
        }
        indexOut[i] = static_cast<uint8_t>(bestIdx);
    }
}

int quantizeImage(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors, bool dither,
                  std::vector<uint8_t>& paletteOut, std::vector<uint8_t>& indexOut) {
    const uint64_t n = static_cast<uint64_t>(w) * h;
    // 透明检测
    bool hasTrans = false;
    for (uint64_t i = 0; i < n; ++i) {
        if (rgba[i * 4 + 3] < 128) {
            hasTrans = true;
            break;
        }
    }
    const int colorBudget = hasTrans ? std::min(maxColors, 255) : maxColors;
    std::vector<uint8_t> pal;
    std::vector<uint8_t> idx;
    medianCutPalette(rgba, w, h, colorBudget, pal, idx);

    int transIdx = -1;
    if (hasTrans) {
        transIdx = static_cast<int>(pal.size() / 3);
        pal.push_back(0);
        pal.push_back(0);
        pal.push_back(0);
    }

    if (!dither) {
        indexOut = std::move(idx);
        // 透明像素统一映射透明索引
        if (hasTrans) {
            for (uint64_t i = 0; i < n; ++i) {
                if (rgba[i * 4 + 3] < 128) {
                    indexOut[i] = static_cast<uint8_t>(transIdx);
                }
            }
        }
        paletteOut = std::move(pal);
        return transIdx;
    }

    // Floyd–Steinberg：浮点误差扩散 + 每像素最近色（无透明扩散；alpha<128 直接透明索引）
    std::vector<float> buf(n * 3);
    for (uint64_t i = 0; i < n; ++i) {
        buf[i * 3 + 0] = rgba[i * 4 + 0];
        buf[i * 3 + 1] = rgba[i * 4 + 1];
        buf[i * 3 + 2] = rgba[i * 4 + 2];
    }
    const int nc = static_cast<int>(pal.size() / 3) - (hasTrans ? 1 : 0);
    indexOut.assign(n, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint64_t i = static_cast<uint64_t>(y) * w + x;
            if (hasTrans && rgba[i * 4 + 3] < 128) {
                indexOut[i] = static_cast<uint8_t>(transIdx);
                continue;
            }
            const float r = std::min(255.0f, std::max(0.0f, buf[i * 3 + 0]));
            const float g = std::min(255.0f, std::max(0.0f, buf[i * 3 + 1]));
            const float b = std::min(255.0f, std::max(0.0f, buf[i * 3 + 2]));
            int bestIdx = 0;
            float bestDist = 1e30f;
            for (int pi = 0; pi < nc; ++pi) {
                const float dr = r - pal[pi * 3];
                const float dg = g - pal[pi * 3 + 1];
                const float db = b - pal[pi * 3 + 2];
                const float d = dr * dr + dg * dg + db * db;
                if (d < bestDist) {
                    bestDist = d;
                    bestIdx = pi;
                }
            }
            indexOut[i] = static_cast<uint8_t>(bestIdx);
            const float er = r - pal[bestIdx * 3];
            const float eg = g - pal[bestIdx * 3 + 1];
            const float eb = b - pal[bestIdx * 3 + 2];
            // FS 核（右 7/16，下左 3/16，下 5/16，下右 1/16）
            const auto push = [&](uint32_t xx, uint32_t yy, float f) {
                if (xx >= w || yy >= h) {
                    return;
                }
                const uint64_t j = static_cast<uint64_t>(yy) * w + xx;
                if (hasTrans && rgba[j * 4 + 3] < 128) {
                    return;
                }
                buf[j * 3 + 0] += er * f;
                buf[j * 3 + 1] += eg * f;
                buf[j * 3 + 2] += eb * f;
            };
            push(x + 1, y, 7.0f / 16.0f);
            if (x > 0) push(x - 1, y + 1, 3.0f / 16.0f);
            push(x, y + 1, 5.0f / 16.0f);
            push(x + 1, y + 1, 1.0f / 16.0f);
        }
    }
    paletteOut = std::move(pal);
    return transIdx;
}

void copyRegion(const uint8_t* src, uint32_t srcW, uint32_t /*srcH*/,
                uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(w) * h * 4, 0);
    for (uint32_t r = 0; r < h; ++r) {
        const uint32_t yy = sy + r;
        std::memcpy(out.data() + static_cast<size_t>(r) * w * 4,
                    src + (static_cast<size_t>(yy) * srcW + sx) * 4,
                    static_cast<size_t>(w) * 4);
    }
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
