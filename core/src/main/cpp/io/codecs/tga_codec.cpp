#include "io/codecs/tga_codec.h"

#include <cstring>

namespace montage {
namespace io {
namespace codecs {

namespace {

constexpr uint32_t kLimit = 100ull * 1000 * 1000;

inline uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline void wrU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

// 像素读取器：按 bpp 从 colormap 或数据流展开一个 BGR(A) 像素
struct PixelSource {
    const uint8_t* cmap = nullptr;   // 调色板 BGRX 平铺（4 字节/项归一后）
    uint32_t cmapEntry = 0;          // 调色板项字节数（归一后恒 4）
    int bpp = 0;                     // 15/16/24/32
};

}  // namespace

bool tgaDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 18) {
        err = "tga: truncated header";
        return false;
    }
    const int idLen = data[0];
    const int cmapType = data[1];
    const int imageType = data[2];
    const uint16_t cmapFirst = rdU16(data + 3);
    const uint16_t cmapCount = rdU16(data + 5);
    const int cmapDepth = data[7];
    const uint16_t w = rdU16(data + 12);
    const uint16_t h = rdU16(data + 14);
    const int depth = data[16];
    const int descriptor = data[17];

    if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h > kLimit) {
        err = "tga: size out of limits";
        return false;
    }
    // 类型：1=cmap 裸 2=真彩裸 3=灰度裸 9/10/11=对应 RLE
    const bool rle = imageType >= 9;
    const int base = rle ? imageType - 8 : imageType;
    if (base != 1 && base != 2 && base != 3) {
        err = "tga: unsupported image type";
        return false;
    }
    if (base == 1 && cmapType != 1) {
        err = "tga: cmap image without colormap";
        return false;
    }

    size_t pos = 18;
    if (static_cast<size_t>(idLen) > len - pos) {
        err = "tga: id truncated";
        return false;
    }
    pos += idLen;

    // 调色板（BGR → 引擎 RGB；32 位项第 4 字节作 alpha 候选）
    std::vector<uint8_t> cmap;  // RGBX 平铺
    if (cmapType == 1) {
        if (cmapDepth != 15 && cmapDepth != 16 && cmapDepth != 24 && cmapDepth != 32) {
            err = "tga: bad cmap depth";
            return false;
        }
        const size_t entryBytes = (cmapDepth + 7) / 8;
        if (pos + static_cast<size_t>(cmapCount) * entryBytes > len) {
            err = "tga: cmap truncated";
            return false;
        }
        cmap.assign(static_cast<size_t>(cmapCount) * 4, 0);
        for (int i = 0; i < cmapCount; ++i) {
            const uint8_t* e = data + pos + static_cast<size_t>(i) * entryBytes;
            uint8_t* d = cmap.data() + static_cast<size_t>(i) * 4;
            if (cmapDepth == 15 || cmapDepth == 16) {
                const uint16_t v = rdU16(e);
                d[0] = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31);
                d[1] = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31);
                d[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
                d[3] = 255;
            } else {
                d[0] = e[2];
                d[1] = e[1];
                d[2] = e[0];
                d[3] = cmapDepth == 32 ? e[3] : 255;
            }
        }
        pos += static_cast<size_t>(cmapCount) * entryBytes;
    }

    if (depth != 8 && depth != 15 && depth != 16 && depth != 24 && depth != 32) {
        err = "tga: bad pixel depth";
        return false;
    }

    out.w = w;
    out.h = h;
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    const size_t rowBytes = static_cast<size_t>(w) * ((depth + 7) / 8);
    auto readPixel = [&](const uint8_t* src, uint8_t dst[4]) -> bool {
        // 返回 false = 越界（调用方判坏）
        if (base == 1) {
            uint32_t idx;
            if (depth == 8) {
                idx = src[0];
            } else if (depth == 15 || depth == 16) {
                idx = rdU16(src);
            } else {
                return false;
            }
            if (idx < cmapFirst || static_cast<uint32_t>(idx - cmapFirst) >= cmapCount) {
                std::memset(dst, 0, 4);
                dst[3] = 255;
                return true;
            }
            const uint8_t* e = cmap.data() + static_cast<size_t>(idx - cmapFirst) * 4;
            dst[0] = e[0];
            dst[1] = e[1];
            dst[2] = e[2];
            dst[3] = e[3];
            return true;
        }
        if (base == 3) {
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = 255;
            return true;
        }
        if (depth == 15 || depth == 16) {
            const uint16_t v = rdU16(src);
            dst[0] = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31);
            dst[1] = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31);
            dst[2] = static_cast<uint8_t>((v & 31) * 255 / 31);
            dst[3] = 255;
            return true;
        }
        dst[0] = src[2];
        dst[1] = src[1];
        dst[2] = src[0];
        dst[3] = depth == 32 ? src[3] : 255;
        return true;
    };

    // 32bpp alpha 启发式：descriptor 属性位 ≥8 声明 alpha；否则全图有非零属性才启用
    bool useAlpha = depth == 32 && (descriptor & 0x0F) >= 8;
    if (depth == 32 && (descriptor & 0x0F) < 8) {
        // 扫描首个 RLE 包/行判断
        // （大量工具写 0 位属性但真带 alpha——混合启发式与 BMP 32bpp 同族）
        size_t probe = pos;
        bool any = false;
        for (uint32_t y = 0; y < h && probe < len; ++y) {
            if (rle) {
                const uint8_t hdr = data[probe++];
                const int count = (hdr & 0x7F) + 1;
                const size_t pb = (depth + 7) / 8;
                if (hdr & 0x80) {
                    for (int i = 0; i < 1 && probe + pb <= len; ++i) {
                        if (data[probe + pb - 1] != 0) any = true;
                    }
                    probe += pb;
                } else {
                    for (int i = 0; i < count && probe + pb <= len; ++i) {
                        if (data[probe + pb - 1] != 0) any = true;
                        probe += pb;
                    }
                }
                if (any) break;
            } else {
                const size_t pb = (depth + 7) / 8;
                for (uint32_t x = 0; x < w && probe + pb <= len; ++x) {
                    if (data[probe + pb - 1] != 0) any = true;
                    probe += pb;
                }
                if (any) break;
            }
        }
        useAlpha = any;
    }

    const bool topDown = (descriptor & 0x20) != 0;
    const bool rightLeft = (descriptor & 0x10) != 0;
    const size_t pb = (depth + 7) / 8;

    if (!rle) {
        if (pos + rowBytes * h > len) {
            err = "tga: pixel data truncated";
            return false;
        }
        for (uint32_t y = 0; y < h; ++y) {
            const uint32_t dy = topDown ? y : (h - 1 - y);
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t dx = rightLeft ? (w - 1 - x) : x;
                const uint8_t* src = data + pos + (static_cast<size_t>(y) * w + x) * pb;
                uint8_t px[4];
                if (!readPixel(src, px)) {
                    err = "tga: bad cmap index depth";
                    return false;
                }
                uint8_t* d = out.rgba.data() + (static_cast<size_t>(dy) * w + dx) * 4;
                if (depth == 32 && !useAlpha) {
                    px[3] = 255;
                }
                d[0] = px[0];
                d[1] = px[1];
                d[2] = px[2];
                d[3] = px[3];
            }
        }
        return true;
    }

    // RLE：逐包展开到行缓冲再落位（行内包不跨界——TGA 规范按行 RLE）
    std::vector<uint8_t> line(rowBytes);
    for (uint32_t y = 0; y < h; ++y) {
        size_t filled = 0;
        while (filled < rowBytes) {
            if (pos >= len) {
                err = "tga: rle truncated";
                return false;
            }
            const uint8_t hdr = data[pos++];
            const int count = (hdr & 0x7F) + 1;
            if (hdr & 0x80) {
                if (pos + pb > len) {
                    err = "tga: rle packet truncated";
                    return false;
                }
                for (int i = 0; i < count && filled < rowBytes; ++i) {
                    std::memcpy(line.data() + filled, data + pos, pb);
                    filled += pb;
                }
                pos += pb;
            } else {
                if (pos + static_cast<size_t>(count) * pb > len) {
                    err = "tga: rle raw packet truncated";
                    return false;
                }
                for (int i = 0; i < count && filled < rowBytes; ++i) {
                    std::memcpy(line.data() + filled, data + pos + static_cast<size_t>(i) * pb, pb);
                    filled += pb;
                }
                pos += static_cast<size_t>(count) * pb;
            }
        }
        const uint32_t dy = topDown ? y : (h - 1 - y);
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t dx = rightLeft ? (w - 1 - x) : x;
            const uint8_t* src = line.data() + x * pb;
            uint8_t px[4];
            if (!readPixel(src, px)) {
                err = "tga: bad cmap index depth";
                return false;
            }
            if (depth == 32 && !useAlpha) {
                px[3] = 255;
            }
            uint8_t* d = out.rgba.data() + (static_cast<size_t>(dy) * w + dx) * 4;
            d[0] = px[0];
            d[1] = px[1];
            d[2] = px[2];
            d[3] = px[3];
        }
    }
    return true;
}

bool tgaEncode(const uint8_t* rgba, uint32_t w, uint32_t h, bool withAlpha,
               std::vector<uint8_t>& out, std::string& err) {
    if (rgba == nullptr || w == 0 || h == 0 ||
        static_cast<uint64_t>(w) * h > kLimit) {
        err = "tga encode: bad input";
        return false;
    }
    out.clear();
    const int depth = withAlpha ? 32 : 24;
    const size_t pb = withAlpha ? 4 : 3;
    out.push_back(0);            // id len
    out.push_back(0);            // no colormap
    out.push_back(10);           // RLE truecolor（灰度走 11 代价大——统一真彩 RLE）
    wrU16(out, 0);
    wrU16(out, 0);
    out.push_back(0);            // cmap depth
    wrU16(out, 0);               // x origin
    wrU16(out, 0);               // y origin
    wrU16(out, static_cast<uint16_t>(w));
    wrU16(out, static_cast<uint16_t>(h));
    out.push_back(static_cast<uint8_t>(depth));
    out.push_back(withAlpha ? 0x28 : 0x20);  // 属性位 8（有 alpha）+ top-down

    std::vector<uint8_t> pixel(pb);
    auto emitPacket = [&](bool rle, const uint8_t* src, int count) {
        out.push_back(static_cast<uint8_t>((rle ? 0x80 : 0x00) | (count - 1)));
        if (rle) {
            out.insert(out.end(), src, src + pb);
        } else {
            for (int i = 0; i < count; ++i) {
                out.insert(out.end(), src + static_cast<size_t>(i) * pb,
                           src + static_cast<size_t>(i) * pb + pb);
            }
        }
    };
    for (uint32_t y = 0; y < h; ++y) {
        // 行缓冲 BGR(A)（top-down 直接行序）
        std::vector<uint8_t> line(static_cast<size_t>(w) * pb);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* s = rgba + (static_cast<size_t>(y) * w + x) * 4;
            uint8_t* d = line.data() + x * pb;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            if (withAlpha) {
                d[3] = s[3];
            }
        }
        uint32_t x = 0;
        while (x < w) {
            // RLE run 探测
            uint32_t run = 1;
            while (x + run < w && run < 128 &&
                   std::memcmp(line.data() + x * pb, line.data() + (x + run) * pb, pb) == 0) {
                ++run;
            }
            if (run >= 2) {
                emitPacket(true, line.data() + x * pb, static_cast<int>(run));
                x += run;
                continue;
            }
            // 裸包：连续异色段（遇重复即断）
            uint32_t raw = 1;
            while (x + raw < w && raw < 128 &&
                   std::memcmp(line.data() + (x + raw - 1) * pb, line.data() + (x + raw) * pb, pb) != 0) {
                ++raw;
            }
            emitPacket(false, line.data() + x * pb, static_cast<int>(raw));
            x += raw;
        }
    }
    // v2 footer（26 字节：签名 18 + '.' + NUL + 扩展区/开发者目录偏移两 u32=0）
    const char* sig = "TRUEVISION-XFILE.";
    out.insert(out.end(), sig, sig + 17);
    out.push_back(0);
    out.push_back('.');
    out.push_back(0);
    for (int i = 0; i < 8; ++i) {
        out.push_back(0);
    }
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
