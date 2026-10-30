#include "io/codecs/exr_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// ---------- 部件头属性解析 ----------

struct ExrPartAttrs {
    int compression = 0;
    int lineOrder = 0;
    int dw[4] = {0, 0, 0, 0};
    int tileW = 0;
    int tileH = 0;
    int levelMode = 0;  // 0=ONE_LEVEL 1=MIPMAP 2=RIPMAP
    std::string partType;  // multipart 时 "scanlineimage"/"tiledimage"，单部件为空
    std::vector<ExrChannel> channels;
};

// 解析一个部件头（pos 从属性区起；终止于 null 属性名）。
bool parseExrPartHeader(const uint8_t* data, size_t len, size_t& pos, ExrPartAttrs& a,
                        std::string& err) {
    while (pos < len) {
        if (data[pos] == 0) {  // 头终止
            ++pos;
            return true;
        }
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
            a.compression = val[0];
        } else if (name == "dataWindow" && type == "box2i" && asize >= 16) {
            for (int i = 0; i < 4; ++i) {
                a.dw[i] = rdI32(val + i * 4);
            }
        } else if (name == "lineOrder" && type == "lineOrder" && asize >= 1) {
            a.lineOrder = val[0];
        } else if (name == "tiles" && type == "tiledesc" && asize >= 9) {
            a.tileW = rdI32(val);
            a.tileH = rdI32(val + 4);
            a.levelMode = val[8] & 0x0F;
        } else if (name == "type" && asize >= 1) {
            a.partType.assign(reinterpret_cast<const char*>(val), std::min<size_t>(asize, 32));
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
                cp += 4;              // pixelType i32（M25 修复沿用）
                cp += 1 + 3 + 4 + 4;  // pLinear u8 + 3 pad + xSampling + ySampling
                a.channels.push_back(std::move(ch));
            }
        }
        // 其余属性跳过
    }
    err = "exr: header unterminated";
    return false;
}

// 单部件块解压（RLE/ZIP 逐行独立流；ZIPS 整块单流；NONE 直拷）。
bool decodeExrBlock(int compression, const uint8_t* cdata, uint32_t dataSize, size_t lineSize,
                    int rows, std::vector<uint8_t>& block, std::vector<uint8_t>& zbuf,
                    std::string& err) {
    const size_t blockUncompressed = lineSize * static_cast<size_t>(rows);
    block.assign(blockUncompressed, 0);
    if (compression == 0) {
        if (dataSize < blockUncompressed) {
            err = "exr: uncompressed chunk short";
            return false;
        }
        std::memcpy(block.data(), cdata, blockUncompressed);
        return true;
    }
    if (compression == 1) {  // RLE：逐行
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
        return true;
    }
    if (compression == 2) {  // ZIPS：整块单流
        zbuf.resize(blockUncompressed);
        uLongf dlen = blockUncompressed;
        uLong slen = dataSize;  // uLong 双关 uint32_t 地址会越界写（64 位 long=8B）
        if (uncompress2(zbuf.data(), &dlen, cdata, &slen) != Z_OK ||
            dlen != blockUncompressed) {
            err = "exr: zips uncompress failed";
            return false;
        }
        std::memcpy(block.data(), zbuf.data(), blockUncompressed);
        return true;
    }
    if (compression == 3) {  // ZIP：逐行独立流
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
            if (uncompress(block.data() + static_cast<size_t>(r) * lineSize, &dlen, cdata + ip,
                           slen) != Z_OK ||
                dlen != lineSize) {
                err = "exr: zip uncompress failed";
                return false;
            }
            ip += lineLen;
        }
        return true;
    }
    err = "exr: unsupported compression (need NONE/RLE/ZIPS/ZIP)";
    return false;
}

// 通道行内偏移（宽 width 的行布局，chlist 顺序）
inline size_t exrChOffset(const std::vector<ExrChannel>& channels, int ci, int width) {
    size_t s = 0;
    for (int i = 0; i < ci; ++i) {
        s += static_cast<size_t>(width) * (channels[static_cast<size_t>(i)].pixelType == 2 ? 4 : 2);
    }
    return s;
}

}  // namespace

// P7：扫描线 + tiled（ONE_LEVEL）+ multipart（首部件）读取。tiled MIPMAP/RIPMAP、
// deep（D 通道族）、PIZ/PXR24/B44/DWAA 压缩仍明确报错（M25 范围登记延续）。
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
    if (version & 0x0800u) {
        err = "exr: deep (non-image) parts unsupported";
        return false;
    }
    const bool isTiled = (version & 0x0200u) != 0;
    const bool isMultipart = (version & 0x1000u) != 0;

    // 部件 0 头
    ExrPartAttrs a;
    size_t pos = 8;
    if (!parseExrPartHeader(data, len, pos, a, err)) {
        return false;
    }
    if (isMultipart) {
        // 多部件：跳过余下部件头（v1 渲染部件 0）；最后部件头以额外 null 终止整个头区
        if (a.partType.empty()) {
            err = "exr: multipart header missing type";
            return false;
        }
        for (int guard = 0; guard < 1024; ++guard) {
            ExrPartAttrs skip;
            if (!parseExrPartHeader(data, len, pos, skip, err)) {
                return false;
            }
            if (pos < len && data[pos] == 0) {
                ++pos;  // 末部件额外终止 null → 头区结束
                break;
            }
        }
    }

    const bool tiled = isTiled || a.partType == "tiledimage";
    if (tiled && (a.tileW <= 0 || a.tileH <= 0)) {
        err = "exr: tiled file missing tiles attribute";
        return false;
    }
    if (tiled && a.levelMode != 0) {
        err = "exr: mipmap/ripmap levels unsupported (ONE_LEVEL only)";
        return false;
    }

    if (a.channels.empty()) {
        err = "exr: missing channels";
        return false;
    }
    if (a.dw[2] < a.dw[0] || a.dw[3] < a.dw[1]) {
        err = "exr: bad dataWindow";
        return false;
    }
    // dataWindow 为闭区间（OpenEXR 规范）；单行/单列图（y1==y0）合法
    const int w = a.dw[2] - a.dw[0] + 1;
    const int h = a.dw[3] - a.dw[1] + 1;
    if (w > 16384 || h > 16384 || static_cast<int64_t>(w) * h > static_cast<int64_t>(kLimit)) {
        err = "exr: size out of limits";
        return false;
    }

    // 通道定位（v1：R/G/B/A/Y；half/float；uint 类型使用到时拒绝）
    int chIdx[5] = {-1, -1, -1, -1, -1};  // R G B A Y
    for (size_t i = 0; i < a.channels.size(); ++i) {
        const std::string& n = a.channels[i].name;
        int slot = -1;
        if (n == "R") slot = 0;
        else if (n == "G") slot = 1;
        else if (n == "B") slot = 2;
        else if (n == "A") slot = 3;
        else if (n == "Y") slot = 4;
        if (slot >= 0) {
            if (a.channels[i].pixelType == 0) {
                err = "exr: uint channel '" + n + "' unsupported";
                return false;
            }
            chIdx[slot] = static_cast<int>(i);
        }
    }
#ifdef EXR_DBG
    std::printf("[exr dbg] channels:");
    for (size_t i = 0; i < a.channels.size(); ++i) {
        std::printf(" %s(t%d)", a.channels[i].name.c_str(), a.channels[i].pixelType);
    }
    std::printf(" | chIdx R=%d G=%d B=%d A=%d Y=%d", chIdx[0], chIdx[1], chIdx[2],
                chIdx[3], chIdx[4]);
    std::printf("\n");
#endif
    const bool hasY = chIdx[4] >= 0;
    const bool hasRGB = chIdx[0] >= 0 || chIdx[1] >= 0 || chIdx[2] >= 0;
    if (!hasRGB && !hasY) {
        err = "exr: no R/G/B/Y channel";
        return false;
    }
    int bytesPerSample[5] = {2, 2, 2, 2, 2};
    for (int i = 0; i < 5; ++i) {
        if (chIdx[i] >= 0) {
            bytesPerSample[i] = a.channels[chIdx[i]].pixelType == 2 ? 4 : 2;
        }
    }
    auto lineSizeFor = [&](int width) {
        size_t s = 0;
        for (const ExrChannel& c : a.channels) {
            s += static_cast<size_t>(width) * (c.pixelType == 2 ? 4 : 2);
        }
        return s;
    };

    // 块几何 + 偏移表
    int linesPerBlock = 1;
    if (!tiled) {
        switch (a.compression) {
            case 0: case 1: case 2: linesPerBlock = 1; break;
            case 3: linesPerBlock = 16; break;
            default:
                err = "exr: unsupported compression (need NONE/RLE/ZIPS/ZIP)";
                return false;
        }
    } else {
        switch (a.compression) {
            case 0: case 1: case 2: case 3: break;
            default:
                err = "exr: unsupported compression (need NONE/RLE/ZIPS/ZIP)";
                return false;
        }
    }
    const int numTilesX = tiled ? (w + a.tileW - 1) / a.tileW : 1;
    const int numTilesY = tiled ? (h + a.tileH - 1) / a.tileH : 1;
    const size_t numBlocks = tiled
                                 ? static_cast<size_t>(numTilesX) * numTilesY
                                 : static_cast<size_t>((h + linesPerBlock - 1) / linesPerBlock);
    if (pos + numBlocks * 8 > len) {
        err = "exr: offset table truncated";
        return false;
    }
    std::vector<uint64_t> offsets(numBlocks);
    for (size_t i = 0; i < numBlocks; ++i) {
        offsets[i] = rdU64(data + pos + i * 8);
    }

    out.w = static_cast<uint32_t>(w);
    out.h = static_cast<uint32_t>(h);
    out.rgba.assign(static_cast<size_t>(w) * h * 4, 0);

    std::vector<uint8_t> block;
    std::vector<uint8_t> zbuf;
    // 行 → RGBA 展开公共核（scanline 全宽行 / tile 子区域行同构）
    auto expandRow = [&](const uint8_t* base, int width, uint8_t* drow) {
        for (int x = 0; x < width; ++x) {
            float fr, fg, fb, fa;
            auto sample = [&](int slot) -> float {
                const int ci = chIdx[slot];
                if (ci < 0) {
                    return -1.0f;  // 缺席
                }
                const uint8_t* p = base + exrChOffset(a.channels, ci, width) +
                                   static_cast<size_t>(x) * bytesPerSample[slot];
                return bytesPerSample[slot] == 4
                           ? [&] { float f; std::memcpy(&f, p, 4); return f; }()
                           : halfToFloat(static_cast<uint16_t>(p[0]) |
                                         (static_cast<uint16_t>(p[1]) << 8));
            };
            if (hasRGB) {
                fr = sample(0);
                fg = sample(1);
                fb = sample(2);
            } else {
                fr = fg = fb = sample(4);
            }
            fa = sample(3);
            drow[x * 4 + 0] = linearToSrgb8(fr);
            drow[x * 4 + 1] = linearToSrgb8(fg);
            drow[x * 4 + 2] = linearToSrgb8(fb);
            drow[x * 4 + 3] = chIdx[3] >= 0 ? linearToSrgb8(std::min(1.0f, std::max(0.0f, fa)))
                                            : 255;
        }
    };

    for (size_t bi = 0; bi < numBlocks; ++bi) {
        const uint64_t off = offsets[bi];
        if (off == 0 || off + 8 > len) {
            err = "exr: chunk offset out of range";
            return false;
        }
        size_t cp = static_cast<size_t>(off);
        if (isMultipart) {
            const uint32_t part = rdU32(data + cp);
            if (part != 0) {
                err = "exr: multipart part table misaligned";
                return false;
            }
            cp += 4;
        }
        if (!tiled) {
            // 扫描线块：[y i32][dataSize u32][data]
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
            const size_t ls = lineSizeFor(w);
            if (!decodeExrBlock(a.compression, data + cp + 8, dataSize, ls, rows, block, zbuf,
                                err)) {
                return false;
            }
            const bool dec = a.lineOrder == 2;  // DECREASING_Y
            for (int r = 0; r < rows; ++r) {
                const int y = dec ? (h - 1 - (y0 + r)) : (y0 + r);
                expandRow(block.data() + static_cast<size_t>(r) * ls, w,
                          out.rgba.data() + static_cast<size_t>(y) * w * 4);
            }
            continue;
        }
        // tiled 块：[tx][ty][lx][ly][dataSize]（ONE_LEVEL lx/ly 恒 0）
        const int tx = rdI32(data + cp);
        const int ty = rdI32(data + cp + 4);
        if (rdI32(data + cp + 8) != 0 || rdI32(data + cp + 12) != 0) {
            err = "exr: mipmap/ripmap levels unsupported (ONE_LEVEL only)";
            return false;
        }
        const uint32_t dataSize = rdU32(data + cp + 16);
        if (cp + 20 + dataSize > len) {
            err = "exr: chunk data truncated";
            return false;
        }
        if (tx < 0 || tx >= numTilesX || ty < 0 || ty >= numTilesY) {
            err = "exr: bad tile coords";
            return false;
        }
        const int tw = std::min(a.tileW, w - tx * a.tileW);
        const int th = std::min(a.tileH, h - ty * a.tileH);
        if (tw <= 0 || th <= 0) {
            err = "exr: bad tile size";
            return false;
        }
        const size_t ls = lineSizeFor(tw);
        if (!decodeExrBlock(a.compression, data + cp + 20, dataSize, ls, th, block, zbuf, err)) {
            return false;
        }
        // DECREASING_Y：tileY 自底向上计数
        const int tileRow = a.lineOrder == 2 ? (numTilesY - 1 - ty) : ty;
        const int y0t = tileRow * a.tileH;
        for (int r = 0; r < th; ++r) {
            uint8_t* drow =
                out.rgba.data() + (static_cast<size_t>(y0t + r) * w + static_cast<size_t>(tx) * a.tileW) * 4;
            expandRow(block.data() + static_cast<size_t>(r) * ls, tw, drow);
        }
    }
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
