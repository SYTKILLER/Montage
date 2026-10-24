#include "io/codecs/bmp_codec.h"

#include <algorithm>
#include <cstring>

namespace montage {
namespace io {
namespace codecs {

namespace {

inline uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t rdI32(const uint8_t* p) { return static_cast<int32_t>(rdU32(p)); }
inline void wrU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}
inline void wrU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

constexpr uint32_t kLimit = 100ull * 1000 * 1000;

// 掩码值 → 8bit 展宽（按掩码位宽右对齐后线性放大到 0..255）
inline uint8_t expandMask(uint32_t v, uint32_t mask) {
    if (mask == 0) {
        return 0;
    }
    int shift = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        ++shift;
    }
    int bits = 0;
    uint32_t m = mask;
    while (m != 0) {
        m >>= 1;
        ++bits;
    }
    const uint32_t val = (v >> shift) & ((1u << bits) - 1u);
    return bits >= 8 ? static_cast<uint8_t>(val >> (bits - 8))
                     : static_cast<uint8_t>(val * 255u / ((1u << bits) - 1u));
}

}  // namespace

bool bmpDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 54) {
        err = "bmp: truncated";
        return false;
    }
    if (data[0] != 'B' || data[1] != 'M') {
        err = "bmp: bad magic";
        return false;
    }
    const uint32_t pixOffset = rdU32(data + 10);
    const uint32_t dibSize = rdU32(data + 14);
    if (dibSize < 12 || 14 + dibSize > len) {
        err = "bmp: bad dib size";
        return false;
    }
    const uint8_t* dib = data + 14;
    uint32_t w = 0;
    uint32_t h = 0;
    bool topDown = false;
    uint16_t bpp = 0;
    uint32_t compression = 0;
    uint32_t colorsUsed = 0;
    size_t palettePos = 14 + dibSize;   // 调色板紧跟 DIB
    size_t corePalEntry = 4;            // 默认 BGRX 4 字节/项；CORE(12)/OS2-V2(52,64) 才是 3

    if (dibSize == 12) {  // BITMAPCOREHEADER
        corePalEntry = 3;
        w = rdU16(dib + 4);
        h = rdU16(dib + 6);
        bpp = rdU16(dib + 10);
        compression = 0;
    } else {
        if (dibSize < 40) {
            err = "bmp: unsupported dib";
            return false;
        }
        w = static_cast<uint32_t>(rdI32(dib + 4));
        const int32_t sh = rdI32(dib + 8);
        topDown = sh < 0;
        h = static_cast<uint32_t>(topDown ? -sh : sh);
        bpp = rdU16(dib + 14);
        compression = rdU32(dib + 16);
        colorsUsed = rdU32(dib + 32);
        // OS/2 V2 特例（52/64）：调色板项 3 字节
        if (dibSize == 52 || dibSize == 64) {
            corePalEntry = 3;
        }
    }
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || static_cast<uint64_t>(w) * h > kLimit) {
        err = "bmp: size out of limits";
        return false;
    }
    // 压缩家族（4/5/6 JPEG/PNG/ALPHABITFIELDS、CMYK 族拒绝）
    if (compression == 4 || compression == 5 || (compression >= 7 && compression <= 11)) {
        err = "bmp: unsupported compression";
        return false;
    }
    if (compression == 1 || compression == 2) {  // RLE 仅 bottom-up 8/4bpp
        if (bpp != 8 && bpp != 4) {
            err = "bmp: rle bpp mismatch";
            return false;
        }
        if (topDown) {
            err = "bmp: rle top-down unsupported";
            return false;
        }
    }

    // 调色板（1/4/8bpp；BITFIELDS 16bpp 可无）
    std::vector<uint8_t> pal;  // BGRX 平铺
    if (bpp <= 8) {
        if (colorsUsed == 0) {
            colorsUsed = 1u << bpp;
        }
        const size_t entry = corePalEntry;
        if (palettePos + static_cast<size_t>(colorsUsed) * entry > len) {
            err = "bmp: palette truncated";
            return false;
        }
        pal.resize(static_cast<size_t>(colorsUsed) * 4, 0);
        for (uint32_t i = 0; i < colorsUsed; ++i) {
            const uint8_t* e = data + palettePos + static_cast<size_t>(i) * entry;
            pal[i * 4 + 0] = e[2];
            pal[i * 4 + 1] = e[1];
            pal[i * 4 + 2] = e[0];
            pal[i * 4 + 3] = 255;
        }
    }

    // 位域掩码（BITFIELDS 头内嵌或跟在 DIB 后）
    uint32_t rMask = 0, gMask = 0, bMask = 0, aMask = 0;
    size_t masksExtra = 0;  // V3+BITFIELDS 时数据前的 3-4 个 u32
    if (compression == 3) {
        if (dibSize >= 52) {  // V2+/V4/V5：掩码在头内（Red@40 Green@44 Blue@48 Alpha@52）
            rMask = rdU32(dib + 40);
            gMask = rdU32(dib + 44);
            bMask = rdU32(dib + 48);
            aMask = dibSize >= 56 ? rdU32(dib + 52) : 0;
        } else {
            if (palettePos + 12 > len) {
                err = "bmp: bitfields truncated";
                return false;
            }
            rMask = rdU32(data + palettePos);
            gMask = rdU32(data + palettePos + 4);
            bMask = rdU32(data + palettePos + 8);
            aMask = 0;
            masksExtra = 12;
        }
        if (bpp == 16 && rMask == 0 && gMask == 0 && bMask == 0) {
            rMask = 0x7C00;
            gMask = 0x03E0;
            bMask = 0x001F;
        }
    }

    const size_t rowStride = (static_cast<size_t>(w) * bpp + 31) / 32 * 4;
    size_t need = rowStride * h;
    if (compression == 1 || compression == 2) {
        need = rowStride * h;  // RLE 行界也要保证可寻址（解码按流走）
    }
    if (pixOffset + masksExtra > len || pixOffset + masksExtra + need > len) {
        // RLE 允许略小（流式终止）；裸行数据不足直接判坏
        if (compression == 0 || compression == 3) {
            err = "bmp: pixel data truncated";
            return false;
        }
    }
    const uint8_t* rows = data + pixOffset + masksExtra;

    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    // RLE8/RLE4 解码（自底向上逐行）
    if (compression == 1 || compression == 2) {
        size_t pos = 0;
        const size_t total = len - pixOffset;
        uint32_t y = 0;
        uint32_t x = 0;
        while (y < h && pos + 1 < total) {
            const uint8_t c = rows[pos];
            const uint8_t v = rows[pos + 1];
            pos += 2;
            if (c != 0) {
                // 填充 run
                for (uint8_t i = 0; i < c && x < w; ++i) {
                    uint8_t idx;
                    if (bpp == 8) {
                        idx = v;
                    } else {
                        idx = (i & 1) ? (v & 0x0F) : (v >> 4);
                    }
                    if (static_cast<uint32_t>(idx) * 4 + 3 < pal.size()) {
                        const uint8_t* p = pal.data() + idx * 4;
                        uint8_t* d = out.rgba.data() + (static_cast<size_t>(h - 1 - y) * w + x) * 4;
                        d[0] = p[0];
                        d[1] = p[1];
                        d[2] = p[2];
                        d[3] = 255;
                    }
                    ++x;
                }
            } else {
                if (v == 0) {  // EOL
                    ++y;
                    x = 0;
                } else if (v == 1) {  // EOB
                    break;
                } else if (v == 2) {  // delta
                    if (pos + 1 >= total) {
                        break;
                    }
                    x += rows[pos];
                    y += rows[pos + 1];
                    pos += 2;
                } else {  // literal：对齐字边界
                    const uint8_t n2 = v;
                    if (pos + n2 > total) {
                        break;
                    }
                    for (uint8_t i = 0; i < n2 && x < w; ++i) {
                        uint8_t idx;
                        if (bpp == 8) {
                            idx = rows[pos + i];
                        } else {
                            idx = (i & 1) ? (rows[pos + i] & 0x0F) : (rows[pos + i] >> 4);
                        }
                        if (static_cast<uint32_t>(idx) * 4 + 3 < pal.size()) {
                            const uint8_t* p = pal.data() + idx * 4;
                            uint8_t* d = out.rgba.data() + (static_cast<size_t>(h - 1 - y) * w + x) * 4;
                            d[0] = p[0];
                            d[1] = p[1];
                            d[2] = p[2];
                            d[3] = 255;
                        }
                        ++x;
                    }
                    pos += n2;
                    if ((bpp == 8 && (n2 & 1)) || (bpp == 4 && ((n2 + 1) / 2) & 1)) {
                        ++pos;
                    }
                }
            }
        }
        return true;
    }

    // 逐行裸解码
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t dy = topDown ? y : (h - 1 - y);
        uint8_t* drow = out.rgba.data() + static_cast<size_t>(dy) * w * 4;
        const uint8_t* srow = rows + static_cast<size_t>(y) * rowStride;
        switch (bpp) {
            case 1: {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t idx = (srow[x >> 3] >> (7 - (x & 7))) & 1;
                    const uint8_t* p = pal.data() + idx * 4;
                    drow[x * 4 + 0] = p[0];
                    drow[x * 4 + 1] = p[1];
                    drow[x * 4 + 2] = p[2];
                    drow[x * 4 + 3] = 255;
                }
                break;
            }
            case 4: {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t idx = (x & 1) ? (srow[x >> 1] & 0x0F) : (srow[x >> 1] >> 4);
                    const uint8_t* p = pal.data() + idx * 4;
                    drow[x * 4 + 0] = p[0];
                    drow[x * 4 + 1] = p[1];
                    drow[x * 4 + 2] = p[2];
                    drow[x * 4 + 3] = 255;
                }
                break;
            }
            case 8: {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint8_t idx = srow[x];
                    const uint8_t* p = pal.data() + idx * 4;
                    drow[x * 4 + 0] = p[0];
                    drow[x * 4 + 1] = p[1];
                    drow[x * 4 + 2] = p[2];
                    drow[x * 4 + 3] = 255;
                }
                break;
            }
            case 16: {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t v = rdU16(srow + x * 2);
                    uint8_t r, g, b;
                    if (compression == 3) {
                        r = expandMask(v, rMask);
                        g = expandMask(v, gMask);
                        b = expandMask(v, bMask);
                    } else {
                        r = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31);
                        g = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31);
                        b = static_cast<uint8_t>((v & 31) * 255 / 31);
                    }
                    drow[x * 4 + 0] = r;
                    drow[x * 4 + 1] = g;
                    drow[x * 4 + 2] = b;
                    drow[x * 4 + 3] = 255;
                }
                break;
            }
            case 24: {
                for (uint32_t x = 0; x < w; ++x) {
                    drow[x * 4 + 0] = srow[x * 3 + 2];
                    drow[x * 4 + 1] = srow[x * 3 + 1];
                    drow[x * 4 + 2] = srow[x * 3 + 0];
                    drow[x * 4 + 3] = 255;
                }
                break;
            }
            case 32: {
                // alpha 启发式：BITFIELDS 带 aMask 或全图存在非零 alpha 才用
                bool useAlpha = aMask == 0xFF000000u;
                if (!useAlpha) {
                    for (uint32_t x = 0; x < w; ++x) {
                        if (srow[x * 4 + 3] != 0) {
                            useAlpha = true;
                            break;
                        }
                    }
                }
                for (uint32_t x = 0; x < w; ++x) {
                    if (compression == 3 && (rMask != 0x00FF0000u || bMask != 0x000000FFu)) {
                        const uint32_t v = rdU32(srow + x * 4);
                        drow[x * 4 + 0] = expandMask(v, rMask);
                        drow[x * 4 + 1] = expandMask(v, gMask);
                        drow[x * 4 + 2] = expandMask(v, bMask);
                    } else {
                        drow[x * 4 + 0] = srow[x * 4 + 2];
                        drow[x * 4 + 1] = srow[x * 4 + 1];
                        drow[x * 4 + 2] = srow[x * 4 + 0];
                    }
                    drow[x * 4 + 3] = useAlpha ? srow[x * 4 + 3] : 255;
                }
                break;
            }
            default:
                err = "bmp: unsupported bpp";
                return false;
        }
    }
    return true;
}

bool bmpEncode(const uint8_t* rgba, uint32_t w, uint32_t h, bool withAlpha,
               std::vector<uint8_t>& out, std::string& err) {
    if (rgba == nullptr || w == 0 || h == 0 ||
        static_cast<uint64_t>(w) * h > kLimit) {
        err = "bmp encode: bad input";
        return false;
    }
    out.clear();
    // 32bpp：文件头(14) + V4 头(108)
    const uint32_t offset = 14 + 108;
    const size_t stride = static_cast<size_t>(w) * 4;
    const uint32_t dataSize = static_cast<uint32_t>(stride * h);
    out.reserve(offset + dataSize);
    out.insert(out.end(), {'B', 'M'});
    wrU32(out, offset + dataSize);
    wrU16(out, 0);
    wrU16(out, 0);
    wrU32(out, offset);
    // BITMAPV4HEADER
    wrU32(out, 108);
    wrU32(out, static_cast<int32_t>(w));
    wrU32(out, static_cast<int32_t>(h));  // 正 = bottom-up
    wrU16(out, 1);
    wrU16(out, 32);
    wrU32(out, 3);  // BI_BITFIELDS
    wrU32(out, dataSize);
    wrU32(out, 3780);  // 96 dpi
    wrU32(out, 3780);
    wrU32(out, 0);
    wrU32(out, 0);
    wrU32(out, 0x00FF0000);  // R
    wrU32(out, 0x0000FF00);  // G
    wrU32(out, 0x000000FF);  // B
    wrU32(out, withAlpha ? 0xFF000000u : 0x00000000u);  // A
    wrU32(out, 0x73524742);  // 'sRGB' (LCS_sRGB)
    // V4 尾段 = CIEXYZTRIPLE 36 + gamma×3 12（intent 是 V5 字段不写——头恒 108）
    for (int i = 0; i < 12; ++i) {
        wrU32(out, 0);
    }
    // 像素（bottom-up，BGRA）
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = rgba + static_cast<size_t>(h - 1 - y) * stride;
        for (uint32_t x = 0; x < w; ++x) {
            out.push_back(src[x * 4 + 2]);
            out.push_back(src[x * 4 + 1]);
            out.push_back(src[x * 4 + 0]);
            out.push_back(withAlpha ? src[x * 4 + 3] : 255);
        }
    }
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
