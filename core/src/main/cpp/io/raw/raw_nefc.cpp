// M23a：Nikon Huffman 压缩 NEF 传感器数据（TIFF compression 34713）：
// "lossless compressed" 与 "lossy compressed"（type 1/2），12 与 14 位。
// 净室实现：格式事实来自公开描述（Clévy NEF 格式文档、Bill Claff NEF Compression、
// ExifTool 标签名文档、ITU-T T.81 Annex F/H），Huffman 表来自对 CC0 样本的黑盒
// 分析（photocraft 侧，~45 文件 ~35 机身验证）。只取格式事实与表。
// 翻译自 photocraft crates/raw/src/nefc.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace montage {
namespace raw {

namespace {

// 未在任何分析样本中出现过的码字。
constexpr uint8_t kUnseen = 0xFF;

// 规范 Huffman 表：每长度 1..=16 的码数与符号（差分类别）按码序。
// counts/symbols 是带长度的切片（上游 &'static [u8]）——长度必须随表携带，
// 各表长度不同（7–9 项），按 16 读会越界。
struct Table {
    const uint8_t* counts;
    size_t nCounts;
    const uint8_t* symbols;
    size_t nSymbols;
};

// LOSSLESS_12 / LOSSLESS_14 / LOSSY_12 / LOSSY_14（上游黑盒恢复的固定表）。
constexpr uint8_t kLossless12Counts[] = {0, 1, 4, 2, 3, 1, 2};
constexpr uint8_t kLossless12Symbols[] = {5, 4, 6, 3, 7, 2, 8, 1, 9, 0, 10, 11, 12};
constexpr uint8_t kLossless14Counts[] = {0, 1, 4, 2, 2, 3, 1, 2};
constexpr uint8_t kLossless14Symbols[] = {7, 6, 8, 5, 9, 4, 10, 3, 11, 12, 2, 0, 1, 13, 14};
constexpr uint8_t kLossy12Counts[] = {0, 1, 5, 1, 1, 1, 1, 1, 1};
constexpr uint8_t kLossy12Symbols[] = {5, 4, 3, 6, 2, 7, 1, 0, 8, 9, kUnseen, 10};
constexpr uint8_t kLossy14Counts[] = {0, 1, 4, 3, 1, 1, 1, 1, 1};
constexpr uint8_t kLossy14Symbols[] = {5, 6, 4, 7, 8, 3, 9, 2, 1, 0, 10, 11, 12};

constexpr Table kLossless12{kLossless12Counts, 7, kLossless12Symbols, 13};
constexpr Table kLossless14{kLossless14Counts, 8, kLossless14Symbols, 15};
constexpr Table kLossy12{kLossy12Counts, 9, kLossy12Symbols, 12};
constexpr Table kLossy14{kLossy14Counts, 9, kLossy14Symbols, 13};

// 上表最长码字。
constexpr uint32_t kMaxLen = 9;

// Huffman 解码器：接下来 kMaxLen 位的查找表。
struct NefHuffman {
    // (category, code length)；length 0 = 无此码。
    std::vector<std::pair<uint8_t, uint8_t>> lut;

    explicit NefHuffman(const Table& t) {
        lut.assign(static_cast<size_t>(1) << kMaxLen, {0, 0});
        uint32_t code = 0;
        size_t si = 0;
        for (size_t i = 0; i < t.nCounts; ++i) {
            uint32_t len = static_cast<uint32_t>(i) + 1;
            uint8_t n = t.counts[i];
            if (len > kMaxLen && n > 0) throw RawError::malformed("NEF Huffman code longer than the lookup");
            for (uint8_t k = 0; k < n; ++k) {
                if (si >= t.nSymbols) throw RawError::malformed("NEF Huffman table is short of symbols");
                uint8_t s = t.symbols[si++];
                size_t first = static_cast<size_t>(code) << (kMaxLen - len);
                size_t span = static_cast<size_t>(1) << (kMaxLen - len);
                if (first + span > lut.size()) throw RawError::malformed("NEF Huffman table overflows");
                if (s != kUnseen) {
                    for (size_t e = first; e < first + span; ++e) lut[e] = {s, static_cast<uint8_t>(len)};
                }
                ++code;
            }
            code <<= 1;
        }
    }
};

// MSB-first 位读取器，无字节填塞。越界读返回零位；consumed 记录读了多远。
struct NefBits {
    const uint8_t* src = nullptr;
    size_t len = 0;
    size_t pos = 0;
    uint64_t acc = 0;
    uint32_t n = 0;
    size_t consumed = 0;

    inline void fill() {
        while (n <= 56) {
            uint8_t b = pos < len ? src[pos] : 0;
            ++pos;
            acc |= static_cast<uint64_t>(b) << (56 - n);
            n += 8;
        }
    }

    inline uint32_t peek(uint32_t k) {
        fill();
        return static_cast<uint32_t>(acc >> (64 - k));
    }

    inline void skip(uint32_t k) {
        acc <<= k;
        n -= k;
        consumed += k;
    }

    inline uint32_t take(uint32_t k) {
        if (k == 0) return 0;
        uint32_t v = peek(k);
        skip(k);
        return v;
    }
};

// 一个编码差分；表中无此码字返回空。
inline std::optional<int32_t> nefDiff(NefBits& b, const NefHuffman& h) {
    uint32_t pk = b.peek(kMaxLen);
    auto e = h.lut[pk];
    if (e.second == 0) return std::nullopt;
    b.skip(e.second);
    uint32_t cat = e.first;
    if (cat > 16) return std::nullopt;
    int32_t v = static_cast<int32_t>(b.take(cat));
    // T.81 F.1.2.1：前导 0 位表示负差分。
    if (cat > 0 && v < (1 << (cat - 1))) return v - (1 << cat) + 1;
    return v;
}

// 解码值如何映射为样本。
struct NefEncoding {
    bool lossless = true;
    std::vector<uint16_t> curve;  // lossy 时为索引曲线
};

// maker note 0x0096 解析结果。
struct NefDecodeTable {
    NefEncoding encoding;
    // 预测子种子 [行奇偶][列]。
    int32_t seeds[2][2] = {};
};

std::optional<uint16_t> noteU16(const uint8_t* t, size_t len, size_t o, bool le) {
    if (o > len || len - o < 2) return std::nullopt;
    return le ? static_cast<uint16_t>(t[o] | t[o + 1] << 8) : static_cast<uint16_t>(t[o] << 8 | t[o + 1]);
}

// 解析 maker note 0x0096（bits 位图像）。le 为 maker note 字节序。
NefDecodeTable parseNefTable(const uint8_t* t, size_t len, bool le, uint32_t bits) {
    if (bits != 12 && bits != 14) {
        throw RawError::unsupported("Nikon compressed NEF with " + std::to_string(bits) + "-bit samples");
    }
    if (len < 2) {
        throw RawError::malformed("NEF linearization table too short (" + std::to_string(len) + " bytes)");
    }
    uint8_t v0 = t[0];
    uint8_t v1 = t[1];
    NefDecodeTable out;
    for (int i = 0; i < 4; ++i) {
        auto s = noteU16(t, len, 2 + 2 * i, le);
        if (!s) throw RawError::malformed("NEF linearization table too short (" + std::to_string(len) + " bytes)");
        out.seeds[i / 2][i % 2] = static_cast<int32_t>(static_cast<int16_t>(*s));
    }
    if (v0 == 0x46) {
        out.encoding.lossless = true;
        return out;
    }
    if (v0 == 0x44 && (v1 == 0x10 || v1 == 0x20 || v1 == 0x40)) {
        auto nOpt = noteU16(t, len, 10, le);
        if (!nOpt) throw RawError::malformed("NEF linearization table too short (" + std::to_string(len) + " bytes)");
        size_t n = *nOpt;
        if (n < 2) throw RawError::malformed("NEF linearization curve of " + std::to_string(n) + " points");
        if (12 + 2 * n > len) throw RawError::malformed("NEF linearization table too short (" + std::to_string(len) + " bytes)");
        std::vector<uint16_t> points(n);
        for (size_t i = 0; i < n; ++i) {
            auto v = noteU16(t, len, 12 + 2 * i, le);
            if (!v) throw RawError::malformed("NEF linearization table too short");
            points[i] = *v;
        }
        if (v1 == 0x10) {
            out.encoding.lossless = false;
            out.encoding.curve = std::move(points);
            return out;
        }
        auto splitOpt = noteU16(t, len, 562, le);
        uint16_t split = splitOpt.value_or(0);
        if (split != 0) {
            throw RawError::unsupported("Nikon \"lossy after split\" compressed NEF (split at row " + std::to_string(split) + ")");
        }
        size_t range = static_cast<size_t>(1) << bits;
        if (n - 1 > range || range % (n - 1) != 0) {
            throw RawError::unsupported("Nikon compressed NEF: " + std::to_string(n) + "-point curve for " + std::to_string(bits) + "-bit data");
        }
        uint32_t step = static_cast<uint32_t>(range / (n - 1));
        std::vector<uint16_t> curve;
        curve.reserve(range + 1);
        for (size_t p = 0; p + 1 < points.size(); ++p) {
            uint32_t a = points[p];
            uint32_t b = points[p + 1];
            for (uint32_t k = 0; k < step; ++k) {
                curve.push_back(static_cast<uint16_t>((a * (step - k) + b * k) / step));
            }
        }
        curve.push_back(points.back());
        out.encoding.lossless = false;
        out.encoding.curve = std::move(curve);
        return out;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%#04x", v0);
    char buf2[16];
    std::snprintf(buf2, sizeof buf2, "%#04x", v1);
    throw RawError::unsupported(std::string("Nikon compressed NEF version ") + buf + " " + buf2);
}

// 饱和加法（上游 saturating_add）：d 为负时 `INT32_MAX - d` 会溢出，须分侧判断。
inline int32_t satAdd(int32_t a, int32_t d) {
    if (d > 0 && a > INT32_MAX - d) return INT32_MAX;
    if (d < 0 && a < INT32_MIN - d) return INT32_MIN;
    return a + d;
}

// 解码 w×h 的 Nikon Huffman 压缩 strip（bits 位样本）。
std::vector<uint16_t> nefcDecode(const uint8_t* src, size_t srcLen, size_t w, size_t h, uint32_t bits, const NefDecodeTable& table) {
    if (bits != 12 && bits != 14) {
        throw RawError::unsupported("Nikon compressed NEF with " + std::to_string(bits) + "-bit samples");
    }
    if (w == 0 || h == 0 || w > SIZE_MAX / h) throw RawError::malformed("NEF image is empty");
    size_t n = w * h;
    // 每个码字至少 2 位：装不下图像的 strip 是截断的（同时按文件大小约束分配）。
    if (n / 4 > srcLen) {
        throw RawError::malformed("NEF: " + std::to_string(srcLen) + " bytes of compressed data for " + std::to_string(w) + "x" + std::to_string(h) + " pixels");
    }
    bool lossy = !table.encoding.lossless;
    const Table& tbl = lossy ? (bits == 12 ? kLossy12 : kLossy14) : (bits == 12 ? kLossless12 : kLossless14);
    NefHuffman huff(tbl);
    const uint16_t* lut = lossy ? table.encoding.curve.data() : nullptr;
    size_t lutLen = lossy ? table.encoding.curve.size() : 0;
    int32_t max = (1 << bits) - 1;
    std::vector<uint16_t> out(n, 0);
    NefBits stream;
    stream.src = src;
    stream.len = srcLen;
    int32_t vpred[2][2] = {{table.seeds[0][0], table.seeds[0][1]}, {table.seeds[1][0], table.seeds[1][1]}};
    size_t clipped = 0;
    size_t dbgN = 0;
    for (size_t y = 0; y < h; ++y) {
        int32_t* seeds = vpred[y & 1];
        int32_t hpred[2] = {0, 0};
        uint16_t* row = out.data() + y * w;
        for (size_t x = 0; x < w; ++x) {
            auto dOpt = nefDiff(stream, huff);
            if (!dOpt) throw RawError::malformed("NEF: invalid Huffman code in row " + std::to_string(y));
            int32_t d = *dOpt;
            int32_t p;
            if (x < 2) {
                seeds[x] = satAdd(seeds[x], d);
                hpred[x] = seeds[x];
                p = seeds[x];
            } else {
                int32_t hidx = x & 1;
                hpred[hidx] = satAdd(hpred[hidx], d);
                p = hpred[hidx];
            }
            int32_t v = std::clamp(p, 0, max);
            if (v != p) {
                ++clipped;
            }
            row[x] = lut ? (static_cast<size_t>(v) < lutLen ? lut[v] : 0) : static_cast<uint16_t>(v);
        }
        // 相机填充 strip；远超末尾的读取意味着数据截断或损坏。
        if (stream.consumed > srcLen * 8 + 64) {
            throw RawError::malformed("NEF: compressed data ends in row " + std::to_string(y) + " of " + std::to_string(h));
        }
    }
    // 错表或坏流很快漂出范围；真文件保持在界内（个别边缘像素）。
    if (clipped > n / 1000 + 16) {
        throw RawError::malformed("NEF: " + std::to_string(clipped) + " decoded values out of range");
    }
    return out;
}

}  // namespace

// 读 raw IFD 的 Huffman 压缩 CFA 图像。maker note（自己的 TIFF 与 IFD0）持解码表。
Plane readNefcCompressed(const Tiff& t, const Ifd& rawIfdIn, const Limits& limits) {
    std::vector<Ifd> ifds = t.allIfds();
    std::optional<Tiff> note;
    std::optional<Ifd> noteIfd;
    for (const Ifd& i : ifds) {
        const Entry* e = i.get(tag::MAKER_NOTE);
        if (!e) continue;
        size_t l = 0;
        const uint8_t* b = t.raw(*e, l);
        if (!b || l < 10 || std::memcmp(b, "Nikon\0", 6) != 0) continue;
        size_t base = e->at + 10;
        if (base > t.len) continue;
        auto inner = Tiff::create(t.data + base, t.len - base);
        if (!inner) continue;
        auto ifd = inner->ifdAt(inner->firstIfd, 0);
        if (!ifd) continue;
        note = std::move(*inner);
        noteIfd = std::move(*ifd);
        break;
    }
    auto wOpt = t.tagUint(rawIfdIn, tag::IMAGE_WIDTH);
    auto hOpt = t.tagUint(rawIfdIn, tag::IMAGE_LENGTH);
    if (!wOpt) throw RawError::malformed("raw image has no width");
    if (!hOpt) throw RawError::malformed("raw image has no height");
    size_t width = *wOpt, height = *hOpt;
    size_t samples = t.tagUint(rawIfdIn, tag::SAMPLES_PER_PIXEL).value_or(1);
    if (samples != 1) {
        throw RawError::unsupported("Nikon compressed NEF with " + std::to_string(samples) + " samples per pixel");
    }
    uint32_t bits = t.tagUint(rawIfdIn, tag::BITS_PER_SAMPLE).value_or(0);
    const Entry* tableEntry = nullptr;
    if (note && noteIfd) tableEntry = noteIfd->get(kNikonLinearizationTable);
    if (!note || !noteIfd || !tableEntry) {
        throw RawError::unsupported("Nikon compressed NEF without a linearization table (maker note 0x96)");
    }
    size_t tl = 0;
    const uint8_t* tableBytes = note->raw(*tableEntry, tl);
    if (!tableBytes) throw RawError::unsupported("Nikon compressed NEF without a linearization table (maker note 0x96)");
    NefDecodeTable table = parseNefTable(tableBytes, tl, note->le, bits);
    limits.check(width, height, 2);
    // 每样本一个 strip；多个则是同一位流的连续部分。
    std::vector<uint32_t> offsets = t.tagUints(rawIfdIn, tag::STRIP_OFFSETS);
    std::vector<uint32_t> counts = t.tagUints(rawIfdIn, tag::STRIP_BYTE_COUNTS);
    if (offsets.empty() || counts.empty()) throw RawError::malformed("NEF: no image data");
    size_t start = offsets.front();
    size_t end = std::min<size_t>(static_cast<size_t>(offsets.back()) + counts.back(), t.len);
    if (start > t.len || end <= start) throw RawError::malformed("raw data lies outside the file");
    std::vector<uint16_t> data = nefcDecode(t.data + start, end - start, width, height, bits, table);
    Plane plane;
    plane.width = width;
    plane.height = height;
    plane.samples = 1;
    plane.bits = bits;
    plane.data = std::move(data);
    return plane;
}

}  // namespace raw
}  // namespace montage
