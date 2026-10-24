// M23a：Sony 压缩 ARW（cRAW, ARW 2）——固定码率有损编码，每像素 1 字节。
// 净室：来自公开格式描述（Dietz 2016、RawDigger/diglloyd 11+7 位方案）与样本观察。
// 翻译自 photocraft crates/raw/src/sony.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <algorithm>
#include <cstring>

namespace montage {
namespace raw {

namespace {

constexpr uint32_t kMax14 = 16383;

// 四结点色调曲线的 2048 项 码→14位值 表（SonyToneCurve）；结点不递增或越界为空。
struct SonyToneLut {
    std::vector<uint16_t> table;
};
std::optional<SonyToneLut> sonyToneLut(const std::vector<uint32_t>& knots) {
    if (knots.size() != 4) return std::nullopt;
    uint32_t pts[6] = {0, knots[0], knots[1], knots[2], knots[3], 1u << 14};
    for (int i = 0; i < 5; ++i)
        if (pts[i] > pts[i + 1]) return std::nullopt;
    SonyToneLut out;
    out.table.resize(2048);
    for (uint32_t code = 0; code < 2048; ++code) {
        // 结点尺度上码是 8 × code；第一段映到 2 × code（缩放位置的四分之一）。
        uint32_t x = code * 8;
        uint32_t y = 0;
        for (int seg = 0; seg < 5; ++seg) {
            if (x > pts[seg]) {
                y += (std::min(x, pts[seg + 1]) - pts[seg]) << seg;
            }
        }
        out.table[code] = static_cast<uint16_t>(std::min<uint32_t>(y / 4, kMax14));
    }
    return out;
}

// 一个 16 字节块解码为 16 个码。
inline std::array<uint16_t, 16> decodeCrBlock(const uint8_t* b) {
    uint64_t lo = 0, hi = 0;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    auto field = [&](uint32_t at, uint32_t bits) -> uint16_t {
        // 128 位小端数的 at 位起 bits 位（可跨 64 位边界）。
        uint64_t v;
        if (at == 0) {
            v = lo;
        } else if (at < 64) {
            v = (lo >> at) | (hi << (64 - at));
        } else {
            v = hi >> (at - 64);
        }
        return static_cast<uint16_t>(v & ((bits >= 64 ? ~0ull : (1ull << bits) - 1)));
    };
    uint16_t max = field(0, 11);
    uint16_t min = field(11, 11);
    size_t imax = field(22, 4);
    size_t imin = field(26, 4);
    uint16_t range = static_cast<uint16_t>(max > min ? max - min : 0);
    uint32_t sh = 0;
    while ((128u << sh) <= range) ++sh;
    std::array<uint16_t, 16> out{};
    uint32_t at = 30;
    for (size_t i = 0; i < 16; ++i) {
        if (i == imax) {
            out[i] = max;
        } else if (i == imin) {
            out[i] = min;
        } else if (at + 7 <= 128) {
            uint16_t d = field(at, 7);
            at += 7;
            out[i] = static_cast<uint16_t>(std::min<uint32_t>(min + (static_cast<uint32_t>(d) << sh), 2047));
        } else {
            // 仅当损坏块两次指名同一位置。
            out[i] = min;
        }
    }
    return out;
}

}  // namespace

// raw IFD 持 cRAW 数据（Sony 私有压缩，每像素 1 字节）时为真。
bool isCr(const Tiff& t, const Ifd& ifd) {
    if (t.tagUint(ifd, tag::COMPRESSION) != 32767) return false;
    uint32_t w = t.tagUint(ifd, tag::IMAGE_WIDTH).value_or(0);
    uint32_t h = t.tagUint(ifd, tag::IMAGE_LENGTH).value_or(0);
    uint64_t bytes = 0;
    for (uint32_t c : t.tagUints(ifd, tag::STRIP_BYTE_COUNTS)) bytes += c;
    std::optional<uint32_t> kind = t.tagUint(ifd, kSonyRawFileType);
    if (kind) return *kind == 2;
    return bytes > 0 && bytes == static_cast<uint64_t>(w) * h;
}

// 读 raw IFD 的 cRAW 图像。
Plane readSonyCr(const Tiff& t, const Ifd& ifd, const Limits& limits) {
    auto wOpt = t.tagUint(ifd, tag::IMAGE_WIDTH);
    auto hOpt = t.tagUint(ifd, tag::IMAGE_LENGTH);
    if (!wOpt) throw RawError::malformed("raw image has no width");
    if (!hOpt) throw RawError::malformed("raw image has no height");
    size_t width = *wOpt, height = *hOpt;
    if (width % 32 != 0) {
        throw RawError::unsupported("Sony compressed ARW " + std::to_string(width) + " pixels wide (not a multiple of 32)");
    }
    auto lutOpt = sonyToneLut(t.tagUints(ifd, kSonyToneCurve));
    if (!lutOpt) {
        throw RawError::unsupported("Sony compressed ARW without a usable tone curve (0x7010)");
    }
    const std::vector<uint16_t>& lut = lutOpt->table;
    limits.check(width, height, 2);
    // 每像素 1 字节：在 strip 中定位每行。
    std::vector<const uint8_t*> rows;
    rows.reserve(height);
    for (const Segment& s : segmentsOf(t, ifd, width, height)) {
        size_t n = std::min(s.h, height - s.y);
        for (size_t r = 0; r < n; ++r) {
            size_t at = s.offset + r * width;
            if ((r + 1) * width > s.len) throw RawError::malformed("compressed ARW strip is shorter than its rows");
            if (at > t.len || width > t.len - at) throw RawError::malformed("compressed ARW data lies outside the file");
            rows.push_back(t.data + at);
        }
    }
    if (rows.size() != height) throw RawError::malformed("compressed ARW strips do not cover the image");
    std::vector<uint16_t> data(width * height, 0);
    for (size_t y = 0; y < height; ++y) {
        const uint8_t* src = rows[y];
        uint16_t* out = data.data() + y * width;
        for (size_t group = 0; group + 31 < width; group += 32) {
            std::array<uint16_t, 16> even = decodeCrBlock(src + group);
            std::array<uint16_t, 16> odd = decodeCrBlock(src + group + 16);
            for (size_t k = 0; k < 16; ++k) {
                out[group + 2 * k] = lut[even[k]];
                out[group + 2 * k + 1] = lut[odd[k]];
            }
        }
    }
    Plane plane;
    plane.width = width;
    plane.height = height;
    plane.samples = 1;
    plane.bits = 14;
    plane.data = std::move(data);
    return plane;
}

}  // namespace raw
}  // namespace montage
