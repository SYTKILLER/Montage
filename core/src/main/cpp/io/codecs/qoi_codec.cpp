#include "io/codecs/qoi_codec.h"

#include <cstring>

namespace montage {
namespace io {
namespace codecs {

namespace {

constexpr uint8_t kOpIndex = 0x00;   // 00iiiiii
constexpr uint8_t kOpDiff = 0x40;    // 01aabbbb
constexpr uint8_t kOpLuma = 0x80;    // 10aaaaaa bbbbbbbb
constexpr uint8_t kOpRun = 0xC0;     // 11rrrrrr（run-1，1..62）
constexpr uint8_t kOpRgb = 0xFE;
constexpr uint8_t kOpRgba = 0xFF;

inline uint8_t qoiHash(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return static_cast<uint8_t>((r * 3 + g * 5 + b * 7 + a * 11) % 64);
}

inline void putU32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

inline uint32_t getU32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

bool qoiDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 14 + 8) {  // 头 14 + 尾 8
        err = "qoi: truncated header";
        return false;
    }
    if (std::memcmp(data, "qoif", 4) != 0) {
        err = "qoi: bad magic";
        return false;
    }
    const uint32_t w = getU32be(data + 4);
    const uint32_t h = getU32be(data + 8);
    const int channels = data[12];
    const int colorspace = data[13];
    if (w == 0 || h == 0 || w > 16384 || h > 16384 ||
        static_cast<uint64_t>(w) * h > 100ull * 1000 * 1000) {
        err = "qoi: size out of limits";
        return false;
    }
    if (channels != 3 && channels != 4) {
        err = "qoi: bad channels";
        return false;
    }
    if (colorspace > 1) {
        err = "qoi: bad colorspace";
        return false;
    }

    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    uint8_t index[64][4] = {};
    uint8_t pr = 0, pg = 0, pb = 0, pa = 255;
    size_t pos = 14;
    const size_t end = len - 8;  // 尾标（7 零 + 0x01）不参与 chunk 流
    uint8_t* dst = out.rgba.data();
    size_t px = 0;
    const size_t pxTotal = static_cast<size_t>(w) * h;

    while (px < pxTotal) {
        if (pos >= end) {
            err = "qoi: chunk stream exhausted";
            return false;
        }
        const uint8_t b1 = data[pos++];
        if (b1 == kOpRgb) {
            if (pos + 3 > end) {
                err = "qoi: truncated rgb";
                return false;
            }
            pr = data[pos];
            pg = data[pos + 1];
            pb = data[pos + 2];
            pos += 3;
        } else if (b1 == kOpRgba) {
            if (pos + 4 > end) {
                err = "qoi: truncated rgba";
                return false;
            }
            pr = data[pos];
            pg = data[pos + 1];
            pb = data[pos + 2];
            pa = data[pos + 3];
            pos += 4;
        } else if ((b1 & 0xC0) == kOpIndex) {
            const uint8_t* e = index[b1 & 0x3F];
            pr = e[0];
            pg = e[1];
            pb = e[2];
            pa = e[3];
        } else if ((b1 & 0xC0) == kOpDiff) {
            pr += static_cast<uint8_t>(((b1 >> 4) & 0x03) - 2);
            pg += static_cast<uint8_t>(((b1 >> 2) & 0x03) - 2);
            pb += static_cast<uint8_t>((b1 & 0x03) - 2);
        } else if ((b1 & 0xC0) == kOpLuma) {
            if (pos >= end) {
                err = "qoi: truncated luma";
                return false;
            }
            const uint8_t b2 = data[pos++];
            const int dg = (b1 & 0x3F) - 32;
            const int dr = dg - 8 + ((b2 >> 4) & 0x0F);
            const int db = dg - 8 + (b2 & 0x0F);
            pr += static_cast<uint8_t>(dr);
            pg += static_cast<uint8_t>(dg);
            pb += static_cast<uint8_t>(db);
        } else {  // kOpRun
            const int run = (b1 & 0x3F) + 1;
            if (px + static_cast<size_t>(run) > pxTotal) {
                err = "qoi: run overruns pixels";
                return false;
            }
            for (int i = 0; i < run; ++i) {
                dst[px * 4 + 0] = pr;
                dst[px * 4 + 1] = pg;
                dst[px * 4 + 2] = pb;
                dst[px * 4 + 3] = pa;
                ++px;
            }
            continue;
        }
        index[qoiHash(pr, pg, pb, pa)][0] = pr;
        index[qoiHash(pr, pg, pb, pa)][1] = pg;
        index[qoiHash(pr, pg, pb, pa)][2] = pb;
        index[qoiHash(pr, pg, pb, pa)][3] = pa;
        dst[px * 4 + 0] = pr;
        dst[px * 4 + 1] = pg;
        dst[px * 4 + 2] = pb;
        dst[px * 4 + 3] = pa;
        ++px;
    }
    return true;
}

bool qoiEncode(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out, std::string& err) {
    if (rgba == nullptr || w == 0 || h == 0 ||
        static_cast<uint64_t>(w) * h > 100ull * 1000 * 1000) {
        err = "qoi encode: bad input";
        return false;
    }
    out.clear();
    out.reserve(static_cast<size_t>(w) * h * 4 + 64);
    out.insert(out.end(), {'q', 'o', 'i', 'f'});
    putU32be(out, w);
    putU32be(out, h);
    out.push_back(4);  // channels：RGBA（引擎域恒 4）
    out.push_back(0);  // colorspace：sRGB

    uint8_t index[64][4] = {};
    uint8_t pr = 0, pg = 0, pb = 0, pa = 255;
    const size_t n = static_cast<size_t>(w) * h;
    size_t i = 0;
    while (i < n) {
        const uint8_t r = rgba[i * 4 + 0];
        const uint8_t g = rgba[i * 4 + 1];
        const uint8_t b = rgba[i * 4 + 2];
        const uint8_t a = rgba[i * 4 + 3];
        // RUN 探测（≥2 像素同色才值得——单像素 run 恒亏 1 字节）
        if (r == pr && g == pg && b == pb && a == pa && i + 1 < n) {
            size_t run = 0;
            while (i + run < n && run < 62 &&
                   rgba[(i + run) * 4] == r && rgba[(i + run) * 4 + 1] == g &&
                   rgba[(i + run) * 4 + 2] == b && rgba[(i + run) * 4 + 3] == a) {
                ++run;
            }
            if (run >= 2) {
                out.push_back(static_cast<uint8_t>(kOpRun | (run - 1)));
                i += run;
                continue;
            }
            // run==1：落 index/diff 常规路径
        }
        if (r == pr && g == pg && b == pb && a == pa) {
            // run==1 且同色（尾像素）：run 1
            out.push_back(static_cast<uint8_t>(kOpRun | 0));
            ++i;
            continue;
        }
        const uint8_t hi = qoiHash(r, g, b, a);
        if (index[hi][0] == r && index[hi][1] == g && index[hi][2] == b && index[hi][3] == a) {
            out.push_back(static_cast<uint8_t>(kOpIndex | hi));
        } else if (a == pa &&
                   static_cast<int8_t>(static_cast<int8_t>(r) - static_cast<int8_t>(pr)) >= -2 &&
                   static_cast<int8_t>(static_cast<int8_t>(r) - static_cast<int8_t>(pr)) <= 1 &&
                   static_cast<int8_t>(static_cast<int8_t>(g) - static_cast<int8_t>(pg)) >= -2 &&
                   static_cast<int8_t>(static_cast<int8_t>(g) - static_cast<int8_t>(pg)) <= 1 &&
                   static_cast<int8_t>(static_cast<int8_t>(b) - static_cast<int8_t>(pb)) >= -2 &&
                   static_cast<int8_t>(static_cast<int8_t>(b) - static_cast<int8_t>(pb)) <= 1) {
            const int8_t dr = static_cast<int8_t>(static_cast<int8_t>(r) - static_cast<int8_t>(pr));
            const int8_t dg = static_cast<int8_t>(static_cast<int8_t>(g) - static_cast<int8_t>(pg));
            const int8_t db = static_cast<int8_t>(static_cast<int8_t>(b) - static_cast<int8_t>(pb));
            out.push_back(static_cast<uint8_t>(kOpDiff | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2)));
        } else if (a == pa) {
            const int dg = static_cast<int>(g) - static_cast<int>(pg);
            const int dr = static_cast<int>(r) - static_cast<int>(pr) - dg;
            const int db = static_cast<int>(b) - static_cast<int>(pb) - dg;
            if (dg >= -32 && dg <= 31 && dr >= -8 && dr <= 7 && db >= -8 && db <= 7) {
                out.push_back(static_cast<uint8_t>(kOpLuma | (dg + 32)));
                out.push_back(static_cast<uint8_t>(((dr + 8) << 4) | (db + 8)));
            } else {
                out.push_back(kOpRgb);
                out.push_back(r);
                out.push_back(g);
                out.push_back(b);
            }
        } else {
            out.push_back(kOpRgba);
            out.push_back(r);
            out.push_back(g);
            out.push_back(b);
            out.push_back(a);
        }
        index[hi][0] = r;
        index[hi][1] = g;
        index[hi][2] = b;
        index[hi][3] = a;
        pr = r;
        pg = g;
        pb = b;
        pa = a;
        ++i;
    }
    out.insert(out.end(), {0, 0, 0, 0, 0, 0, 0, 1});
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
