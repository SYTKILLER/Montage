// M23a：strip/tile 共享读取器。翻译自 photocraft crates/raw/src/sensor.rs 的
// read_plane / segments / undelta / unpack（MIT OR Apache-2.0）。
// 支持：未压缩（8/12 打包/16 位）、Adobe DNG 压缩 8（zlib + 预测器）、
// 压缩 7（无损 JPEG，经 raw_ljpeg）。JPEG XL（52546）与有损 JPEG（34892）
// 本批登记不支持（无 C++ 解码器依赖，错误信息明确）。

#include <cstring>
#include <optional>
#include <string>

#include "io/raw/raw_ljpeg.h"
#include "io/raw/raw_types.h"

#if defined(__has_include)
#if __has_include(<zlib.h>)
#include <zlib.h>
#define MONTAGE_RAW_HAVE_ZLIB 1
#endif
#endif

namespace montage {
namespace raw {

namespace {

// 未压缩样本解包：8 位、文件字节序 16 位、或 MSB-first 位打包行（TIFF 6.0）。
// 9–15 位数据存 16 位容器（相机 raw 常见）按字节计数识别。
std::vector<uint16_t> unpack(const uint8_t* src, size_t srcLen, size_t w, size_t rows, size_t samples, uint32_t bits, bool le) {
    size_t n = w * rows * samples;
    auto truncated = [] { return RawError::malformed("uncompressed raw data is truncated"); };
    if (bits == 16 || (bits > 8 && srcLen >= n * 2)) {
        if (srcLen < n * 2) throw truncated();
        std::vector<uint16_t> out(n);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* c = src + i * 2;
            out[i] = le ? static_cast<uint16_t>(c[0] | c[1] << 8) : static_cast<uint16_t>(c[0] << 8 | c[1]);
        }
        return out;
    }
    if (bits == 8) {
        if (srcLen < n) throw truncated();
        std::vector<uint16_t> out(n);
        for (size_t i = 0; i < n; ++i) out[i] = src[i];
        return out;
    }
    size_t rowBytes = (w * samples * bits + 7) / 8;
    if (srcLen < rowBytes * rows) throw truncated();
    std::vector<uint16_t> out;
    out.reserve(n);
    uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    for (size_t r = 0; r < rows; ++r) {
        const uint8_t* row = src + r * rowBytes;
        size_t rowAt = 0;
        uint32_t acc = 0;
        uint32_t have = 0;
        for (size_t k = 0; k < w * samples; ++k) {
            while (have < bits) {
                acc = (acc << 8) | (rowAt < rowBytes ? row[rowAt] : 0);
                ++rowAt;
                have += 8;
            }
            have -= bits;
            out.push_back(static_cast<uint16_t>((acc >> have) & mask));
        }
    }
    return out;
}

// 对交错样本反转 TIFF 预测器 2（水平差分）：每个样本加该通道行内前驱，
// 在样本容器（8 或 16 位）处回绕。
std::vector<uint16_t> undelta(const std::vector<uint16_t>& v, size_t plane, size_t rowSamples, size_t rows, uint32_t bits) {
    uint16_t mask = bits <= 8 ? 0xFF : 0xFFFF;
    std::vector<uint16_t> out = v;
    size_t rs = rowSamples > 0 ? rowSamples : 1;
    for (size_t rowStart = 0; rowStart < out.size() && rows > 0; rowStart += rs, --rows) {
        size_t rowEnd = std::min(rowStart + rs, out.size());
        for (size_t i = rowStart + plane; i < rowEnd; ++i) {
            out[i] = static_cast<uint16_t>((out[i] + out[i - plane]) & mask);
        }
    }
    return out;
}

}  // namespace

std::vector<Segment> segmentsOf(const Tiff& t, const Ifd& ifd, size_t width, size_t height) {
    std::vector<Segment> out;
    auto twOpt = t.tagUint(ifd, tag::TILE_WIDTH);
    auto thOpt = t.tagUint(ifd, tag::TILE_LENGTH);
    if (twOpt && thOpt) {
        size_t tw = *twOpt, th = *thOpt;
        if (tw == 0 || th == 0 || tw > (1u << 16) || th > (1u << 16)) throw RawError::malformed("bad tile size");
        size_t across = (width + tw - 1) / tw;
        size_t down = (height + th - 1) / th;
        std::vector<uint32_t> offsets = t.tagUints(ifd, tag::TILE_OFFSETS);
        std::vector<uint32_t> counts = t.tagUints(ifd, tag::TILE_BYTE_COUNTS);
        if (across > SIZE_MAX / down) throw RawError::malformed("too many tiles");
        size_t n = across * down;
        if (offsets.size() < n || counts.size() < n) throw RawError::malformed("missing tile offsets");
        for (size_t i = 0; i < n; ++i) {
            out.push_back(Segment{(i % across) * tw, (i / across) * th, tw, th, offsets[i], counts[i]});
        }
    } else {
        size_t rps = height;
        auto rOpt = t.tagUint(ifd, tag::ROWS_PER_STRIP);
        if (rOpt && *rOpt > 0) rps = std::min<size_t>(*rOpt, height);
        size_t n = (height + (rps > 0 ? rps : 1) - 1) / (rps > 0 ? rps : 1);
        std::vector<uint32_t> offsets = t.tagUints(ifd, tag::STRIP_OFFSETS);
        std::vector<uint32_t> counts = t.tagUints(ifd, tag::STRIP_BYTE_COUNTS);
        if (offsets.size() < n) throw RawError::malformed("missing strip offsets");
        if (counts.size() < n) {
            if (n == 1) {
                counts.assign(1, static_cast<uint32_t>(t.len > offsets[0] ? t.len - offsets[0] : 0));
            } else {
                throw RawError::malformed("missing strip byte counts");
            }
        }
        for (size_t i = 0; i < n; ++i) {
            out.push_back(Segment{0, i * rps, width, rps, offsets[i], counts[i]});
        }
    }
    return out;
}

Plane readPlane(const Tiff& t, const Ifd& ifd, const Limits& limits, JpegLayout layout) {
    auto wOpt = t.tagUint(ifd, tag::IMAGE_WIDTH);
    auto hOpt = t.tagUint(ifd, tag::IMAGE_LENGTH);
    if (!wOpt) throw RawError::malformed("raw image has no width");
    if (!hOpt) throw RawError::malformed("raw image has no height");
    size_t width = *wOpt, height = *hOpt;
    size_t samples = t.tagUint(ifd, tag::SAMPLES_PER_PIXEL).value_or(1);
    uint32_t bits = t.tagUint(ifd, tag::BITS_PER_SAMPLE).value_or(1);
    uint32_t compression = t.tagUint(ifd, tag::COMPRESSION).value_or(1);
    if (samples < 1 || samples > 4) throw RawError::unsupported(std::to_string(samples) + " samples per pixel");
    if (t.tagUint(ifd, tag::SAMPLE_FORMAT) == 3) throw RawError::unsupported("floating-point raw data");
    if (bits < 1 || bits > 16) throw RawError::unsupported(std::to_string(bits) + "-bit raw samples");
    if (samples > 1 && t.tagUint(ifd, tag::PLANAR_CONFIGURATION) == 2) throw RawError::unsupported("planar raw data");
    constexpr uint32_t kJpegXl = 52546;
    switch (compression) {
        case 1: case 7: case 8: case 34892: case kJpegXl:
            break;
        case 34713: throw RawError::unsupported("Nikon compressed NEF");
        case 32767: throw RawError::unsupported("Sony compressed ARW");
        case 65535: throw RawError::unsupported("Pentax compressed PEF");
        default: throw RawError::unsupported("raw compression scheme " + std::to_string(compression));
    }
    limits.check(width, height, samples * 2);
    std::vector<Segment> segs = segmentsOf(t, ifd, width, height);
    bool le = t.le;
    std::vector<std::optional<std::vector<uint16_t>>> decoded(segs.size());
    for (size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = segs[i];
        size_t rows = std::min(s.h, height - s.y);
        if (s.offset > t.len || s.len > t.len - s.offset) throw RawError::malformed("raw data lies outside the file");
        const uint8_t* src = t.data + s.offset;
        // 下面每个分支按段自身几何定长（文件声明，独立于图像）：瓦片可以远大于
        // 所属图像，故上方的图像检查不约束这些缓冲。先对段本身计费（上游 #2925）。
        limits.check(s.w, s.h, samples * 2);
        switch (compression) {
            case 1: {
                decoded[i] = unpack(src, s.len, s.w, rows, samples, bits, le);
                break;
            }
            case 8: {
#ifdef MONTAGE_RAW_HAVE_ZLIB
                auto tooBig = [] { return RawError::malformed("deflate tile is too large"); };
                size_t need = s.w * rows;
                if (need > SIZE_MAX / samples) throw tooBig();
                need *= samples;
                size_t byteW = (bits + 7) / 8;
                if (need > SIZE_MAX / byteW) throw tooBig();
                need *= byteW;
                std::vector<uint8_t> out(need);
                z_stream zs{};
                if (inflateInit(&zs) != Z_OK) throw RawError::malformed("deflate tile does not decompress");
                zs.next_in = const_cast<uint8_t*>(src);
                zs.avail_in = static_cast<uInt>(std::min<uint64_t>(s.len, 0x7FFFFFFFull));
                zs.next_out = out.data();
                zs.avail_out = static_cast<uInt>(std::min<uint64_t>(need, 0x7FFFFFFFull));
                int rc = inflate(&zs, Z_FINISH);
                uLong totalOut = zs.total_out;
                inflateEnd(&zs);
                if (rc != Z_STREAM_END && !(rc == Z_OK || rc == Z_BUF_ERROR)) {
                    throw RawError::malformed("deflate tile does not decompress");
                }
                if (rc == Z_BUF_ERROR && totalOut < need) {
                    throw RawError::malformed("deflate tile does not decompress");
                }
                out.resize(std::min<size_t>(static_cast<size_t>(totalOut), need));
                std::vector<uint16_t> v = unpack(out.data(), out.size(), s.w, rows, samples, bits, le);
                // TIFF 预测器在解包之后，逐行逐样本平面。
                uint32_t predictor = t.tagUint(ifd, tag::PREDICTOR).value_or(1);
                if (predictor == 1) {
                    decoded[i] = std::move(v);
                } else if (predictor == 2) {
                    decoded[i] = undelta(v, samples, s.w * samples, rows, bits);
                } else if (predictor == 3) {
                    throw RawError::unsupported("floating-point predictor (compression 8, predictor 3)");
                } else {
                    throw RawError::malformed("unknown predictor " + std::to_string(predictor));
                }
#else
                (void)layout;
                throw RawError::unsupported("deflate raw data (zlib unavailable)");
#endif
                break;
            }
            case kJpegXl: {
                throw RawError::unsupported("JPEG XL-compressed DNG (DNG 1.7) is not decoded yet");
            }
            case 34892: {
                throw RawError::unsupported("lossy baseline JPEG tiles (DNG compression 34892) are not decoded yet");
            }
            default: {
                // 压缩 7（无损 JPEG）：熵流声明的帧受预算而非仅受段约束，
                // 下方 Quads 散布缓冲同预算。max_alloc 计字节，样本占两个。
                size_t need = s.w * rows * samples;
                uint64_t scatter = (layout == JpegLayout::Quads && samples == 1)
                                       ? static_cast<uint64_t>(s.w) * s.h * 2
                                       : 0;
                uint64_t budgetU64 = limits.maxAlloc > scatter ? (limits.maxAlloc - scatter) / 2 : 0;
                size_t budget = budgetU64 > static_cast<uint64_t>(SIZE_MAX) ? SIZE_MAX : static_cast<size_t>(budgetU64);
                size_t geom = s.w * s.h * samples * 4;
                auto [f, v] = ljpegDecode(src, s.len, std::min(geom, budget));
                if (v.size() < need) throw RawError::malformed("lossless JPEG tile is smaller than its tile");
                if (layout == JpegLayout::Quads && samples == 1 && f.components == 4 && f.width * 2 == s.w && f.height * 2 == s.h) {
                    std::vector<uint16_t> tile(s.w * s.h, 0);
                    for (size_t qi = 0; qi + 3 < v.size() / 4 * 4; qi += 4) {
                        size_t idx = qi / 4;
                        size_t at = 2 * (idx / f.width) * s.w + 2 * (idx % f.width);
                        tile[at] = v[qi];
                        tile[at + 1] = v[qi + 1];
                        tile[at + s.w] = v[qi + 2];
                        tile[at + s.w + 1] = v[qi + 3];
                    }
                    decoded[i] = std::move(tile);
                } else {
                    decoded[i] = std::move(v);
                }
                break;
            }
        }
    }
    std::vector<uint16_t> data(width * height * samples, 0);
    for (size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = segs[i];
        const std::vector<uint16_t>& d = *decoded[i];
        size_t rows = std::min(s.h, height - s.y);
        size_t cols = std::min(s.w, width - s.x);
        for (size_t r = 0; r < rows; ++r) {
            const uint16_t* srcRow = d.data() + r * s.w * samples;
            size_t at = ((s.y + r) * width + s.x) * samples;
            std::memcpy(data.data() + at, srcRow, cols * samples * sizeof(uint16_t));
        }
    }
    Plane plane;
    plane.width = width;
    plane.height = height;
    plane.samples = samples;
    plane.bits = bits;
    plane.data = std::move(data);
    return plane;
}

}  // namespace raw
}  // namespace montage
