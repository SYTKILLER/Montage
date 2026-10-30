#include "io/codecs/tiff_codec.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <zlib.h>

#include "io/psd/psd_reader.h"
#include "io/psd/psd_import.h"

namespace montage {
namespace io {
namespace codecs {

namespace {

constexpr uint32_t kLimit = 100ull * 1000 * 1000;

// ---------- 元读取 ----------

struct Reader {
    const uint8_t* d = nullptr;
    size_t n = 0;
    bool le = true;
    bool big = false;  // BigTIFF

    uint16_t u16(size_t off) const {
        if (off + 2 > n) return 0;
        return le ? static_cast<uint16_t>(d[off]) | (static_cast<uint16_t>(d[off + 1]) << 8)
                  : static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
    }
    uint32_t u32(size_t off) const {
        if (off + 4 > n) return 0;
        return le ? static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
                        (static_cast<uint32_t>(d[off + 2]) << 16) |
                        (static_cast<uint32_t>(d[off + 3]) << 24)
                  : (static_cast<uint32_t>(d[off]) << 24) | (static_cast<uint32_t>(d[off + 1]) << 16) |
                        (static_cast<uint32_t>(d[off + 2]) << 8) | static_cast<uint32_t>(d[off + 3]);
    }
    uint64_t u64(size_t off) const {
        return big ? (static_cast<uint64_t>(u32(off)) | (static_cast<uint64_t>(u32(off + 4)) << 32))
                   : u32(off);
    }
};

// IFD 条目值读取：返回字节视图（内联或外置 offset）
bool entryValue(const Reader& r, size_t ifdPos, size_t idx, size_t& dataOff, uint64_t& count,
                uint16_t& type) {
    const size_t ent = ifdPos + (r.big ? 8 : 2) + idx * (r.big ? 20 : 12);
    type = r.u16(ent + 2);        // 条目 = tag(2)+type(2)+count(4/8)+value(4/8)
    count = r.u64(ent + 4);
    static const int kSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8, 8, 0, 8, 8, 8};
    if (type == 0 || type >= 19) {
        return false;
    }
    const uint64_t bytes = kSize[type] * count;
    const uint64_t inlineCap = r.big ? 8 : 4;
    if (bytes <= inlineCap) {
        dataOff = ent + (r.big ? 12 : 8);
    } else {
        dataOff = static_cast<size_t>(r.u64(ent + (r.big ? 12 : 8)));
    }
    return dataOff + bytes <= r.n;
}

std::vector<uint64_t> entryList(const Reader& r, size_t ifdPos, size_t idx) {
    uint16_t type = 0;
    uint64_t count = 0;
    size_t off = 0;
    if (!entryValue(r, ifdPos, idx, off, count, type)) {
        return {};
    }
    std::vector<uint64_t> out;
    static const int kSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8, 8, 0, 8, 8, 8};
    for (uint64_t i = 0; i < count; ++i) {
        switch (type) {
            case 3: out.push_back(r.u16(off + i * 2)); break;
            case 4: out.push_back(r.u32(off + i * 4)); break;
            case 16: out.push_back(r.u64(off + i * 8)); break;
            default: out.push_back(r.u32(off + i * 4)); break;
        }
    }
    return out;
}

// tag →（首个条目）→ 值数组；不存在返回空。
std::vector<uint64_t> entryListByTag(const Reader& r, size_t ifdPos, uint16_t tag) {
    const uint64_t nEntries = r.big ? r.u64(ifdPos) : r.u16(ifdPos);
    const size_t esize = r.big ? 20 : 12;
    const size_t ebase = ifdPos + (r.big ? 8 : 2);  // BigTIFF nEntries 是 u64
    for (uint64_t i = 0; i < nEntries; ++i) {
        if (r.u16(ebase + static_cast<size_t>(i) * esize) == tag) {
            return entryList(r, ifdPos, static_cast<size_t>(i));
        }
    }
    return {};
}

// tag →（首个条目）→ 标量值；条目不存在返回 def。
// 注意第三参是 tag 而非条目索引（调用方语义；与 entryList 的 idx 参数区分）
uint64_t entryScalar(const Reader& r, size_t ifdPos, uint16_t tag, uint64_t def) {
    const uint64_t nEntries = r.big ? r.u64(ifdPos) : r.u16(ifdPos);
    const size_t esize = r.big ? 20 : 12;
    const size_t ebase = ifdPos + (r.big ? 8 : 2);
    for (uint64_t i = 0; i < nEntries; ++i) {
        if (r.u16(ebase + static_cast<size_t>(i) * esize) == tag) {
            const auto v = entryList(r, ifdPos, static_cast<size_t>(i));
            return v.empty() ? def : v[0];
        }
    }
    return def;
}

// ---------- 压缩解码 ----------

// TIFF LZW：MSB 先行、early change（next 达 2^w - 1 即加宽）
bool tiffLzw(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, std::string& err) {
    int width = 9;
    int next = 258;
    uint32_t acc = 0;
    int nbits = 0;
    size_t ip = 0;
    std::vector<uint16_t> prefix(4096, 0);
    std::vector<uint8_t> suffix(4096, 0);
    for (int i = 0; i < 256; ++i) {
        suffix[i] = static_cast<uint8_t>(i);
    }
    uint8_t stack[4096];
    int prev = -1;
    out.clear();
    out.reserve(inLen * 2 + 1024);
    while (true) {
        while (nbits < width) {
            if (ip >= inLen) {
                return true;  // 流尽（无 EOI 的残缺流按已得数据收场）
            }
            acc = (acc << 8) | in[ip++];
            nbits += 8;
        }
        const int code = static_cast<int>((acc >> (nbits - width)) & ((1u << width) - 1u));
        nbits -= width;
        if (code == 256) {  // clear
            width = 9;
            next = 258;
            prev = -1;
            continue;
        }
        if (code == 257) {
            break;
        }
        if (code > next || (code == next && prev < 0)) {
            err = "tiff lzw: code out of dict";
            return false;
        }
        int sp = 0;
        int cur = code;
        if (code == next) {
            stack[sp++] = suffix[prev];
            cur = prev;
        }
        while (cur >= 258 && sp < 4096) {
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        if (sp >= 4096) {
            err = "tiff lzw: chain overflow";
            return false;
        }
        stack[sp++] = suffix[cur];
        const uint8_t first = suffix[cur];
        for (int i = sp - 1; i >= 0; --i) {
            out.push_back(stack[i]);
        }
        if (prev >= 0 && next < 4096) {
            prefix[next] = static_cast<uint16_t>(prev);
            suffix[next] = first;
            ++next;
            if (next == (1 << width) - 1 && width < 12) {  // early change
                ++width;
            }
        }
        prev = code;
    }
    return true;
}

bool tiffPackbits(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
    size_t ip = 0;
    size_t op = 0;
    while (ip < inLen && op < outLen) {
        const int8_t n = static_cast<int8_t>(in[ip++]);
        if (n >= 0) {
            const size_t cnt = static_cast<size_t>(n) + 1;
            if (ip + cnt > inLen || op + cnt > outLen) {
                return false;
            }
            std::memcpy(out + op, in + ip, cnt);
            ip += cnt;
            op += cnt;
        } else if (n != -128) {
            const size_t cnt = static_cast<size_t>(-n) + 1;
            if (ip >= inLen || op + cnt > outLen) {
                return false;
            }
            std::memset(out + op, in[ip++], cnt);
            op += cnt;
        }
        // -128 = no-op
    }
    return op == outLen || ip >= inLen;
}

bool tiffInflate(const uint8_t* in, size_t inLen, std::vector<uint8_t>& out, size_t expect,
                 std::string& err) {
    out.resize(expect);
    uLongf dlen = expect;
    if (uncompress(out.data(), &dlen, in, static_cast<uLong>(inLen)) != Z_OK) {
        err = "tiff: inflate failed";
        return false;
    }
    return true;
}

// 水平差值预测器还原（chunky 或单平面；bps 8/16/32f 按 32f 不支持 predictor 语义）
void undoPredictor(uint8_t* row, size_t rowBytes, uint32_t samples, int bps) {
    if (bps == 8) {
        for (size_t i = samples; i < rowBytes; ++i) {
            row[i] = static_cast<uint8_t>(row[i] + row[i - samples]);
        }
    } else if (bps == 16) {
        const size_t count = rowBytes / 2;
        for (size_t i = samples; i < count; ++i) {
            const size_t p = i * 2;
            uint16_t v = static_cast<uint16_t>(row[p] | (row[p + 1] << 8));
            const size_t q = (i - samples) * 2;
            const uint16_t pv = static_cast<uint16_t>(row[q] | (row[q + 1] << 8));
            v = static_cast<uint16_t>(v + pv);
            row[p] = static_cast<uint8_t>(v & 0xFF);
            row[p + 1] = static_cast<uint8_t>(v >> 8);
        }
    }
    // 其他 bps predictor 罕见——忽略（登记者上游自己不会写）
}

// 位流取样本（1/2/4 位 MSB 先行；8/16/32 按文件字节序）
inline double unpackSample(const uint8_t* base, size_t bitIdx, uint64_t bps, bool le) {
    // bitIdx 单位 = 位（bps<8 的打包样本与 8/16/32 的整字节统一）；整字节族先 >>3
    switch (bps) {
        case 8: {
            const size_t byte = bitIdx >> 3;
            return base[byte];
        }
        case 16: {
            const size_t byte = bitIdx >> 3;
            const uint16_t v = le ? static_cast<uint16_t>(base[byte]) |
                                        static_cast<uint16_t>(base[byte + 1] << 8)
                                  : static_cast<uint16_t>((base[byte] << 8) | base[byte + 1]);
            return v;
        }
        case 32: {
            const size_t byte = bitIdx >> 3;
            float f;
            if (le) {
                uint32_t bits = static_cast<uint32_t>(base[byte]) |
                                (static_cast<uint32_t>(base[byte + 1]) << 8) |
                                (static_cast<uint32_t>(base[byte + 2]) << 16) |
                                (static_cast<uint32_t>(base[byte + 3]) << 24);
                std::memcpy(&f, &bits, 4);
            } else {
                uint32_t bits = (static_cast<uint32_t>(base[byte]) << 24) |
                                (static_cast<uint32_t>(base[byte + 1]) << 16) |
                                (static_cast<uint32_t>(base[byte + 2]) << 8) |
                                static_cast<uint32_t>(base[byte + 3]);
                std::memcpy(&f, &bits, 4);
            }
            return f;
        }
        default: {
            const size_t byte = bitIdx >> 3;
            const int bit = static_cast<int>(bitIdx & 7);
            const int shift = 8 - static_cast<int>(bps) - bit;
            const uint8_t mask = static_cast<uint8_t>((1u << bps) - 1u);
            return (base[byte] >> shift) & mask;
        }
    }
}

// 样本值 → 8bit 通道（1..8bit 线性展宽；16bit 除 257 取整；32f 视作线性光 → sRGB）
inline uint8_t toChannel8(double v, uint64_t bps) {
    switch (bps) {
        case 32: {
            const float f = static_cast<float>(v);
            if (!(f > 0.0f)) return 0;
            if (f >= 1.0f) return 255;
            return linearToSrgb8(f);
        }
        case 16:
            return static_cast<uint8_t>(
                std::min(255.0, std::max(0.0, (v * 255.0 + 32767.0) / 65535.0)));
        default: {
            const double maxV = static_cast<double>((1u << bps) - 1u);
            return static_cast<uint8_t>(std::min(255.0, std::max(0.0, v * 255.0 / maxV)));
        }
    }
}

}  // namespace

bool tiffDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 8) {
        err = "tiff: truncated header";
        return false;
    }
    Reader r;
    r.d = data;
    r.n = len;
    if (std::memcmp(data, "II", 2) == 0) {
        r.le = true;
    } else if (std::memcmp(data, "MM", 2) == 0) {
        r.le = false;
    } else {
        err = "tiff: bad byte order";
        return false;
    }
    const uint16_t magic = r.u16(2);
    const bool le = r.le;  // lambda 捕获用（瓦片循环 r 遮蔽 Reader 名）
    if (magic == 42) {
        r.big = false;
    } else if (magic == 43) {
        r.big = true;
        if (r.u16(4) != 8) {
            err = "tiff: bad bigtiff offset size";
            return false;
        }
    } else {
        err = "tiff: bad magic";
        return false;
    }

    // IFD 定位：优先含图像数据的 IFD0；否则下沉 SubIFDs 取最大
    // BigTIFF 的 IFD 偏移在 @8（@4-5=offsetSize u16、@6-7=zero）；经典 TIFF 在 @4 u32
    size_t ifdPos = static_cast<size_t>(r.big ? r.u64(8) : r.u32(4));
    std::vector<size_t> chain;
    for (int guard = 0; guard < 8 && ifdPos >= 8 && ifdPos + 2 <= len; ++guard) {
        chain.push_back(ifdPos);
        if (r.big) {
            const uint64_t nEntries = r.u64(ifdPos);
            ifdPos = static_cast<size_t>(r.u64(ifdPos + 8 + nEntries * 20));
        } else {
            const uint16_t nEntries = r.u16(ifdPos);
            ifdPos = r.u32(ifdPos + 2 + nEntries * 12);
        }
    }
    auto findTag = [&](size_t pos, uint16_t tag) -> int {
        const uint64_t nEntries = r.big ? r.u64(pos) : r.u16(pos);  // 经典=u16
        const size_t esize = r.big ? 20 : 12;
        const size_t ebase = pos + (r.big ? 8 : 2);  // BigTIFF nEntries 是 u64
        for (uint64_t i = 0; i < nEntries; ++i) {
            if (r.u16(ebase + static_cast<size_t>(i) * esize) == tag) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    size_t imgIfd = 0;
    for (size_t cand : chain) {
        if (findTag(cand, 273) >= 0 || findTag(cand, 324) >= 0) {
            imgIfd = cand;
            break;
        }
        const int sub = findTag(cand, 330);  // SubIFDs
        if (sub >= 0) {
            const auto subs = entryList(r, cand, static_cast<size_t>(sub));
            uint64_t best = 0;
            for (uint64_t s : subs) {
                const size_t sp = static_cast<size_t>(s);
                if (sp >= 8 && sp < len) {
                    const uint64_t w = entryScalar(r, sp, 256, 0);
                    const uint64_t h = entryScalar(r, sp, 257, 0);
                    if (w * h > best && (entryScalar(r, sp, 273, 0) != 0 ||
                                         entryScalar(r, sp, 324, 0) != 0)) {
                        best = w * h;
                        imgIfd = sp;
                    }
                }
            }
            if (imgIfd != 0) {
                break;
            }
        }
    }
    if (imgIfd == 0) {
        err = "tiff: no image IFD";
        return false;
    }

    const uint64_t width = entryScalar(r, imgIfd, 256, 0);
    const uint64_t height = entryScalar(r, imgIfd, 257, 0);
    const uint64_t compression = entryScalar(r, imgIfd, 259, 1);
    const uint64_t photometric = entryScalar(r, imgIfd, 262, 1);
    const uint64_t spp = entryScalar(r, imgIfd, 277, 1);
    const uint64_t planar = entryScalar(r, imgIfd, 284, 1);
    const uint64_t predictor = entryScalar(r, imgIfd, 317, 1);
    const uint64_t rowsPerStrip = entryScalar(r, imgIfd, 278, 0xFFFFFFFFull);
    uint64_t bps = entryScalar(r, imgIfd, 258, 1);
    const auto bpsList = entryListByTag(r, imgIfd, 258);
    if (!bpsList.empty()) {
        bps = bpsList[0];
        for (uint64_t b : bpsList) {
            if (b != bps) {
                err = "tiff: mixed bits per sample unsupported";
                return false;
            }
        }
    }
    const auto sampleFormat = entryListByTag(r, imgIfd, 339);
    const auto extraSamples = entryListByTag(r, imgIfd, 338);
    const uint64_t tileW = entryScalar(r, imgIfd, 322, 0);
    const uint64_t tileH = entryScalar(r, imgIfd, 323, 0);
    const bool tiled = tileW != 0 && tileH != 0;
    const auto offsets = entryListByTag(r, imgIfd, tiled ? 324 : 273);
    const auto byteCounts = entryListByTag(r, imgIfd, tiled ? 325 : 279);

    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        width * height > kLimit) {
        err = "tiff: size out of limits";
        return false;
    }
    if (compression != 1 && compression != 5 && compression != 8 && compression != 32946 &&
        compression != 32773) {
        err = "tiff: unsupported compression (need none/LZW/PackBits/Deflate)";
        return false;
    }
    if (photometric != 0 && photometric != 1 && photometric != 2 && photometric != 3) {
        err = "tiff: unsupported photometric (need WhiteIsZero/BlackIsZero/RGB/Palette)";
        return false;
    }
    if (bps != 1 && bps != 2 && bps != 4 && bps != 8 && bps != 16 && bps != 32) {
        err = "tiff: unsupported bits per sample";
        return false;
    }
    if (bps == 32) {
        bool isFloat = false;
        for (uint64_t sf : sampleFormat) {
            if (sf == 3) isFloat = true;
        }
        if (!isFloat) {
            err = "tiff: 32-bit int unsupported (float only)";
            return false;
        }
    }
    if (spp < 1 || spp > 5) {
        err = "tiff: unsupported samples per pixel";
        return false;
    }
    if (photometric == 3 && bps > 8) {
        err = "tiff: palette >8bit unsupported";
        return false;
    }
    if (predictor != 1 && predictor != 2) {
        err = "tiff: unsupported predictor";
        return false;
    }

    // 调色板（u16 × 3×2^bps；高位字节；palEntries = 2^bps 三段每段项数）
    std::vector<uint8_t> palette;
    size_t palEntries = 0;
    if (photometric == 3) {
        const int palIdx = findTag(imgIfd, 320);
        if (palIdx < 0) {
            err = "tiff: palette missing";
            return false;
        }
        uint16_t ptype = 0;
        uint64_t pcount = 0;
        size_t poff = 0;
        if (!entryValue(r, imgIfd, static_cast<size_t>(palIdx), poff, pcount, ptype) ||
            pcount < 3u * (1u << bps)) {
            err = "tiff: palette truncated";
            return false;
        }
        const size_t entries = 1u << bps;
        palette.resize(entries * 3);
        palEntries = entries;
        for (size_t i = 0; i < entries * 3; ++i) {
            palette[i] = static_cast<uint8_t>(r.u16(poff + i * 2) >> 8);
        }
    }

    // alpha 语义：extraSamples[0]==1 → 预乘（还原）；==2/0 或缺省 → 直
    bool alphaPremul = false;
    bool hasAlpha = false;
    if (!extraSamples.empty()) {
        hasAlpha = extraSamples[0] == 1 || extraSamples[0] == 2;
        alphaPremul = extraSamples[0] == 1;
    } else if (spp == 4 && photometric == 2) {
        hasAlpha = true;  // 4 采样 RGB 默认带 alpha（直）
    } else if (spp == 2 && photometric != 2) {
        hasAlpha = true;
    }

    // 行展开：planar 条带/瓦片 = 单平面行；chunky = 全样本行。块尺寸一律 planeRowBytes×行数。
    const size_t bitsPerRow = static_cast<size_t>(width) * (planar == 2 ? 1 : spp) * bps;
    const size_t planeRowBytes = (bitsPerRow + 7) / 8;
    out.w = static_cast<uint32_t>(width);
    out.h = static_cast<uint32_t>(height);
    out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    std::vector<uint8_t> scratch;

    auto decompressBlock = [&](size_t blockIdx, uint8_t* dst, size_t expectBytes) -> bool {
        if (blockIdx >= offsets.size() || blockIdx >= byteCounts.size()) {
            err = "tiff: strip/tile index out of range";
            return false;
        }
        const uint64_t off = offsets[blockIdx];
        const uint64_t cnt = byteCounts[blockIdx];
        if (off + cnt > len) {
            err = "tiff: block data truncated";
            return false;
        }
        if (compression == 1) {
            if (cnt < expectBytes) {
                err = "tiff: uncompressed block short";
                return false;
            }
            std::memcpy(dst, data + off, expectBytes);
            return true;
        }
        scratch.clear();
        if (compression == 5) {
            if (!tiffLzw(data + off, cnt, scratch, err)) {
                return false;
            }
        } else if (compression == 8 || compression == 32946) {
            if (!tiffInflate(data + off, cnt, scratch, expectBytes, err)) {
                return false;
            }
        } else {  // 32773 PackBits
            scratch.resize(expectBytes, 0);
            if (!tiffPackbits(data + off, cnt, scratch.data(), expectBytes)) {
                err = "tiff: packbits decode failed";
                return false;
            }
        }
        if (scratch.size() < expectBytes) {
            scratch.resize(expectBytes, 0);  // 尾部短补零（宽容）
        }
        std::memcpy(dst, scratch.data(), expectBytes);
        return true;
    };

    auto emitRow = [&](const uint8_t* rowBase, uint32_t y, uint32_t plane) {
        // rowBase = 该行字节（planar 时即单平面行；plane 仅记录日志用）
        (void)plane;
        uint8_t* drow = out.rgba.data() + static_cast<size_t>(y) * width * 4;
        auto sampleAt = [&](uint32_t x, uint32_t s) -> double {
            if (planar == 2) {
                const size_t bitIdx = static_cast<size_t>(x) * bps;
                return unpackSample(rowBase, bitIdx, bps, le);
            }
            const size_t bitIdx = (static_cast<size_t>(x) * spp + s) * bps;
            return unpackSample(rowBase, bitIdx, bps, le);
        };
        for (uint32_t x = 0; x < width; ++x) {
            double v[4] = {0, 0, 0, 255};
            if (photometric == 2) {  // RGB
                v[0] = sampleAt(x, 0);
                v[1] = sampleAt(x, 1);
                v[2] = sampleAt(x, spp > 2 ? 2 : 0);
                if (hasAlpha && spp >= 4) {
                    v[3] = sampleAt(x, 3);
                }
            } else if (photometric == 3) {  // palette（ColorMap = R|G|B 三段连续，各 2^bps 项）
                const size_t idx = static_cast<size_t>(sampleAt(x, 0));
                if (idx < palEntries) {
                    v[0] = palette[idx];
                    v[1] = palette[palEntries + idx];
                    v[2] = palette[palEntries * 2 + idx];
                }
            } else {  // gray
                const double g = sampleAt(x, 0);
                v[0] = v[1] = v[2] = g;
                if (hasAlpha && spp >= 2) {
                    v[3] = sampleAt(x, 1);
                }
            }
            // palette 值域 = 8bit（调色板已展宽），photometric 3 不再按 bps 缩放
            const uint64_t outBps = (photometric == 3) ? 8u : bps;
            drow[x * 4 + 0] = toChannel8(v[0], outBps);
            drow[x * 4 + 1] = toChannel8(v[1], outBps);
            drow[x * 4 + 2] = toChannel8(v[2], outBps);
            drow[x * 4 + 3] = toChannel8(v[3], outBps);
        }
    };

    if (!tiled) {
        const uint64_t rps = rowsPerStrip == 0 ? 1 : rowsPerStrip;
        const uint64_t stripsPerPlane = (height + rps - 1) / rps;
        for (uint64_t strip = 0; strip < offsets.size(); ++strip) {
            const uint32_t plane =
                planar == 2 ? static_cast<uint32_t>(strip / stripsPerPlane) : 0;
            const uint64_t rowInPlane = strip % stripsPerPlane;
            const uint32_t y0 = static_cast<uint32_t>(rowInPlane * rps);
            const uint32_t rows = static_cast<uint32_t>(
                std::min<uint64_t>(rps, height - y0));
            if (y0 >= height) {
                continue;
            }
            // planar/chunky 条带都只含本条带行 × 单行宽（planar 行=1 样本/像素）
            const size_t expect = planeRowBytes * rows;
            std::vector<uint8_t> block(expect);
            if (!decompressBlock(strip, block.data(), expect)) {
                return false;
            }
            const uint32_t predSamples = planar == 2 ? 1 : static_cast<uint32_t>(spp);
            for (uint32_t r = 0; r < rows; ++r) {
                uint8_t* rowBase = block.data() + static_cast<size_t>(r) * planeRowBytes;
                if (predictor == 2) {
                    undoPredictor(rowBase, planeRowBytes, predSamples, static_cast<int>(bps));
                }
                emitRow(rowBase, y0 + r, plane);
            }
        }
    } else {
        if (tileW == 0 || tileH == 0 || tileW > 16384 || tileH > 16384) {
            err = "tiff: bad tile size";
            return false;
        }
        const uint64_t tilesX = (width + tileW - 1) / tileW;
        const uint64_t tilesY = (height + tileH - 1) / tileH;
        const uint64_t tilesPerPlane = tilesX * tilesY;
        const size_t tileRowBytes = (static_cast<size_t>(tileW) * (planar == 2 ? 1 : spp) * bps + 7) / 8;
        const size_t tileBytes = tileRowBytes * tileH;
        for (uint64_t ty = 0; ty < tilesY; ++ty) {
            for (uint64_t tx = 0; tx < tilesX; ++tx) {
                for (uint32_t plane = 0; plane < (planar == 2 ? spp : 1); ++plane) {
                    const uint64_t idx =
                        (planar == 2 ? static_cast<uint64_t>(plane) * tilesPerPlane : 0) +
                        ty * tilesX + tx;
                    std::vector<uint8_t> block(tileBytes);
                    if (!decompressBlock(idx, block.data(), tileBytes)) {
                        return false;
                    }
                    const uint32_t yMax = static_cast<uint32_t>(
                        std::min<uint64_t>(tileH, height - ty * tileH));
                    const uint32_t xMax = static_cast<uint32_t>(
                        std::min<uint64_t>(tileW, width - tx * tileW));
                    const uint32_t predSamples = planar == 2 ? 1 : static_cast<uint32_t>(spp);
                    for (uint32_t r = 0; r < yMax; ++r) {
                        uint8_t* rowBase =
                            block.data() + static_cast<size_t>(r) * tileRowBytes;
                        if (predictor == 2) {
                            undoPredictor(rowBase, tileRowBytes, predSamples, static_cast<int>(bps));
                        }
                        // 瓦片行内联展开
                        const uint32_t y = static_cast<uint32_t>(ty * tileH + r);
                        uint8_t* drow = out.rgba.data() + static_cast<size_t>(y) * width * 4;
                        for (uint32_t x = 0; x < xMax; ++x) {
                            auto sampleAt = [&](uint32_t s) -> double {
                                if (planar == 2) {
                                    const size_t bitIdx = static_cast<size_t>(x) * bps;
                                    return unpackSample(rowBase, bitIdx, bps, le);
                                }
                                const size_t bitIdx = (static_cast<size_t>(x) * spp + s) * bps;
                                return unpackSample(rowBase, bitIdx, bps, le);
                            };
                            double v[4] = {0, 0, 0, 255};
                            if (photometric == 2) {
                                v[0] = sampleAt(0);
                                v[1] = sampleAt(1);
                                v[2] = sampleAt(spp > 2 ? 2 : 0);
                                if (hasAlpha && spp >= 4) {
                                    v[3] = sampleAt(3);
                                }
                            } else if (photometric == 3) {
                                const size_t idx = static_cast<size_t>(sampleAt(0));
                                if (idx < palEntries) {
                                    v[0] = palette[idx];
                                    v[1] = palette[palEntries + idx];
                                    v[2] = palette[palEntries * 2 + idx];
                                }
                            } else {
                                const double g = sampleAt(0);
                                v[0] = v[1] = v[2] = g;
                                if (hasAlpha && spp >= 2) {
                                    v[3] = sampleAt(1);
                                }
                            }
                            const uint32_t dx = static_cast<uint32_t>(tx * tileW + x);
                            const uint64_t outBpsT = (photometric == 3) ? 8u : bps;
                            drow[dx * 4 + 0] = toChannel8(v[0], outBpsT);
                            drow[dx * 4 + 1] = toChannel8(v[1], outBpsT);
                            drow[dx * 4 + 2] = toChannel8(v[2], outBpsT);
                            drow[dx * 4 + 3] = toChannel8(v[3], outBpsT);
                        }
                    }
                }
            }
        }
    }

    // 预乘还原（ExtraSamples=1；banded 精度舍入与引擎一致）
    if (alphaPremul) {
        for (size_t i = 0; i < out.rgba.size(); i += 4) {
            const uint8_t a = out.rgba[i + 3];
            if (a != 255 && a != 0) {
                for (int c = 0; c < 3; ++c) {
                    const uint32_t v = out.rgba[i + c];
                    out.rgba[i + c] = static_cast<uint8_t>(
                        std::min(255u, (v * 255u + a / 2u) / a));
                }
            }
        }
    }
    // WhiteIsZero 灰度反转
    if (photometric == 0) {
        for (size_t i = 0; i < out.rgba.size(); i += 4) {
            out.rgba[i] = 255 - out.rgba[i];
            out.rgba[i + 1] = 255 - out.rgba[i + 1];
            out.rgba[i + 2] = 255 - out.rgba[i + 2];
        }
    }
    return true;
}

// P7：core = tiffEncode 主体 + 可选 ImageSourceData 附加 tag（37724，分层 TIFF）。
bool tiffEncodeCore(const uint8_t* rgba, uint32_t w, uint32_t h, int bitDepth, bool withAlpha,
                    const std::string& compression, const std::vector<uint8_t>* isd,
                    std::vector<uint8_t>& out, std::string& err) {
    if (rgba == nullptr || w == 0 || h == 0 ||
        static_cast<uint64_t>(w) * h > kLimit) {
        err = "tiff encode: bad input";
        return false;
    }
    if (bitDepth != 8 && bitDepth != 16) {
        err = "tiff encode: bitDepth must be 8 or 16";
        return false;
    }
    const bool doAlpha = withAlpha;
    const int spp = doAlpha ? 4 : 3;
    const bool useDeflate = compression == "deflate";
    const bool useLzw = compression == "lzw";
    const int compTag = useDeflate ? 8 : (useLzw ? 5 : 1);
    const int bytesPer = bitDepth / 8;
    const size_t rowBytes = static_cast<size_t>(w) * spp * bytesPer;

    // 条带化（~256KB/strip）+ 压缩
    uint32_t rps = static_cast<uint32_t>(std::max<size_t>(1, 262144 / std::max<size_t>(1, rowBytes)));
    if (rps > h) rps = h;
    const uint32_t strips = (h + rps - 1) / rps;
    std::vector<std::vector<uint8_t>> stripData(strips);
    std::vector<uint16_t> pixels16;
    if (bitDepth == 16) {
        pixels16.resize(rowBytes / 2);
    }
    std::vector<uint8_t> raw(rowBytes);
    for (uint32_t s = 0; s < strips; ++s) {
        const uint32_t y0 = s * rps;
        const uint32_t rows = std::min(rps, h - y0);
        raw.assign(rowBytes * rows, 0);
        for (uint32_t r = 0; r < rows; ++r) {
            const uint8_t* src = rgba + static_cast<size_t>(y0 + r) * w * 4;
            uint8_t* dst = raw.data() + static_cast<size_t>(r) * rowBytes;
            if (bitDepth == 8) {
                for (uint32_t x = 0; x < w; ++x) {
                    dst[x * spp + 0] = src[x * 4 + 0];
                    dst[x * spp + 1] = src[x * 4 + 1];
                    dst[x * spp + 2] = src[x * 4 + 2];
                    if (doAlpha) dst[x * spp + 3] = src[x * 4 + 3];
                }
            } else {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint16_t r16 = static_cast<uint16_t>(src[x * 4 + 0] * 257);
                    const uint16_t g16 = static_cast<uint16_t>(src[x * 4 + 1] * 257);
                    const uint16_t b16 = static_cast<uint16_t>(src[x * 4 + 2] * 257);
                    const size_t o = x * spp;
                    pixels16[o + 0] = r16;
                    pixels16[o + 1] = g16;
                    pixels16[o + 2] = b16;
                    if (doAlpha) pixels16[o + 3] = static_cast<uint16_t>(src[x * 4 + 3] * 257);
                }
                std::memcpy(dst, pixels16.data(), rowBytes);
            }
        }
        if (useDeflate) {
            uLongf bound = compressBound(static_cast<uLong>(raw.size()));
            std::vector<uint8_t> comp(bound + 16);
            uLongf clen = bound;
            if (compress2(comp.data(), &clen, raw.data(), static_cast<uLong>(raw.size()),
                          Z_DEFAULT_COMPRESSION) != Z_OK) {
                err = "tiff encode: compress failed";
                return false;
            }
            comp.resize(clen);
            stripData[s] = std::move(comp);
        } else {
            stripData[s] = raw;  // none（lzw 写出 v1 不做——读全解、写选 none/deflate）
        }
    }
    if (useLzw) {
        err = "tiff encode: lzw write unsupported (use none/deflate)";
        return false;
    }

    // 布局：头 8 + 像素数据 + IFD + 外置值（+ ImageSourceData）
    size_t stripTotal = 0;
    for (const auto& s : stripData) stripTotal += s.size();
    const bool hasIsd = isd != nullptr && !isd->empty();
    const size_t ifdPos = 8 + stripTotal;
    // 条目数：基础 12（含 296/339）+ alpha extraSamples + ISD（entry 调用数恒定，与 strips 无关）
    const int nEntries = (doAlpha ? 13 : 12) + (hasIsd ? 1 : 0);
    const size_t ifdBytes = 2 + static_cast<size_t>(nEntries) * 12 + 4;
    const size_t extPos = ifdPos + ifdBytes;
    // 外置值区：bps 数组(若 alpha 4×SHORT=8B>4) + stripOffsets(4×strips) + stripByteCounts
    const bool bpsExt = doAlpha;  // 4 项 SHORT = 8B
    const size_t bpsOff = bpsExt ? extPos : 0;
    const size_t soOff = extPos + (bpsExt ? 8 : 0);
    const size_t bcOff = soOff + static_cast<size_t>(strips) * 4;
    const size_t isdOff = bcOff + static_cast<size_t>(strips) * 4;
    const size_t total = isdOff + (hasIsd ? isd->size() : 0);

    out.clear();
    out.resize(total, 0);
    auto w8 = [&](size_t p, uint8_t v) { out[p] = v; };
    auto w16 = [&](size_t p, uint16_t v) {
        out[p] = static_cast<uint8_t>(v);
        out[p + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto w32 = [&](size_t p, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out[p + i] = static_cast<uint8_t>(v >> (i * 8));
        }
    };
    w8(0, 'I'); w8(1, 'I'); w16(2, 42); w32(4, static_cast<uint32_t>(ifdPos));

    // 像素数据（单 strip 时 273/279 的 LONG count=1 内联 value 字段——写真值非外置地址）
    size_t po = 8;
    for (uint32_t s = 0; s < strips; ++s) {
        std::memcpy(out.data() + po, stripData[s].data(), stripData[s].size());
        w32(soOff + static_cast<size_t>(s) * 4, static_cast<uint32_t>(po));
        w32(bcOff + static_cast<size_t>(s) * 4, static_cast<uint32_t>(stripData[s].size()));
        po += stripData[s].size();
    }
    const uint32_t strip0Off = 8;
    const uint32_t strip0Cnt = static_cast<uint32_t>(stripData.empty() ? 0 : stripData[0].size());

    // IFD（tag 升序）
    w16(ifdPos, static_cast<uint16_t>(nEntries));
    size_t e = ifdPos + 2;
    auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
        w16(e, tag); w16(e + 2, type); w32(e + 4, count); w32(e + 8, value);
        e += 12;
    };
    entry(256, 4, 1, w);
    entry(257, 4, 1, h);
    if (bpsExt) {
        w16(bpsOff, static_cast<uint16_t>(bitDepth));
        w16(bpsOff + 2, static_cast<uint16_t>(bitDepth));
        w16(bpsOff + 4, static_cast<uint16_t>(bitDepth));
        w16(bpsOff + 6, static_cast<uint16_t>(bitDepth));
        entry(258, 3, spp, static_cast<uint32_t>(bpsOff));
    } else {
        entry(258, 3, 1, static_cast<uint32_t>(bitDepth));
    }
    entry(259, 3, 1, static_cast<uint32_t>(compTag));
    entry(262, 3, 1, 2);  // RGB
    if (strips == 1) {
        entry(273, 4, 1, strip0Off);   // 单条：LONG 4B 内联真值
        entry(277, 3, 1, static_cast<uint32_t>(spp));
        entry(278, 4, 1, rps);
        entry(279, 4, 1, strip0Cnt);
    } else {
        entry(273, 4, strips, static_cast<uint32_t>(soOff));
        entry(277, 3, 1, static_cast<uint32_t>(spp));
        entry(278, 4, 1, rps);
        entry(279, 4, strips, static_cast<uint32_t>(bcOff));
    }
    entry(284, 3, 1, 1);  // chunky
    entry(296, 3, 1, 2);  // inch
    if (doAlpha) {
        entry(338, 3, 1, 2);  // unassociated alpha
    }
    entry(339, 3, 1, 1);  // sample format uint
    if (hasIsd) {
        // P7：ImageSourceData（37724，type UNDEFINED）——外置数据在 ext 值区之后
        std::memcpy(out.data() + isdOff, isd->data(), isd->size());
        entry(37724, 7, static_cast<uint32_t>(isd->size()), static_cast<uint32_t>(isdOff));
    }
    w32(e, 0);  // 下一个 IFD = 无
    return true;
}

bool tiffEncode(const uint8_t* rgba, uint32_t w, uint32_t h, int bitDepth, bool withAlpha,
                const std::string& compression, std::vector<uint8_t>& out, std::string& err) {
    return tiffEncodeCore(rgba, w, h, bitDepth, withAlpha, compression, nullptr, out, err);
}

// ---------- P7：分层 TIFF 写出（合成图 + 37724 ImageSourceData/Layr） ----------

bool tiffEncodeLayers(const PsdDocSnap& snap, const std::string& compression,
                      std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (snap.width == 0 || snap.height == 0) {
        err = "tiff layers: empty document";
        return false;
    }
    if (snap.width > 30000 || snap.height > 30000) {
        // PSD v1 层记录矩形域上限；经典 TIFF 合成图 >4GB 同因（BigTIFF 写出 v1 不做）
        err = "tiff layers: document exceeds 30000px (use PSD/PSB)";
        return false;
    }
    // 1) 层信息段提取：exportPsdToMemory（v1 布局，尺寸上限已保证不触发 PSB）→
    //    Layer Info content = [count i16][records+channels] → Layr = [count u32][records+channels]
    PsdDocSnap psdSnap = snap;
    psdSnap.psb = false;
    psdSnap.zipComposite = false;
    std::vector<uint8_t> psdBytes;
    const PsdExportResult pr = exportPsdToMemory(psdSnap, psdBytes);
    if (!pr.ok || psdBytes.size() < 30) {
        err = pr.error.empty() ? "tiff layers: psd layer encode failed" : pr.error;
        return false;
    }
    auto be32 = [&](size_t p) -> uint32_t {
        if (p + 4 > psdBytes.size()) return 0;
        return (static_cast<uint32_t>(psdBytes[p]) << 24) |
               (static_cast<uint32_t>(psdBytes[p + 1]) << 16) |
               (static_cast<uint32_t>(psdBytes[p + 2]) << 8) | static_cast<uint32_t>(psdBytes[p + 3]);
    };
    size_t p = 26;  // sig4+ver2+res6+ch2+h4+w4+depth2+mode2
    const uint32_t cmdLen = be32(p);
    p += 4 + cmdLen;
    const uint32_t resLen = be32(p);
    p += 4 + resLen;
    if (p + 8 > psdBytes.size()) {
        err = "tiff layers: psd layout truncated";
        return false;
    }
    const size_t liPos = p + 4;  // lmLen 字段后 = Layer Info 长度字段
    const uint32_t liLen = be32(liPos);
    if (liLen < 2 || liPos + 4 + liLen > psdBytes.size()) {
        err = "tiff layers: layer info truncated";
        return false;
    }
    const uint8_t* content = psdBytes.data() + liPos + 4;
    const uint32_t layerCount = static_cast<uint32_t>(static_cast<uint16_t>(
        (content[0] << 8) | content[1]));
    if (layerCount == 0) {
        err = "tiff layers: no layers";
        return false;
    }
    // Layr 载荷 = [count u32 BE][records+channels]（buildSyntheticPsd 读取契约同构）
    std::vector<uint8_t> layr;
    layr.reserve(static_cast<size_t>(liLen) + 2);
    layr.push_back(static_cast<uint8_t>(layerCount >> 24));
    layr.push_back(static_cast<uint8_t>(layerCount >> 16));
    layr.push_back(static_cast<uint8_t>(layerCount >> 8));
    layr.push_back(static_cast<uint8_t>(layerCount));
    layr.insert(layr.end(), content + 2, content + liLen);

    // 2) ImageSourceData = "Photoshop 3.0\0"（对齐 4）+ 8BIM Layr 块（偶对齐）
    std::vector<uint8_t> isd;
    isd.reserve(16 + 12 + layr.size() + 1);
    const char sig[14] = {'P', 'h', 'o', 't', 'o', 's', 'h', 'o', 'p', ' ', '3', '.', '0', '\0'};
    isd.insert(isd.end(), sig, sig + 14);
    while (isd.size() % 4 != 0) {
        isd.push_back(0);
    }
    isd.insert(isd.end(), {'8', 'B', 'I', 'M', 'L', 'a', 'y', 'r'});
    isd.push_back(static_cast<uint8_t>(layr.size() >> 24));
    isd.push_back(static_cast<uint8_t>(layr.size() >> 16));
    isd.push_back(static_cast<uint8_t>(layr.size() >> 8));
    isd.push_back(static_cast<uint8_t>(layr.size()));
    isd.insert(isd.end(), layr.begin(), layr.end());
    if (layr.size() % 2 == 1) {
        isd.push_back(0);
    }

    // 3) 合成图（快照未携带时现场扁平化）+ 8-bit RGBA core 写出
    std::vector<uint8_t> flat = flattenSnapComposite(snap);
    return tiffEncodeCore(flat.data(), snap.width, snap.height, 8, true, compression, &isd, out, err);
}

// ---------- Photoshop 分层 tag（37724 ImageSourceData → 合成 PSD 容器喂 psd_reader） ----------

namespace {

auto w32beLambda = [](std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
};
auto w16beLambda = [](std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
};

// ImageSourceData = "Photoshop 3.0\0"（对齐 4）+ 8BIM/8B64 块（key4 + size + data）。
// 字节序约定（photocraft psd/tiff.rs block_order 对拍）：8BIM/8B64 = 大端（Photoshop 恒大端）；
// MIB8/46B8 = 小端变体（v1 不支持——notes 提示走合成图）。Layr 内容 = PSD layer info
// 同构（大端）：u32 层计数（Lr16 为 u64）+ 层记录 + 通道数据。
// 合成最小 PSD：头(26) + 色彩模式(4) + 图像资源(4) + 层与蒙版段（层信息 + 全局蒙版 u32 0）。
bool buildSyntheticPsd(const uint8_t* layr, size_t len, bool psb, std::vector<uint8_t>& out,
                       uint32_t w, uint32_t h, std::string& err) {
    size_t p = 0;
    uint64_t layerCount = 0;
    if (psb) {
        if (len < 8) { err = "tiff layers: Lr16 truncated"; return false; }
        layerCount = (static_cast<uint64_t>(layr[0]) << 56) | (static_cast<uint64_t>(layr[1]) << 48) |
                     (static_cast<uint64_t>(layr[2]) << 40) | (static_cast<uint64_t>(layr[3]) << 32) |
                     (static_cast<uint64_t>(layr[4]) << 24) | (static_cast<uint64_t>(layr[5]) << 16) |
                     (static_cast<uint64_t>(layr[6]) << 8) | static_cast<uint64_t>(layr[7]);
        p = 8;
    } else {
        if (len < 4) { err = "tiff layers: Layr truncated"; return false; }
        layerCount = (static_cast<uint64_t>(layr[0]) << 24) | (static_cast<uint64_t>(layr[1]) << 16) |
                     (static_cast<uint64_t>(layr[2]) << 8) | static_cast<uint64_t>(layr[3]);
        p = 4;
    }
    if (layerCount == 0 || layerCount > 16384) {
        err = "tiff layers: bad layer count";
        return false;
    }
    const size_t recordsLen = len - p;
    // 层信息段：u32 长度 + i16 计数 + 记录/通道数据 + 偶对齐垫
    const size_t liPayload = 2 + recordsLen + (recordsLen & 1);
    const size_t liLen = 4 + liPayload;
    const size_t lmLen = liLen + 4;  // + 全局蒙版信息 u32 0

    out.clear();
    out.reserve(38 + lmLen);
    // 文件头
    out.insert(out.end(), {'8', 'B', 'P', 'S'});
    // P7 修复：版本为 u16——旧代码只 push 1 字节（头整体错位 1B → version 读成
    // 0x0100=256 恒拒），分层 TIFF 导入从未真正通过、恒被合成图回退掩盖。
    out.push_back(0);
    out.push_back(psb ? 2 : 1);  // 版本（PSB=2）
    for (int i = 0; i < 6; ++i) out.push_back(0);
    out.push_back(0); out.push_back(3);  // channels=3（大端）
    w32beLambda(out, h);
    w32beLambda(out, w);
    w16beLambda(out, 8);   // depth
    w16beLambda(out, 3);   // RGB
    w32beLambda(out, 0);   // 色彩模式段
    w32beLambda(out, 0);   // 图像资源段
    w32beLambda(out, static_cast<uint32_t>(lmLen));
    // 层信息段
    w32beLambda(out, static_cast<uint32_t>(liLen));
    w16beLambda(out, static_cast<uint16_t>(layerCount & 0xFFFF));
    out.insert(out.end(), layr + p, layr + len);
    if (recordsLen & 1) out.push_back(0);
    w32beLambda(out, 0);  // 全局层蒙版信息：长度 0
    return true;
}

}  // namespace

bool tiffHasLayerData(const uint8_t* data, size_t len) {
    if (len < 8) return false;
    Reader r;
    r.d = data;
    r.n = len;
    if (std::memcmp(data, "II", 2) == 0) r.le = true;
    else if (std::memcmp(data, "MM", 2) == 0) r.le = false;
    else return false;
    r.big = r.u16(2) == 43;
    // BigTIFF 的 IFD 偏移在 @8（@4-5=offsetSize u16、@6-7=zero）；经典 TIFF 在 @4 u32
    size_t ifdPos = static_cast<size_t>(r.big ? r.u64(8) : r.u32(4));
    for (int guard = 0; guard < 4 && ifdPos >= 8 && ifdPos + 2 <= len; ++guard) {
        const uint64_t nEntries = r.big ? r.u64(ifdPos) : r.u16(ifdPos);  // 经典=u16
        const size_t esize = r.big ? 20 : 12;
        for (uint64_t i = 0; i < nEntries; ++i) {
            const size_t ent = ifdPos + 2 + static_cast<size_t>(i) * esize;
            if (r.u16(ent) == 37724) {
                uint64_t count = 0;
                size_t off = 0;
                uint16_t type = 0;
                if (!entryValue(r, ifdPos, static_cast<size_t>(i), off, count, type)) {
                    return false;
                }
                static const int kSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8, 8, 0, 8, 8, 8};
                const uint64_t bytes = kSize[type] * count;
                if (off + bytes > len) return false;
                const uint8_t* v = data + off;
                for (uint64_t b = 0; b + 8 <= bytes; ++b) {
                    if (std::memcmp(v + b, "8BIM", 4) == 0 || std::memcmp(v + b, "8B64", 4) == 0) {
                        const char* key = reinterpret_cast<const char*>(v + b + 4);
                        if (std::memcmp(key, "Layr", 4) == 0 || std::memcmp(key, "Lr16", 4) == 0 ||
                            std::memcmp(key, "Lr32", 4) == 0) {
                            return true;
                        }
                    }
                }
                return false;
            }
        }
        ifdPos = r.big ? static_cast<size_t>(r.u64(ifdPos + 8 + nEntries * esize))
                       : r.u32(ifdPos + 2 + nEntries * esize);
    }
    return false;
}

bool tiffDecodeLayers(const uint8_t* data, size_t len, uint32_t& w, uint32_t& h,
                      std::vector<CodecLayerRec>& layers, std::vector<std::string>& notes,
                      std::string& err, psd::PsdDocument* pdocOut) {
    // 1) 主 IFD 尺寸（文档域）
    if (len < 8) { err = "tiff: truncated"; return false; }
    Reader r;
    r.d = data;
    r.n = len;
    r.le = std::memcmp(data, "II", 2) == 0;
    r.big = r.u16(2) == 43;
    // BigTIFF 的 IFD 偏移在 @8（@4-5=offsetSize u16、@6-7=zero）；经典 TIFF 在 @4 u32
    size_t ifdPos = static_cast<size_t>(r.big ? r.u64(8) : r.u32(4));
    if (ifdPos < 8 || ifdPos + 2 > len) { err = "tiff: bad IFD"; return false; }
    auto scalar = [&](uint16_t tag, uint64_t def) -> uint64_t {
        const uint64_t nEntries = r.big ? r.u64(ifdPos) : r.u16(ifdPos);  // 经典=u16
        const size_t esize = r.big ? 20 : 12;
        for (uint64_t i = 0; i < nEntries; ++i) {
            const size_t ent = ifdPos + 2 + static_cast<size_t>(i) * esize;
            if (r.u16(ent) == tag) {
                const auto v = entryList(r, ifdPos, static_cast<size_t>(i));
                return v.empty() ? def : v[0];
            }
        }
        return def;
    };
    w = static_cast<uint32_t>(scalar(256, 0));
    h = static_cast<uint32_t>(scalar(257, 0));
    if (w == 0 || h == 0 || static_cast<uint64_t>(w) * h > kLimit) {
        err = "tiff layers: size out of limits";
        return false;
    }

    // 2) 取 ImageSourceData
    std::vector<uint8_t> isd;
    {
        const uint64_t nEntries = r.big ? r.u64(ifdPos) : r.u16(ifdPos);  // 经典=u16
        const size_t esize = r.big ? 20 : 12;
        bool found = false;
        for (uint64_t i = 0; i < nEntries && !found; ++i) {
            const size_t ent = ifdPos + 2 + static_cast<size_t>(i) * esize;
            if (r.u16(ent) != 37724) continue;
            size_t off = 0;
            uint64_t count = 0;
            uint16_t type = 0;
            if (!entryValue(r, ifdPos, static_cast<size_t>(i), off, count, type)) {
                err = "tiff layers: ISD out of range";
                return false;
            }
            static const int kSize[] = {0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 8, 8, 0, 8, 8, 8};
            const uint64_t bytes = kSize[type] * count;
            if (off + bytes > len) { err = "tiff layers: ISD truncated"; return false; }
            isd.assign(data + off, data + off + bytes);
            found = true;
        }
        if (!found) { err = "no layers"; return false; }
    }

    // 3) 解析 8BIM 块（Photoshop 3.0\0 头之后）
    size_t pos = 14;
    if (isd.size() < 16 || std::memcmp(isd.data(), "Photoshop 3.0\0", 14) != 0) {
        err = "no layers";
        return false;
    }
    pos = (pos + 3) & ~static_cast<size_t>(3);  // 对齐 4
    const uint8_t* layr = nullptr;
    size_t layrLen = 0;
    bool psb = false;
    while (pos + 8 <= isd.size()) {
        const bool b64 = std::memcmp(isd.data() + pos, "8B64", 4) == 0;
        if (!b64 && std::memcmp(isd.data() + pos, "8BIM", 4) != 0) {
            if (std::memcmp(isd.data() + pos, "MIB8", 4) == 0 ||
                std::memcmp(isd.data() + pos, "46B8", 4) == 0) {
                notes.push_back("小端字节序分层 TIFF 暂不支持，导入合成图");
            }
            break;
        }
        const char* key = reinterpret_cast<const char*>(isd.data() + pos + 4);
        uint64_t bsize;
        size_t dpos;
        if (b64) {
            bsize = 0;
            for (int i = 0; i < 8; ++i) {
                bsize = (bsize << 8) | isd[pos + 8 + i];
            }
            dpos = pos + 16;
        } else {
            bsize = (static_cast<uint64_t>(isd[pos + 8]) << 24) |
                    (static_cast<uint64_t>(isd[pos + 9]) << 16) |
                    (static_cast<uint64_t>(isd[pos + 10]) << 8) |
                    static_cast<uint64_t>(isd[pos + 11]);
            dpos = pos + 12;
        }
        if (dpos + bsize > isd.size()) { err = "tiff layers: 8BIM truncated"; return false; }
        if (std::memcmp(key, "Layr", 4) == 0) {
            layr = isd.data() + dpos;
            layrLen = static_cast<size_t>(bsize);
            psb = false;
        } else if (std::memcmp(key, "Lr16", 4) == 0 || std::memcmp(key, "Lr32", 4) == 0) {
            layr = isd.data() + dpos;
            layrLen = static_cast<size_t>(bsize);
            psb = true;
        }
        pos = dpos + static_cast<size_t>(bsize);
        if ((bsize & 1) == 1) ++pos;  // 块偶对齐
    }
    if (layr == nullptr) { err = "no layers"; return false; }
    if (psb) {
        notes.push_back("16/32 位分层 TIFF 暂不支持，导入合成图");
    }

    // 4) 合成 PSD → readPsd → 层记录
    std::vector<uint8_t> psdBytes;
    if (!buildSyntheticPsd(layr, layrLen, psb, psdBytes, w, h, err)) {
        return false;
    }
    psd::PsdDocument pdoc;
    std::string perr;
    if (!psd::readPsd(psdBytes.data(), psdBytes.size(), pdoc, perr)) {
        notes.push_back("分层 TIFF 解析失败（" + perr + "），导入合成图");
        err = "no layers";
        return false;
    }
    if (pdocOut != nullptr) {
        *pdocOut = pdoc;
    }
    layers.clear();
    layers.reserve(pdoc.layers.size());
    for (const psd::Record& rec : pdoc.layers) {
        CodecLayerRec out;
        out.left = rec.left;
        out.top = rec.top;
        out.w = static_cast<uint32_t>(rec.width);
        out.h = static_cast<uint32_t>(rec.height);
        out.rgba = rec.rgba;  // 预乘（psd_reader 语义），导入端统一还原
        out.name = rec.name;
        out.isGroup = rec.isGroup;
        out.opacity = rec.opacity;
        out.blendKey = rec.blendKey;
        out.visible = rec.visible;
        layers.push_back(std::move(out));
    }
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
