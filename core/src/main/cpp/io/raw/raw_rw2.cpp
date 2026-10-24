// M23a：Panasonic/Leica RW2 位打包传感器布局（PanasonicRaw RawFormat 5）。
// 容器为 TIFF 变体（IIU\0）；IFD0 持 Panasonic 私有标签（ExifTool PanasonicRaw 表）。
// 打包方式由样本观察确立：RawDataOffset 起按 0x4000 字节页存，每页在 0x1FF8 处
// 切分且第二半在前（逻辑页 = 字节 0x1FF8..0x4000 后跟字节 0..0x1FF8）；逻辑流为
// 16 字节块序列，按小端 128 位数从最低位向上持 10 个 12 位（或 9 个 14 位）像素。
// 翻译自 photocraft crates/raw/src/rw2.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <cstring>

namespace montage {
namespace raw {

namespace {

constexpr uint16_t kSensorWidth = 0x0002;
constexpr uint16_t kSensorHeight = 0x0003;
constexpr uint16_t kSensorTopBorder = 0x0004;
constexpr uint16_t kSensorLeftBorder = 0x0005;
constexpr uint16_t kSensorBottomBorder = 0x0006;
constexpr uint16_t kSensorRightBorder = 0x0007;
constexpr uint16_t kRw2CfaPattern = 0x0009;
constexpr uint16_t kRw2BitsPerSample = 0x000A;
constexpr uint16_t kLinearityLimit[3] = {0x000E, 0x000F, 0x0010};
constexpr uint16_t kRw2BlackLevel[3] = {0x001C, 0x001D, 0x001E};
constexpr uint16_t kRw2WbLevel[3] = {0x0024, 0x0025, 0x0026};
constexpr uint16_t kRawFormat = 0x002D;
constexpr uint16_t kRawDataOffset = 0x0118;

// 存储流的页尺寸与切分点（观察值）。
constexpr size_t kRw2Page = 0x4000;
constexpr size_t kRw2Split = 0x1FF8;

// 逻辑字节 i 的物理偏移（相对 RawDataOffset）。
inline size_t rw2Physical(size_t i) {
    size_t page = i - i % kRw2Page;
    return page + (i % kRw2Page + kRw2Split) % kRw2Page;
}

// bits 位样本每 16 字节块的像素数。
size_t pixelsPerBlock(uint32_t bits) {
    switch (bits) {
        case 12: return 10;
        case 14: return 9;
        default: return 0;
    }
}

// 解包一个 16 字节块的 bits 位像素。
inline void unpackRw2Block(const uint8_t* b, uint32_t bits, uint16_t* out, size_t n) {
    uint64_t lo = 0, hi = 0;
    std::memcpy(&lo, b, 8);
    std::memcpy(&hi, b + 8, 8);
    uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    for (size_t i = 0; i < n; ++i) {
        uint32_t at = static_cast<uint32_t>(bits * i);
        if (at + bits > 128) break;
        uint64_t v;
        if (at == 0) {
            v = lo;
        } else if (at < 64) {
            v = (lo >> at) | (hi << (64 - at));
        } else {
            v = hi >> (at - 64);
        }
        out[i] = static_cast<uint16_t>(v & mask);
    }
}

}  // namespace

Sensor decodeRw2(const Tiff& t, const Limits& limits) {
    auto ifdOpt = t.ifdAt(t.firstIfd, 0);
    if (!ifdOpt) throw RawError::malformed("RW2 has no IFD0");
    const Ifd& ifd = *ifdOpt;
    std::optional<uint32_t> rawFormat = t.tagUint(ifd, kRawFormat);
    if (rawFormat != 5) {
        std::string f = rawFormat ? std::to_string(*rawFormat) : "unknown";
        throw RawError::unsupported("Panasonic RW2 raw format " + f + " (vendor-compressed) is not decoded yet");
    }
    auto wOpt = t.tagUint(ifd, kSensorWidth);
    auto hOpt = t.tagUint(ifd, kSensorHeight);
    if (!wOpt) throw RawError::malformed("RW2 has no sensor width");
    if (!hOpt) throw RawError::malformed("RW2 has no sensor height");
    size_t width = *wOpt, height = *hOpt;
    uint32_t bits = t.tagUint(ifd, kRw2BitsPerSample).value_or(12);
    size_t ppb = pixelsPerBlock(bits);
    if (ppb == 0) throw RawError::unsupported("Panasonic RW2 with " + std::to_string(bits) + "-bit samples");
    if (width % ppb != 0) {
        throw RawError::unsupported("Panasonic RW2 " + std::to_string(width) + " pixels wide (not whole " + std::to_string(ppb) + "-pixel blocks)");
    }
    limits.check(width, height, 2);
    const char* cfaName;
    switch (t.tagUint(ifd, kRw2CfaPattern).value_or(0)) {
        case 1: cfaName = "RGGB"; break;
        case 2: cfaName = "GRBG"; break;
        case 3: cfaName = "GBRG"; break;
        case 4: cfaName = "BGGR"; break;
        default: throw RawError::unsupported("Panasonic RW2 without a CFA pattern");
    }
    auto cfaOpt = Cfa::bayer(cfaName);
    if (!cfaOpt) throw RawError::malformed("bad CFA");
    std::optional<uint32_t> offsetTag = t.tagUint(ifd, kRawDataOffset);
    if (!offsetTag) offsetTag = t.tagUint(ifd, tag::STRIP_OFFSETS);
    if (!offsetTag) throw RawError::malformed("RW2 has no raw data offset");
    size_t offset = *offsetTag;
    size_t rowBytes = width / ppb * 16;
    if (offset > t.len) throw RawError::malformed("RW2 raw data lies outside the file");
    const uint8_t* src = t.data + offset;
    size_t srcLen = t.len - offset;
    size_t need = rowBytes * height;
    // 存整页；容忍短末页（缺字节读为 0）。
    if (srcLen + kRw2Page < need) throw RawError::malformed("RW2 raw data is truncated");
    auto half = [&](size_t l, uint8_t out[8]) {
        size_t phys = rw2Physical(l);
        for (int k = 0; k < 8; ++k) out[k] = phys + k < srcLen ? src[phys + k] : 0;
    };
    std::vector<uint16_t> data(width * height, 0);
    for (size_t y = 0; y < height; ++y) {
        uint16_t* out = data.data() + y * width;
        size_t at = y * rowBytes;
        for (size_t k = 0; k * ppb < width; ++k) {
            // SPLIT 与 PAGE 均为 8 的倍数，故每个 8 字节半连续。
            size_t l = at + k * 16;
            uint8_t b[16];
            half(l, b);
            half(l + 8, b + 8);
            unpackRw2Block(b, bits, out + k * ppb, ppb);
        }
    }

    Rect full = Rect::make(0, 0, width, height);
    auto level = [&](uint16_t tg) -> std::optional<double> {
        auto v = t.tagUint(ifd, tg);
        if (!v) return std::nullopt;
        return static_cast<double>(*v);
    };
    std::vector<std::string> warnings;
    BlackLevels black;
    {
        std::optional<double> bl[3];
        for (int i = 0; i < 3; ++i) bl[i] = level(kRw2BlackLevel[i]);
        if (bl[0] && bl[1] && bl[2]) {
            auto phase = cfaOpt->phase(0, 0);
            black.rows = 2;
            black.cols = 2;
            for (size_t k = 0; k < 4; ++k) {
                int c = std::min<int>(phase[k], 2);
                black.values.push_back(static_cast<float>(*bl[c]));
            }
        } else {
            warnings.push_back("RW2: black level not recorded; assumed 0");
            black = BlackLevels::uniform(0.0f);
        }
    }
    float max = static_cast<float>((1ull << bits) - 1);
    float white[3];
    for (int i = 0; i < 3; ++i) {
        auto v = level(kLinearityLimit[i]);
        white[i] = v && *v > 0.0 && *v <= max ? static_cast<float>(*v) : max;
    }
    std::optional<std::array<double, 3>> cameraWb;
    {
        std::optional<double> wb[3];
        for (int i = 0; i < 3; ++i) wb[i] = level(kRw2WbLevel[i]);
        if (wb[0] && wb[1] && wb[2] && *wb[1] > 0.0) {
            std::array<double, 3> m = {*wb[0] / *wb[1], 1.0, *wb[2] / *wb[1]};
            bool ok = true;
            for (double v : m)
                if (!(v >= 0.25 && v < 8.0)) ok = false;
            if (ok) cameraWb = m;
        }
    }
    Rect crop = full;
    {
        auto l = t.tagUint(ifd, kSensorLeftBorder);
        auto tp = t.tagUint(ifd, kSensorTopBorder);
        auto r = t.tagUint(ifd, kSensorRightBorder);
        auto b = t.tagUint(ifd, kSensorBottomBorder);
        if (l && tp && r && b && *r > *l && *b > *tp) {
            Rect rc = Rect::make(*l, *tp, *r - *l, *b - *tp).intersect(full);
            if (!rc.empty()) crop = rc;
        }
    }

    Sensor s;
    s.format = RawFormat::Rw2;
    s.make = t.tagAscii(ifd, tag::MAKE);
    s.model = t.tagAscii(ifd, tag::MODEL);
    s.width = width;
    s.height = height;
    s.samples = 1;
    s.data = std::move(data);
    s.cfa = std::move(*cfaOpt);
    s.linearization = std::nullopt;
    s.black = std::move(black);
    s.white[0] = white[0];
    s.white[1] = white[1];
    s.white[2] = white[2];
    s.active = full;
    s.crop = crop;
    s.color = ColorInfo{};
    s.cameraWb = cameraWb;
    s.orientation = orientationOf(t.tagUint(ifd, tag::ORIENTATION));
    s.baselineExposure = 0.0;
    s.gainMaps.clear();
    s.toneCurve.clear();
    s.warnings = std::move(warnings);
    return s;
}

}  // namespace raw
}  // namespace montage
