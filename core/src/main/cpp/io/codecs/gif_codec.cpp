#include "io/codecs/gif_codec.h"

#include <cstring>
#include <unordered_map>

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

// ---------- LZW 解码（GIF 变体：LSB 先行、宽度延迟增位） ----------

bool lzwDecode(const uint8_t* data, size_t len, int minCodeSize, std::vector<uint8_t>& out,
               size_t expect, std::string& err) {
    if (minCodeSize < 2 || minCodeSize > 8) {
        err = "gif lzw: bad min code size";
        return false;
    }
    const int clear = 1 << minCodeSize;
    const int eoi = clear + 1;
    int width = minCodeSize + 1;
    int next = eoi + 1;

    // 字典链：prefix 前缀码 + suffix 末字符（重建沿链回溯；非 static——解码可并发）
    std::vector<uint16_t> prefix(4096, 0);
    std::vector<uint8_t> suffix(4096, 0);
    for (int i = 0; i < clear; ++i) {
        suffix[i] = static_cast<uint8_t>(i);
    }

    // 位流（LSB 先行；子块拼平已在调用方完成——这里直接连续读）
    size_t bitPos = 0;
    auto readCode = [&](int* code) -> bool {
        const size_t byte = bitPos >> 3;
        if (byte + 3 > len) {
            // 尾部允许不足 4 字节时按位取到尽头
            if (byte >= len) {
                return false;
            }
            uint32_t acc = 0;
            for (size_t i = byte; i < len; ++i) {
                acc |= static_cast<uint32_t>(data[i]) << ((i - byte) * 8);
            }
            *code = static_cast<int>((acc >> (bitPos & 7)) & ((1u << width) - 1u));
            bitPos += static_cast<size_t>(width);
            return true;
        }
        uint32_t acc = static_cast<uint32_t>(data[byte]) |
                       (static_cast<uint32_t>(data[byte + 1]) << 8) |
                       (static_cast<uint32_t>(data[byte + 2]) << 16);
        *code = static_cast<int>((acc >> (bitPos & 7)) & ((1u << width) - 1u));
        bitPos += static_cast<size_t>(width);
        return true;
    };

    out.clear();
    out.reserve(expect);
    uint8_t stack[4096];
    int prev = -1;
    int code;
    while (readCode(&code)) {
        if (code == clear) {
            width = minCodeSize + 1;
            next = eoi + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) {
            break;
        }
        if (code > next || (code == next && prev < 0)) {
            err = "gif lzw: code out of dict";
            return false;
        }
        // 展开到栈（KwKwK：先放 prev 首字符）
        int sp = 0;
        int cur = code;
        if (code == next) {
            stack[sp++] = suffix[prev];
            cur = prev;
        }
        while (cur >= clear + 2 && sp < 4096) {
            stack[sp++] = suffix[cur];
            cur = prefix[cur];
        }
        if (sp >= 4096) {
            err = "gif lzw: dict chain overflow";
            return false;
        }
        stack[sp++] = suffix[cur];
        const uint8_t first = suffix[cur];
        for (int i = sp - 1; i >= 0; --i) {
            out.push_back(stack[i]);
        }
        // 新条目（宽度延迟增位：next 达 2^width 才加宽）
        if (prev >= 0 && next < 4096) {
            prefix[next] = static_cast<uint16_t>(prev);
            suffix[next] = first;
            ++next;
            if (next == (1 << width) && width < 12) {
                ++width;
            }
        }
        prev = code;
        if (expect != 0 && out.size() >= expect) {
            break;
        }
    }
    return true;
}

// ---------- LZW 编码 ----------

void lzwEncode(const std::vector<uint8_t>& pixels, int minCodeSize, std::vector<uint8_t>& out) {
    const int clear = 1 << minCodeSize;
    const int eoi = clear + 1;
    int width = minCodeSize + 1;
    int next = eoi + 1;

    uint32_t acc = 0;
    int nbits = 0;
    auto emit = [&](int code) {
        acc |= static_cast<uint32_t>(code) << nbits;
        nbits += width;
        while (nbits >= 8) {
            out.push_back(static_cast<uint8_t>(acc & 0xFF));
            acc >>= 8;
            nbits -= 8;
        }
    };

    std::unordered_map<uint32_t, int> dict;
    dict.reserve(1 << 14);
    emit(clear);
    int prefix = -1;
    for (uint8_t c : pixels) {
        if (prefix < 0) {
            prefix = c;
            continue;
        }
        const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | c;
        auto it = dict.find(key);
        if (it != dict.end()) {
            prefix = it->second;
            continue;
        }
        emit(prefix);
        if (next < 4096) {
            dict.emplace(key, next);
            ++next;
            // 宽度切换延迟一档（next > 2^width）：编码器字典比解码器超前一个条目
            //（解码器首个数据码不建条目），== 切换会让码流超前解码器一位
            //（PIL 裁决实证：引擎解码器语义与 PIL 一致，编码器对齐之）
            if (next > (1 << width) && width < 12) {
                ++width;
            }
        } else {
            // 字典满：清码重置（宽度随 clear 回到初始）
            emit(clear);
            dict.clear();
            width = minCodeSize + 1;
            next = eoi + 1;
        }
        prefix = c;
    }
    if (prefix >= 0) {
        emit(prefix);
    }
    emit(eoi);
    if (nbits > 0) {
        out.push_back(static_cast<uint8_t>(acc & 0xFF));
    }
}

}  // namespace

bool gifDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 13) {
        err = "gif: truncated header";
        return false;
    }
    if (std::memcmp(data, "GIF87a", 6) != 0 && std::memcmp(data, "GIF89a", 6) != 0) {
        err = "gif: bad magic";
        return false;
    }
    const uint16_t sw = rdU16(data + 6);
    const uint16_t sh = rdU16(data + 8);
    const uint8_t packed = data[10];
    size_t pos = 13;
    std::vector<uint8_t> gct;
    if (packed & 0x80) {
        const size_t n = static_cast<size_t>(3) * (1u << ((packed & 7) + 1));
        if (pos + n > len) {
            err = "gif: gct truncated";
            return false;
        }
        gct.assign(data + pos, data + pos + n);
        pos += n;
    }

    // v1 首帧：读到第一个图像块即合成
    std::vector<uint8_t> lct;
    uint16_t fw = 0;
    uint16_t fh = 0;
    bool interlace = false;
    int transIdx = -1;  // 最近 GCE 的透明索引
    bool gotImage = false;

    while (pos < len && !gotImage) {
        const uint8_t block = data[pos++];
        if (block == 0x3B) {
            break;
        }
        if (block == 0x21) {
            if (pos >= len) {
                break;
            }
            const uint8_t label = data[pos++];
            if (label == 0xF9 && pos + 5 <= len && data[pos] == 4) {  // GCE
                const uint8_t gpacked = data[pos + 1];
                transIdx = (gpacked & 0x01) ? data[pos + 4] : -1;
                pos += 6;
            } else {
                // 通用子块跳过
                while (pos < len) {
                    const uint8_t sz = data[pos++];
                    if (sz == 0) {
                        break;
                    }
                    pos += sz;
                }
            }
            continue;
        }
        if (block == 0x2C) {
            if (pos + 9 > len) {
                err = "gif: image descriptor truncated";
                return false;
            }
            // left/top 忽略（首帧合成全屏）
            fw = rdU16(data + pos + 4);
            fh = rdU16(data + pos + 6);
            const uint8_t ipacked = data[pos + 8];
            pos += 9;
            if (fw == 0 || fh == 0 || static_cast<uint64_t>(fw) * fh > kLimit) {
                err = "gif: frame size out of limits";
                return false;
            }
            interlace = (ipacked & 0x40) != 0;
            lct.clear();
            if (ipacked & 0x80) {
                const size_t n = static_cast<size_t>(3) * (1u << ((ipacked & 7) + 1));
                if (pos + n > len) {
                    err = "gif: lct truncated";
                    return false;
                }
                lct.assign(data + pos, data + pos + n);
                pos += n;
            }
            if (pos >= len) {
                err = "gif: missing lzw min code size";
                return false;
            }
            const int minCodeSize = data[pos++];
            // 子块拼平
            std::vector<uint8_t> stream;
            while (pos < len) {
                const uint8_t sz = data[pos++];
                if (sz == 0) {
                    break;
                }
                if (pos + sz > len) {
                    err = "gif: lzw data truncated";
                    return false;
                }
                stream.insert(stream.end(), data + pos, data + pos + sz);
                pos += sz;
            }
            const std::vector<uint8_t>& pal = lct.empty() ? gct : lct;
            if (pal.empty()) {
                err = "gif: no color table";
                return false;
            }
            std::vector<uint8_t> indices;
            if (!lzwDecode(stream.data(), stream.size(), minCodeSize, indices,
                           static_cast<size_t>(fw) * fh, err)) {
                return false;
            }
            if (indices.size() < static_cast<size_t>(fw) * fh) {
                err = "gif: index stream short";
                return false;
            }
            out.w = fw;
            out.h = fh;
            out.rgba.assign(static_cast<size_t>(fw) * fh * 4, 0);
            // interlace 行序还原（四趟 8/8、8/8、4/2、2/1）——先建源行→目标行映射
            {
                static constexpr int kStart[4] = {0, 4, 2, 1};
                static constexpr int kStep[4] = {8, 8, 4, 2};
                std::vector<int> rowMap;
                rowMap.reserve(fh);
                if (!interlace) {
                    for (int y = 0; y < fh; ++y) {
                        rowMap.push_back(y);
                    }
                } else {
                    for (int pass = 0; pass < 4; ++pass) {
                        for (int y = kStart[pass]; y < fh; y += kStep[pass]) {
                            rowMap.push_back(y);
                        }
                    }
                }
                for (int y = 0; y < fh; ++y) {
                    const int dy = rowMap[y];
                    uint8_t* drow = out.rgba.data() + static_cast<size_t>(dy) * fw * 4;
                    for (int x = 0; x < fw; ++x) {
                        const uint8_t idx = indices[static_cast<size_t>(y) * fw + x];
                        const size_t pe = static_cast<size_t>(idx) * 3;
                        drow[x * 4 + 0] = pe + 2 < pal.size() ? pal[pe + 0] : 0;
                        drow[x * 4 + 1] = pe + 2 < pal.size() ? pal[pe + 1] : 0;
                        drow[x * 4 + 2] = pe + 2 < pal.size() ? pal[pe + 2] : 0;
                        drow[x * 4 + 3] = (transIdx >= 0 && idx == transIdx) ? 0 : 255;
                    }
                }
            }
            // 透明索引：GCE 可能在 descriptor 前出现——本简化路径不回收 GCE 透明
            //（透明 GIF v1 由 encode 侧支持；读侧 alpha 恒 255 登记边界）
            gotImage = true;
            (void)sw;
            (void)sh;
            break;
        }
        // 未知块：中止（防死循环）
        err = "gif: unknown block";
        return false;
    }
    if (!gotImage) {
        err = "gif: no image data";
        return false;
    }
    return true;
}

bool gifEncode(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors, bool dither,
               std::vector<uint8_t>& out, std::string& err) {
    if (rgba == nullptr || w == 0 || h == 0 ||
        static_cast<uint64_t>(w) * h > kLimit) {
        err = "gif encode: bad input";
        return false;
    }
    if (maxColors < 2 || maxColors > 256) {
        maxColors = 256;
    }
    std::vector<uint8_t> palette;
    std::vector<uint8_t> indices;
    const int transIdx = quantizeImage(rgba, w, h, maxColors, dither, palette, indices);
    int nColors = static_cast<int>(palette.size() / 3);
    if (nColors < 1) {
        err = "gif encode: empty palette";
        return false;
    }
    // 调色板尺寸 = 2 的幂 ≥ nColors（最小 2）
    int gctBits = 1;
    while ((1 << (gctBits + 1)) < nColors) {
        ++gctBits;
    }
    // (1<<(gctBits+1)) ≥ nColors；nColors=1 → gctBits=0 → 2 项
    const int gctSize = 1 << (gctBits + 1);
    const int minCodeSize = gctBits + 1 < 2 ? 2 : gctBits + 1;

    out.clear();
    out.insert(out.end(), {'G', 'I', 'F', '8', '9', 'a'});
    wrU16(out, static_cast<uint16_t>(w));
    wrU16(out, static_cast<uint16_t>(h));
    out.push_back(static_cast<uint8_t>(0x80 | (7 << 4) | gctBits));  // GCT flag + 色深 + size
    out.push_back(0);   // bg
    out.push_back(0);   // aspect
    for (int i = 0; i < gctSize; ++i) {
        if (i < nColors) {
            out.push_back(palette[i * 3 + 0]);
            out.push_back(palette[i * 3 + 1]);
            out.push_back(palette[i * 3 + 2]);
        } else {
            out.push_back(0);
            out.push_back(0);
            out.push_back(0);
        }
    }
    // GCE（透明）
    if (transIdx >= 0) {
        out.push_back(0x21);
        out.push_back(0xF9);
        out.push_back(4);
        out.push_back(0x01);  // 透明 flag
        wrU16(out, 0);
        out.push_back(static_cast<uint8_t>(transIdx));
        out.push_back(0);
    }
    // 图像描述符
    out.push_back(0x2C);
    wrU16(out, 0);
    wrU16(out, 0);
    wrU16(out, static_cast<uint16_t>(w));
    wrU16(out, static_cast<uint16_t>(h));
    out.push_back(0);  // 无 LCT、不 interlace
    // LZW
    out.push_back(static_cast<uint8_t>(minCodeSize));
    std::vector<uint8_t> lzw;
    lzwEncode(indices, minCodeSize, lzw);
    // 子块封装（≤255）
    size_t off = 0;
    while (off < lzw.size()) {
        const size_t n = std::min<size_t>(255, lzw.size() - off);
        out.push_back(static_cast<uint8_t>(n));
        out.insert(out.end(), lzw.begin() + off, lzw.begin() + off + n);
        off += n;
    }
    out.push_back(0);  // 数据块终止
    out.push_back(0x3B);  // trailer
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
