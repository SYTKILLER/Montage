#include "io/codecs/exr_codec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <zlib.h>

namespace montage {
namespace io {
namespace codecs {

namespace {

constexpr uint32_t kLimit = 100ull * 1000 * 1000;

inline uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t rdI32(const uint8_t* p) { return static_cast<int32_t>(rdU32(p)); }
inline uint64_t rdU64(const uint8_t* p) {
    return static_cast<uint64_t>(rdU32(p)) | (static_cast<uint64_t>(rdU32(p + 4)) << 32);
}

struct ExrChannel {
    std::string name;
    int pixelType = 1;  // 0=uint 1=half 2=float
};

// EXR RLE（OpenEXR 规范：b∈[0,127] → b+1 字面量；b∈[129,255] → 258-b 重复；128 非法）
bool exrRle(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, std::string& err) {
    size_t ip = 0;
    size_t op = 0;
    while (op < outLen) {
        if (ip >= inLen) {
            err = "exr rle: input exhausted";
            return false;
        }
        const int b = in[ip++];
        if (b < 128) {
            const size_t n = static_cast<size_t>(b) + 1;
            if (ip + n > inLen || op + n > outLen) {
                err = "exr rle: literal overflow";
                return false;
            }
            std::memcpy(out + op, in + ip, n);
            ip += n;
            op += n;
        } else if (b > 128) {
            if (ip >= inLen) {
                err = "exr rle: value truncated";
                return false;
            }
            const size_t n = 258 - static_cast<size_t>(b);
            if (op + n > outLen) {
                err = "exr rle: run overflow";
                return false;
            }
            std::memset(out + op, in[ip++], n);
            op += n;
        } else {
            err = "exr rle: invalid code 128";
            return false;
        }
    }
    return true;
}

}  // namespace

bool exrDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 8) {
        err = "exr: truncated";
        return false;
    }
    if (rdU32(data) != 0x01312F76u) {
        err = "exr: bad magic";
        return false;
    }
    const uint32_t version = rdU32(data + 4);
    if ((version & 0xFF) != 2) {
        err = "exr: unsupported version";
        return false;
    }
    if (version & 0x0200u) {
        err = "exr: tiled files unsupported (scanline only)";
        return false;
    }
    if (version & 0x0800u) {
        err = "exr: deep (non-image) parts unsupported";
        return false;
    }
    if (version & 0x1000u) {
        err = "exr: multipart files unsupported";
        return false;
    }

    // 头属性
    int compression = 0;
    int lineOrder = 0;
    int dw[4] = {0, 0, 0, 0};
    std::vector<ExrChannel> channels;
    size_t pos = 8;
    while (pos < len) {
        if (data[pos] == 0) {  // 头终止
            ++pos;
            break;
        }
        // name\0
        const size_t n0 = std::memchr(data + pos, 0, len - pos)
                              ? static_cast<const uint8_t*>(std::memchr(data + pos, 0, len - pos)) -
                                    (data + pos)
                              : 0;
        if (n0 == 0 || pos + n0 + 1 > len) {
            err = "exr: bad attribute name";
            return false;
        }
        const std::string name(reinterpret_cast<const char*>(data + pos), n0);
        pos += n0 + 1;
        // type\0
        if (pos >= len) {
            err = "exr: bad attribute type";
            return false;
        }
        const size_t t0 = std::memchr(data + pos, 0, len - pos)
                              ? static_cast<const uint8_t*>(std::memchr(data + pos, 0, len - pos)) -
                                    (data + pos)
                              : 0;
        if (t0 == 0 || pos + t0 + 1 > len) {
            err = "exr: bad attribute type";
            return false;
        }
        const std::string type(reinterpret_cast<const char*>(data + pos), t0);
        pos += t0 + 1;
        if (pos + 4 > len) {
            err = "exr: attribute size truncated";
            return false;
        }
        const uint32_t asize = rdU32(data + pos);
        pos += 4;
        if (pos + asize > len) {
            err = "exr: attribute value truncated";
            return false;
        }
        const uint8_t* val = data + pos;
        pos += asize;

        if (name == "compression" && type == "compression" && asize >= 1) {
            compression = val[0];
        } else if (name == "dataWindow" && type == "box2i" && asize >= 16) {
            for (int i = 0; i < 4; ++i) {
                dw[i] = rdI32(val + i * 4);
            }
        } else if (name == "lineOrder" && type == "lineOrder" && asize >= 1) {
            lineOrder = val[0];
        } else if (name == "channels" && type == "chlist") {
            size_t cp = 0;
            while (cp + 1 < asize && val[cp] != 0) {
                const size_t c0 = static_cast<const uint8_t*>(std::memchr(val + cp, 0, asize - cp)) -
                                  (val + cp);
                if (c0 == 0 || cp + c0 + 1 + 16 > asize) {
                    err = "exr: bad chlist entry";
                    return false;
                }
                ExrChannel ch;
                ch.name.assign(reinterpret_cast<const char*>(val + cp), c0);
                cp += c0 + 1;
                ch.pixelType = rdI32(val + cp);
                // pLinear uchar + 3 pad + xSampling + ySampling
                cp += 1 + 3 + 4 + 4;
                channels.push_back(std::move(ch));
            }
        }
        // 其余属性跳过
    }

    if (channels.empty()) {
        err = "exr: missing channels";
        return false;
    }
    if (dw[2] <= dw[0] || dw[3] <= dw[1]) {
        err = "exr: bad dataWindow";
        return false;
    }
    const int w = dw[2] - dw[0];
    const int h = dw[3] - dw[1];
    if (w > 16384 || h > 16384 || static_cast<int64_t>(w) * h > static_cast<int64_t>(kLimit)) {
        err = "exr: size out of limits";
        return false;
    }

    // 通道定位（v1：R/G/B/A/Y；类型 half/float；采样恒 1 的前提由 chlist 跳过校验保证——
    // 非 1 采样/uint 类型在使用到时拒绝）
    int chIdx[5] = {-1, -1, -1, -1, -1};  // R G B A Y
    for (size_t i = 0; i < channels.size(); ++i) {
        const std::string& n = channels[i].name;
        int slot = -1;
        if (n == "R") slot = 0;
        else if (n == "G") slot = 1;
        else if (n == "B") slot = 2;
        else if (n == "A") slot = 3;
        else if (n == "Y") slot = 4;
        if (slot >= 0) {
            if (channels[i].pixelType == 0) {
                err = "exr: uint channel '" + n + "' unsupported";
                return false;
            }
            chIdx[slot] = static_cast<int>(i);
        }
    }

    // 每行字节布局（chlist 顺序；alphabetical 由读序保证）
    int bytesPerSample[5] = {2, 2, 2, 2, 2};
    for (int i = 0; i < 5; ++i) {
        if (chIdx[i] >= 0) {
            bytesPerSample[i] = channels[chIdx[i]].pixelType == 2 ? 4 : 2;
        }
    }
    const bool hasY = chIdx[4] >= 0;
    const bool hasRGB = chIdx[0] >= 0 || chIdx[1] >= 0 || chIdx[2] >= 0;
    if (!hasRGB && !hasY) {
        err = "exr: no R/G/B/Y channel";
        return false;
    }
    size_t lineSize = 0;
    std::vector<size_t> chOffset(channels.size(), 0);
    for (size_t i = 0; i < channels.size(); ++i) {
        chOffset[i] = lineSize;
        lineSize += static_cast<size_t>(w) * (channels[i].pixelType == 2 ? 4 : 2);
    }

    // 块高
    int linesPerBlock;
    switch (compression) {
        case 0: case 1: case 2: linesPerBlock = 1; break;
        case 3: linesPerBlock = 16; break;
        default:
            err = "exr: unsupported compression (need NONE/RLE/ZIPS/ZIP)";
            return false;
    }
    const int numBlocks = (h + linesPerBlock - 1) / linesPerBlock;
    // 偏移表
    if (pos + static_cast<size_t>(numBlocks) * 8 > len) {
        err = "exr: offset table truncated";
        return false;
    }
    std::vector<uint64_t> offsets(numBlocks);
    for (int i = 0; i < numBlocks; ++i) {
        offsets[i] = rdU64(data + pos + static_cast<size_t>(i) * 8);
    }

    out.w = static_cast<uint32_t>(w);
    out.h = static_cast<uint32_t>(h);
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    std::vector<uint8_t> block;   // 解压后行数据
    std::vector<uint8_t> zbuf;    // zlib 输出缓冲
    for (int bi = 0; bi < numBlocks; ++bi) {
        const uint64_t off = offsets[bi];
        if (off == 0 || off + 8 > len) {
            err = "exr: chunk offset out of range";
            return false;
        }
        const size_t cp = static_cast<size_t>(off);
        const int y0 = rdI32(data + cp);
        const uint32_t dataSize = rdU32(data + cp + 4);
        if (cp + 8 + dataSize > len) {
            err = "exr: chunk data truncated";
            return false;
        }
        const int rows = std::min(linesPerBlock, h - y0);
        if (rows <= 0 || y0 < 0 || y0 >= h) {
            err = "exr: bad chunk y";
            return false;
        }
        const size_t blockUncompressed = lineSize * static_cast<size_t>(rows);
        block.assign(blockUncompressed, 0);
        const uint8_t* cdata = data + cp + 8;
        if (compression == 0) {
            if (dataSize < blockUncompressed) {
                err = "exr: uncompressed chunk short";
                return false;
            }
            std::memcpy(block.data(), cdata, blockUncompressed);
        } else if (compression == 1) {  // RLE：逐行
            size_t ip = 0;
            for (int r = 0; r < rows; ++r) {
                if (ip + 4 > dataSize) {
                    err = "exr: rle line header truncated";
                    return false;
                }
                const uint32_t lineLen = rdU32(cdata + ip);
                ip += 4;
                if (ip + lineLen > dataSize) {
                    err = "exr: rle line truncated";
                    return false;
                }
                if (!exrRle(cdata + ip, lineLen, block.data() + static_cast<size_t>(r) * lineSize,
                            lineSize, err)) {
                    return false;
                }
                ip += lineLen;
            }
        } else if (compression == 2) {  // ZIPS：整块单流（单行块同）
            zbuf.resize(blockUncompressed);
            uLongf dlen = blockUncompressed;
            uLong slen = dataSize;  // uLong 双关 uint32_t 地址会越界写（64 位 long=8B）
            if (uncompress2(zbuf.data(), &dlen, cdata, &slen) != Z_OK ||
                dlen != blockUncompressed) {
                err = "exr: zips uncompress failed";
                return false;
            }
            std::memcpy(block.data(), zbuf.data(), blockUncompressed);
        } else if (compression == 3) {  // ZIP：逐行独立流
            size_t ip = 0;
            for (int r = 0; r < rows; ++r) {
                if (ip + 4 > dataSize) {
                    err = "exr: zip line header truncated";
                    return false;
                }
                const uint32_t lineLen = rdU32(cdata + ip);
                ip += 4;
                if (ip + lineLen > dataSize) {
                    err = "exr: zip line truncated";
                    return false;
                }
                uLongf dlen = lineSize;
                uLong slen = lineLen;
                if (uncompress(block.data() + static_cast<size_t>(r) * lineSize, &dlen,
                               cdata + ip, slen) != Z_OK ||
                    dlen != lineSize) {
                    err = "exr: zip uncompress failed";
                    return false;
                }
                ip += lineLen;
            }
        }
        // 展开行 → RGBA（线性 → sRGB8；行序）
        const bool dec = lineOrder == 2;  // DECREASING_Y
        for (int r = 0; r < rows; ++r) {
            const int y = dec ? (h - 1 - (y0 + r)) : (y0 + r);
            uint8_t* drow = out.rgba.data() + static_cast<size_t>(y) * w * 4;
            const uint8_t* base = block.data() + static_cast<size_t>(r) * lineSize;
            auto sample = [&](int slot, int x) -> float {
                const int ci = chIdx[slot];
                if (ci < 0) {
                    return -1.0f;  // 缺席
                }
                const uint8_t* p = base + chOffset[ci] + static_cast<size_t>(x) * bytesPerSample[slot];
                return bytesPerSample[slot] == 4
                           ? [&] { float f; std::memcpy(&f, p, 4); return f; }()
                           : halfToFloat(static_cast<uint16_t>(p[0]) |
                                         (static_cast<uint16_t>(p[1]) << 8));
            };
            for (int x = 0; x < w; ++x) {
                float fr, fg, fb, fa;
                if (hasRGB) {
                    fr = sample(0, x);
                    fg = sample(1, x);
                    fb = sample(2, x);
                } else {
                    fr = fg = fb = sample(4, x);
                }
                fa = sample(3, x);
                drow[x * 4 + 0] = linearToSrgb8(fr);
                drow[x * 4 + 1] = linearToSrgb8(fg);
                drow[x * 4 + 2] = linearToSrgb8(fb);
                drow[x * 4 + 3] = chIdx[3] >= 0 ? linearToSrgb8(std::min(1.0f, std::max(0.0f, fa)))
                                                : 255;
            }
        }
    }
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
