// M23a：无损 JPEG 解码器实现。翻译自 photocraft crates/raw/src/ljpeg.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_ljpeg.h"

#include <cstring>

namespace montage {
namespace raw {

namespace {

// 一次表查找解析的位数。
constexpr uint32_t kLookupBits = 13;

struct Huffman {
    // 长度 ≤ kLookupBits 的码：(length << 8) | symbol；更长为 0。
    std::vector<uint16_t> lookup;
    // 码连同附加位能塞进 kLookupBits 时：(difference << 8) | bits；否则 0：
    // 常见路径每样本一次查找。
    std::vector<int32_t> fast;
    // 每长度的最大码（-1 = 无），长度 1..=16。
    int32_t maxcode[17] = {};
    int32_t mincode[17] = {};
    size_t valptr[17] = {};
    std::vector<uint8_t> vals;

    // 从 BITS（每长度 1..16 的计数）与 HUFFVAL（T.81 Annex C）构建解码器。
    static Huffman build(const uint8_t counts[16], const uint8_t* valsIn, size_t nVals) {
        size_t total = 0;
        for (int i = 0; i < 16; ++i) total += counts[i];
        if (total == 0 || total > 256 || nVals < total) throw RawError::malformed("bad Huffman table");
        Huffman h;
        h.lookup.assign(static_cast<size_t>(1) << kLookupBits, 0);
        h.fast.assign(static_cast<size_t>(1) << kLookupBits, 0);
        for (int i = 0; i <= 16; ++i) {
            h.maxcode[i] = -1;
            h.mincode[i] = 0;
            h.valptr[i] = 0;
        }
        h.vals.assign(valsIn, valsIn + total);
        uint32_t code = 0;
        size_t k = 0;
        for (uint32_t len = 1; len <= 16; ++len) {
            size_t n = counts[len - 1];
            if (n > 0) {
                h.valptr[len] = k;
                h.mincode[len] = static_cast<int32_t>(code);
                for (size_t i = 0; i < n; ++i) {
                    if (code >= (1u << len)) throw RawError::malformed("Huffman code overflow");
                    if (len <= kLookupBits) {
                        uint32_t shift = kLookupBits - len;
                        size_t base = static_cast<size_t>(code << shift);
                        uint16_t entry = static_cast<uint16_t>((len << 8) | h.vals[k]);
                        std::fill(h.lookup.begin() + base, h.lookup.begin() + base + (1u << shift), entry);
                    }
                    ++code;
                    ++k;
                }
                h.maxcode[len] = static_cast<int32_t>(code) - 1;
            }
            code <<= 1;
        }
        for (size_t idx = 0; idx < h.fast.size(); ++idx) {
            uint16_t e = h.lookup[idx];
            uint32_t len = e >> 8;
            uint32_t s = e & 0xFF;
            if (len == 0) continue;
            int32_t d;
            if (s == 0) {
                d = 0;
            } else if (s == 16) {
                d = 32768;
            } else if (s >= 1 && s <= 15 && len + s <= kLookupBits) {
                int32_t v = static_cast<int32_t>((idx >> (kLookupBits - len - s)) & ((1u << s) - 1));
                d = v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
            } else {
                continue;
            }
            uint32_t extra = (s >= 1 && s <= 15) ? s : 0;
            h.fast[idx] = (d << 8) | static_cast<int32_t>(len + extra);
        }
        return h;
    }
};

// 熵编码数据上的 MSB-first 位读取器：去掉 FF 00 填塞，遇 marker 停（此后喂零位）。
struct Bits {
    const uint8_t* data = nullptr;
    size_t len = 0;
    size_t pos = 0;
    uint64_t acc = 0;
    uint32_t n = 0;
    bool marker = false;
    // 数据尾或 marker 之后喂入的零字节数。
    size_t fake = 0;

    void refill() {
        // 快路径：接下来 8 字节无 0xFF（无填塞、无 marker）。
        if (!marker && len >= pos + 8) {
            uint64_t chunk = 0;
            for (int i = 0; i < 8; ++i) chunk = chunk << 8 | data[pos + i];
            uint64_t inv = ~chunk;
            if (((inv - 0x0101010101010101ull) & ~inv & 0x8080808080808080ull) == 0) {
                uint32_t k = (64 - n) / 8;  // 放得下的整字节数（1..=8）
                if (k == 0) return;
                if (k == 8) {
                    acc = chunk;
                } else {
                    acc |= (chunk >> (64 - 8 * k)) << (64 - n - 8 * k);
                }
                n += 8 * k;
                pos += k;
                return;
            }
        }
        while (n <= 56) {
            uint8_t byte;
            if (marker) {
                ++fake;
                byte = 0;
            } else if (pos < len && data[pos] == 0xFF) {
                if (pos + 1 < len && data[pos + 1] == 0) {
                    pos += 2;
                    byte = 0xFF;
                } else {
                    marker = true;
                    ++fake;
                    byte = 0;
                }
            } else if (pos < len) {
                byte = data[pos++];
            } else {
                marker = true;
                ++fake;
                byte = 0;
            }
            acc |= static_cast<uint64_t>(byte) << (56 - n);
            n += 8;
        }
    }

    inline uint32_t peek(uint32_t k) const { return static_cast<uint32_t>(acc >> (64 - k)); }

    inline void consume(uint32_t k) {
        acc <<= k;
        n -= k;
    }

    // 读 k（1..=16）位。
    inline uint32_t get(uint32_t k) {
        if (n < k) refill();
        uint32_t v = peek(k);
        consume(k);
        return v;
    }

    // 跳到下一个 RSTn marker 并越过；重置位缓冲。
    void restart() {
        acc = 0;
        n = 0;
        marker = false;
        fake = 0;
        // 容忍 marker 前的填充字节。
        while (pos < len) {
            uint8_t b = data[pos];
            if (b == 0xFF) {
                if (pos + 1 < len && data[pos + 1] >= 0xD0 && data[pos + 1] <= 0xD7) {
                    pos += 2;
                    return;
                }
                if (pos + 1 < len && data[pos + 1] == 0xFF) {
                    ++pos;
                    continue;
                }
                throw RawError::malformed("expected a restart marker");
            }
            ++pos;
        }
        throw RawError::malformed("missing restart marker");
    }

    inline uint32_t decode(const Huffman& h) {
        if (n < 32) refill();
        uint16_t e = h.lookup[peek(kLookupBits)];
        if (e != 0) {
            consume(e >> 8);
            return e & 0xFF;
        }
        for (uint32_t len = kLookupBits + 1; len <= 16; ++len) {
            int32_t code = static_cast<int32_t>(peek(len));
            if (code <= h.maxcode[len]) {
                size_t i = h.valptr[len] + static_cast<size_t>(code - h.mincode[len]);
                if (i >= h.vals.size()) throw RawError::malformed("bad Huffman code");
                uint32_t v = h.vals[i];
                consume(len);
                return v;
            }
        }
        throw RawError::malformed("bad Huffman code");
    }

    // 一个差分值：Huffman 编码的类别 SSSS 然后 SSSS 附加位（T.81 H.1.2.2）。
    inline int32_t diff(const Huffman& h) {
        if (n < 32) refill();
        int32_t f = h.fast[peek(kLookupBits)];
        if (f != 0) {
            consume(static_cast<uint32_t>(f & 0xFF));
            return f >> 8;
        }
        uint32_t s = decode(h);
        if (s == 0) return 0;
        if (s >= 1 && s <= 15) {
            int32_t v = static_cast<int32_t>(get(s));
            return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
        }
        if (s == 16) return 32768;
        throw RawError::malformed("bad difference category");
    }
};

size_t be16(const uint8_t* d, size_t len, size_t at) {
    if (at > len || len - at < 2) throw RawError::malformed("truncated lossless JPEG");
    return static_cast<size_t>(d[at]) << 8 | d[at + 1];
}

struct Parsed {
    LjpegFrame frame;
    // scan 各组件（帧序）的 Huffman 表。
    std::vector<Huffman> tables;
    uint8_t predictor = 0;
    uint8_t pointTransform = 0;
    size_t restart = 0;
    const uint8_t* entropy = nullptr;
    size_t entropyLen = 0;
};

Parsed ljpegParse(const uint8_t* data, size_t len) {
    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8) throw RawError::malformed("lossless JPEG: missing SOI");
    size_t pos = 2;
    std::optional<LjpegFrame> frame;
    std::vector<uint8_t> compIds;
    std::optional<Huffman> huff[4];
    size_t restart = 0;
    while (true) {
        // 跳到 marker 的填充字节。
        while (pos + 1 < len && data[pos] == 0xFF && data[pos + 1] == 0xFF) ++pos;
        if (pos >= len || data[pos] != 0xFF) throw RawError::malformed("lossless JPEG: expected a marker");
        if (pos + 1 >= len) throw RawError::malformed("truncated lossless JPEG");
        uint8_t marker = data[pos + 1];
        pos += 2;
        if (marker == 0xD9) throw RawError::malformed("lossless JPEG: no scan");
        if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
        size_t segLen = be16(data, len, pos);
        if (segLen < 2) throw RawError::malformed("lossless JPEG: bad segment length");
        if (pos + segLen > len) throw RawError::malformed("truncated lossless JPEG segment");
        const uint8_t* seg = data + pos + 2;
        size_t segBody = segLen - 2;
        switch (marker) {
            case 0xC3: {
                if (segBody < 6) throw RawError::malformed("short SOF3");
                uint8_t precision = seg[0];
                size_t height = static_cast<size_t>(seg[1]) << 8 | seg[2];
                size_t width = static_cast<size_t>(seg[3]) << 8 | seg[4];
                size_t nf = seg[5];
                if (precision < 2 || precision > 16 || width == 0 || height == 0 || nf == 0 || nf > 4 || segBody < 6 + 3 * nf) {
                    throw RawError::malformed("unsupported SOF3 (" + std::to_string(precision) + "-bit, " + std::to_string(width) + "x" +
                                              std::to_string(height) + ", " + std::to_string(nf) + " components)");
                }
                compIds.clear();
                for (size_t c = 0; c < nf; ++c) {
                    const uint8_t* s = seg + 6 + 3 * c;
                    if (s[1] != 0x11) throw RawError::unsupported("lossless JPEG with subsampled components");
                    compIds.push_back(s[0]);
                }
                frame = LjpegFrame{precision, width, height, nf};
                break;
            }
            case 0xC0: case 0xC1: case 0xC2:
                throw RawError::unsupported("lossy (DCT) JPEG compressed raw data");
            case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
            case 0xCD: case 0xCE: case 0xCF:
                throw RawError::unsupported("hierarchical or arithmetic-coded JPEG");
            case 0xC4: {
                size_t p = 0;
                while (p < segBody) {
                    uint8_t tcTh = seg[p];
                    size_t id = tcTh & 0x0F;
                    if (p + 17 > segBody) throw RawError::malformed("short DHT");
                    uint8_t counts[16];
                    std::memcpy(counts, seg + p + 1, 16);
                    size_t total = 0;
                    for (int i = 0; i < 16; ++i) total += counts[i];
                    if (p + 17 + total > segBody) throw RawError::malformed("short DHT");
                    if (id < 4) huff[id] = Huffman::build(counts, seg + p + 17, total);
                    p += 17 + total;
                }
                break;
            }
            case 0xDD: {
                restart = be16(seg, segBody, 0);
                break;
            }
            case 0xDA: {
                if (!frame) throw RawError::malformed("lossless JPEG: SOS before SOF3");
                LjpegFrame f = *frame;
                if (segBody < 1) throw RawError::malformed("short SOS");
                size_t ns = seg[0];
                if (ns != f.components || segBody < 1 + 2 * ns + 3) {
                    throw RawError::unsupported("lossless JPEG with non-interleaved scans");
                }
                Parsed out;
                out.tables.reserve(ns);
                for (size_t c = 0; c < ns; ++c) {
                    uint8_t id = seg[1 + 2 * c];
                    if (c >= compIds.size() || compIds[c] != id) {
                        throw RawError::unsupported("lossless JPEG scan component order");
                    }
                    size_t td = seg[2 + 2 * c] >> 4;
                    if (td >= 4 || !huff[td]) throw RawError::malformed("missing Huffman table");
                    out.tables.push_back(*huff[td]);
                }
                out.frame = f;
                out.predictor = seg[1 + 2 * ns];
                out.pointTransform = seg[3 + 2 * ns] & 0x0F;
                if (out.predictor < 1 || out.predictor > 7) {
                    throw RawError::malformed("lossless JPEG predictor " + std::to_string(out.predictor));
                }
                if (out.pointTransform >= f.precision) throw RawError::malformed("lossless JPEG point transform");
                out.entropy = pos + segLen <= len ? data + (pos + segLen) : nullptr;
                out.entropyLen = pos + segLen <= len ? len - (pos + segLen) : 0;
                out.restart = restart;
                return out;
            }
            default:
                break;  // APPn、COM、DQT、DNL… 忽略
        }
        pos += segLen;
    }
}

}  // namespace

LjpegFrame ljpegFrame(const uint8_t* data, size_t len) { return ljpegParse(data, len).frame; }

std::pair<LjpegFrame, std::vector<uint16_t>> ljpegDecode(const uint8_t* data, size_t len, size_t maxSamples) {
    Parsed p = ljpegParse(data, len);
    LjpegFrame f = p.frame;
    auto totalOpt = f.samples();
    size_t total;
    if (!totalOpt || *totalOpt > maxSamples) {
        throw RawError::limitExceeded("lossless JPEG frame " + std::to_string(f.width) + "x" + std::to_string(f.height) + "x" +
                                      std::to_string(f.components));
    }
    total = *totalOpt;
    // 一个熵编码样本至少要 1 位：比帧短太多的流不可能有效（廉价的解压炸弹护栏）。
    if (p.entropyLen * 8 < total / 2) throw RawError::malformed("lossless JPEG data is truncated");
    size_t rowLen = f.width * f.components;
    if (p.restart != 0 && p.restart % f.width != 0) {
        throw RawError::unsupported("lossless JPEG restart interval that is not whole rows");
    }
    size_t rowsPerInterval = p.restart == 0 ? SIZE_MAX : p.restart / f.width;
    std::vector<uint16_t> out(total, 0);
    Bits bits;
    bits.data = p.entropy;
    bits.len = p.entropyLen;
    size_t nc = f.components;
    int32_t initial = 1 << (f.precision - p.pointTransform - 1);
    uint8_t pred = p.predictor;
    size_t intervalRow = 0;
    for (size_t y = 0; y < f.height; ++y) {
        if (intervalRow == rowsPerInterval) {
            bits.restart();
            intervalRow = 0;
        }
        bool firstLine = intervalRow == 0;
        uint16_t* cur = out.data() + y * rowLen;
        const uint16_t* prev = y == 0 ? nullptr : out.data() + (y - 1) * rowLen;
        if (pred == 1 || firstLine) {
            // 预测器 1（或区间的首行）：首列后是 Ra。
            for (size_t c = 0; c < nc; ++c) {
                int32_t px = firstLine ? initial : static_cast<int32_t>(prev[c]);
                cur[c] = static_cast<uint16_t>(px + bits.diff(p.tables[c]));
            }
            if (nc == 1) {
                const Huffman& t = p.tables[0];
                for (size_t i = 1; i < rowLen; ++i) {
                    cur[i] = static_cast<uint16_t>(static_cast<int32_t>(cur[i - 1]) + bits.diff(t));
                }
            } else {
                for (size_t i = nc; i < rowLen; i += nc) {
                    for (size_t c = 0; c < nc; ++c) {
                        cur[i + c] = static_cast<uint16_t>(static_cast<int32_t>(cur[i + c - nc]) + bits.diff(p.tables[c]));
                    }
                }
            }
            ++intervalRow;
            if (bits.fake > 64) throw RawError::malformed("lossless JPEG data is truncated");
            continue;
        }
        for (size_t x = 0; x < f.width; ++x) {
            for (size_t c = 0; c < nc; ++c) {
                size_t i = x * nc + c;
                int32_t px;
                if (x == 0) {
                    px = firstLine ? initial : static_cast<int32_t>(prev[i]);
                } else {
                    int32_t ra = static_cast<int32_t>(cur[i - nc]);
                    if (firstLine) {
                        px = ra;
                    } else {
                        int32_t rb = static_cast<int32_t>(prev[i]);
                        int32_t rc = static_cast<int32_t>(prev[i - nc]);
                        switch (pred) {
                            case 1: px = ra; break;
                            case 2: px = rb; break;
                            case 3: px = rc; break;
                            case 4: px = ra + rb - rc; break;
                            case 5: px = ra + ((rb - rc) >> 1); break;
                            case 6: px = rb + ((ra - rc) >> 1); break;
                            default: px = (ra + rb) >> 1; break;
                        }
                    }
                }
                int32_t d = bits.diff(p.tables[c]);
                cur[i] = static_cast<uint16_t>(px + d);
            }
        }
        ++intervalRow;
        if (bits.fake > 64) throw RawError::malformed("lossless JPEG data is truncated");
    }
    if (p.pointTransform > 0) {
        for (uint16_t& v : out) v = static_cast<uint16_t>(v << p.pointTransform);
    }
    return {f, std::move(out)};
}

}  // namespace raw
}  // namespace montage
