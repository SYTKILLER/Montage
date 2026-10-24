// M23a：TIFF/EP 结构 raw（NEF、ARW、PEF 等），传感器数据未压缩或无损 JPEG，
// 存于标准 CFA IFD（TIFF/EP, ISO 12234-2: PhotometricInterpretation 32803、
// CFARepeatPatternDim、CFAPattern）；另接 Sony cRAW 与 Nikon Huffman NEF。
// 黑/白电平：DNG 风格标签优先，然后厂商公开标签（Nikon maker note BlackLevel、
// Sony raw-IFD BlackLevel），否则 0 + 警告；白电平回退数据剪裁点。
// as-shot 白平衡来自公开厂商标签，否则估计。
// 翻译自 photocraft crates/raw/src/tiffep.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <algorithm>

#include "io/raw/raw_ljpeg.h"

namespace montage {
namespace raw {

namespace {

constexpr uint32_t kPhotometricCfa = 32803;
constexpr uint16_t kNikonWbRbLevels = 0x000C;
constexpr uint16_t kNikonBlackLevel = 0x003D;
constexpr uint16_t kSonyBlackLevel = 0x7310;
constexpr uint16_t kSonyWbRggbLevels = 0x7313;

std::optional<Ifd> rawIfdOf(const Tiff& t, const std::vector<Ifd>& ifds) {
    const Ifd* best = nullptr;
    uint64_t bestArea = 0;
    for (const Ifd& i : ifds) {
        if (t.tagUint(i, tag::PHOTOMETRIC) != kPhotometricCfa) continue;
        uint64_t area = static_cast<uint64_t>(t.tagUint(i, tag::IMAGE_WIDTH).value_or(0)) *
                        static_cast<uint64_t>(t.tagUint(i, tag::IMAGE_LENGTH).value_or(0));
        if (!best || area > bestArea) {
            best = &i;
            bestArea = area;
        }
    }
    if (!best) return std::nullopt;
    return *best;
}

// Sony 减尺寸无损 "M"/"S" YCbCr 变体：识别但不解码。
bool isSonyNonCfa(const Tiff& t, const std::vector<Ifd>& ifds) {
    bool hasType = false;
    for (const Ifd& i : ifds)
        if (i.has(kSonyRawFileType)) hasType = true;
    bool hasSony = false;
    for (const Ifd& i : ifds) {
        auto m = t.tagAscii(i, tag::MAKE);
        if (m && m->size() >= 4) {
            std::string up = *m;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (up.rfind("SONY", 0) == 0) hasSony = true;
        }
    }
    return hasType && hasSony;
}

// Nikon maker note："Nikon\0"、版本，然后内嵌 TIFF 头，偏移相对自身。
struct NikonNote {
    Tiff inner;
    Ifd ifd;
};
std::optional<NikonNote> nikonMakerNote(const Tiff& t, const std::vector<Ifd>& ifds) {
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
        return NikonNote{*inner, *ifd};
    }
    return std::nullopt;
}

}  // namespace

// R、G、G、B 顺序的四个电平，按 2×2 CFA 位置（数据奇偶）放置。
std::optional<std::array<float, 4>> rggbByPosition(const Cfa& cfa, const std::vector<double>& v) {
    if (v.size() != 4) return std::nullopt;
    double vals[4] = {v[0], v[1], v[2], v[3]};
    for (double x : vals)
        if (!std::isfinite(x) || x < 0.0) return std::nullopt;
    double greens[2] = {vals[1], vals[2]};
    int gi = 0;
    std::array<float, 4> out{};
    for (size_t k = 0; k < 4; ++k) {
        double slot;
        uint8_t c = cfa.color(k & 1, k >> 1);
        if (c == 0) {
            slot = vals[0];
        } else if (c == 2) {
            slot = vals[3];
        } else {
            slot = greens[gi++ % 2];
        }
        out[k] = static_cast<float>(slot);
    }
    return out;
}

// raw IFD 的 TIFF/EP 标签 CFA，或 EXIF CFAPattern。
std::optional<Cfa> tiffEpCfa(const Tiff& t, const Ifd& raw, const std::vector<Ifd>& ifds) {
    std::vector<uint32_t> dim = t.tagUints(raw, tag::CFA_REPEAT_PATTERN_DIM);
    std::vector<uint32_t> pat = t.tagUints(raw, tag::CFA_PATTERN);
    size_t rows = 0, cols = 0;
    std::vector<uint32_t> colors;
    if (dim.size() == 2 && pat.size() == static_cast<size_t>(dim[0]) * dim[1] && !pat.empty()) {
        rows = dim[0];
        cols = dim[1];
        colors = pat;
    } else {
        // EXIF CFAPattern：两个 16 位计数（列、行）然后颜色。
        const Entry* e = nullptr;
        for (const Ifd& i : ifds) {
            const Entry* c = i.get(tag::EXIF_CFA_PATTERN);
            if (c) {
                e = c;
                break;
            }
        }
        if (!e) return std::nullopt;
        size_t l = 0;
        const uint8_t* b = t.raw(*e, l);
        if (!b || l < 4) return std::nullopt;
        auto rd = [&](size_t o) -> std::optional<uint16_t> {
            if (o + 2 > l) return std::nullopt;
            return t.le ? static_cast<uint16_t>(b[o] | b[o + 1] << 8) : static_cast<uint16_t>(b[o] << 8 | b[o + 1]);
        };
        auto cOpt = rd(0);
        auto rOpt = rd(2);
        if (!cOpt || !rOpt) return std::nullopt;
        cols = *cOpt;
        rows = *rOpt;
        if (cols > SIZE_MAX / rows) return std::nullopt;
        if (l < 4 + rows * cols) return std::nullopt;
        for (size_t k = 0; k < rows * cols; ++k) colors.push_back(b[4 + k]);
    }
    if (rows == 0 || cols == 0 || rows > 16 || cols > 16) return std::nullopt;
    Cfa c;
    c.width = cols;
    c.height = rows;
    c.originX = 0;
    c.originY = 0;
    c.colors.reserve(colors.size());
    for (uint32_t v : colors) {
        if (v > 2) return std::nullopt;
        c.colors.push_back(static_cast<uint8_t>(v));
    }
    return c;
}

namespace {

// 尾部填充列的宽度（至多 128），保持 CFA 相位取偶。填充列 ≥90% 的行持末列值，
// 且该值偏暗（低于 2^bits 的四分之一）——剪到白的图像边不会被误判为填充。
size_t paddedWidth(const std::vector<uint16_t>& data, size_t w, size_t h, uint32_t bits) {
    if (w < 256 || h < 2 || data.size() < w * h) return w;
    uint16_t fill = data[(h / 2) * w + w - 1];
    if (static_cast<uint32_t>(fill) >= ((1u << std::min<uint32_t>(bits, 16)) / 4)) return w;
    auto padding = [&](size_t x) {
        size_t count = 0;
        for (size_t y = 0; y < h; ++y)
            if (data[y * w + x] == fill) ++count;
        return count * 10 >= h * 9;
    };
    size_t aw = w;
    while (aw > w - 128 && padding(aw - 1)) --aw;
    return aw & ~static_cast<size_t>(1);
}

}  // namespace

Sensor decodeTiffEp(const Tiff& t, RawFormat format, const Limits& limits) {
    std::vector<Ifd> ifds = t.allIfds();
    auto rawOpt = rawIfdOf(t, ifds);
    if (!rawOpt) {
        if (isSonyNonCfa(t, ifds)) throw RawError::unsupported("Sony reduced-size (M / S) lossless ARW");
        throw RawError::unsupported(std::string(rawFormatName(format)) + ": no CFA image found");
    }
    const Ifd& raw = *rawOpt;
    auto nikon = format == RawFormat::Nef ? nikonMakerNote(t, ifds) : std::nullopt;
    bool craw = format == RawFormat::Arw && isCr(t, raw);
    Plane plane = [&] {
        if (craw) return readSonyCr(t, raw, limits);
        if (format == RawFormat::Nef && t.tagUint(raw, tag::COMPRESSION) == kNikonCompression) {
            return readNefcCompressed(t, raw, limits);
        }
        return readPlane(t, raw, limits, JpegLayout::Quads);
    }();
    if (plane.samples != 1) {
        throw RawError::unsupported("CFA data with " + std::to_string(plane.samples) + " samples per pixel");
    }
    auto cfaOpt = tiffEpCfa(t, raw, ifds);
    if (!cfaOpt) {
        throw RawError::unsupported(std::string(rawFormatName(format)) + ": CFA pattern not recorded");
    }
    Cfa cfa = *cfaOpt;
    if (!cfa.isBayer()) {
        throw RawError::unsupported(std::to_string(cfa.height) + "x" + std::to_string(cfa.width) + " non-Bayer CFA");
    }
    std::vector<std::string> warnings;
    // Nikon 在部分机身右侧填充传感器行；它们不是图像数据。
    size_t width = format == RawFormat::Nef ? paddedWidth(plane.data, plane.width, plane.height, plane.bits) : plane.width;
    Rect full = Rect::make(0, 0, width, plane.height);
    std::optional<std::string> make, model;
    for (const Ifd& i : ifds) {
        if (!make) make = t.tagAscii(i, tag::MAKE);
        if (!model) model = t.tagAscii(i, tag::MODEL);
    }

    std::vector<double> bl = t.tagFloats(raw, tag::BLACK_LEVEL);
    std::vector<double> vendorBlack;
    if (nikon) {
        // Nikon 的 BlackLevel 按 14 位单位记录，无论样本位深：
        // 12 位文件的 600/400/1008 对应 darkest ≈ 四分之一。
        vendorBlack = nikon->inner.tagFloats(nikon->ifd, kNikonBlackLevel);
        if (plane.bits == 12) {
            for (double& v : vendorBlack) v /= 4.0;
        }
    } else {
        vendorBlack = t.tagFloats(raw, kSonyBlackLevel);
    }
    BlackLevels black;
    if (!bl.empty()) {
        black = BlackLevels::uniform(static_cast<float>(bl[0]));
    } else if (auto v = rggbByPosition(cfa, vendorBlack)) {
        black.rows = 2;
        black.cols = 2;
        black.values.assign(v->begin(), v->end());
    } else if (craw) {
        // 第一代 cRAW 机身无 BlackLevel 标签；色调曲线把黑码映射到 512。
        black = BlackLevels::uniform(512.0f);
    } else if (nikon) {
        // 无 BlackLevel 标签的 Nikon 机身在机内减黑。
        black = BlackLevels::uniform(0.0f);
    } else {
        warnings.push_back(std::string(rawFormatName(format)) + ": black level not recorded in a documented tag; assumed 0");
        black = BlackLevels::uniform(0.0f);
    }
    float white;
    {
        std::vector<double> wl = t.tagFloats(raw, tag::WHITE_LEVEL);
        if (!wl.empty() && wl[0] > 0.0) {
            white = static_cast<float>(wl[0]);
        } else {
            white = clipLevel(plane.data, plane.bits);
        }
    }
    std::optional<std::array<double, 3>> cameraWb;
    if (nikon) {
        std::vector<double> wb = nikon->inner.tagFloats(nikon->ifd, kNikonWbRbLevels);
        if (wb.size() >= 2) {
            std::array<double, 3> m = {wb[0], 1.0, wb[1]};
            bool ok = true;
            for (double v : m)
                if (!(v >= 0.25 && v < 8.0)) ok = false;
            if (ok) cameraWb = m;
        }
    } else {
        std::vector<double> wb = t.tagFloats(raw, kSonyWbRggbLevels);
        if (wb.size() == 4 && wb[1] + wb[2] > 0.0) {
            std::array<double, 3> m = {2.0 * wb[0] / (wb[1] + wb[2]), 1.0, 2.0 * wb[3] / (wb[1] + wb[2])};
            bool ok = true;
            for (double v : m)
                if (!(v >= 0.25 && v < 8.0)) ok = false;
            if (ok) cameraWb = m;
        }
    }
    // DNG 风格 DefaultCropOrigin / DefaultCropSize（部分厂商写在 raw IFD）。
    Rect crop = full;
    {
        std::vector<double> origin = t.tagFloats(raw, tag::DEFAULT_CROP_ORIGIN);
        std::vector<double> size = t.tagFloats(raw, tag::DEFAULT_CROP_SIZE);
        if (origin.size() == 2 && size.size() == 2) {
            auto f = [](double v) { return (std::isfinite(v) && v >= 0.0) ? static_cast<size_t>(std::round(v)) : 0; };
            Rect r = Rect::make(f(origin[0]), f(origin[1]), f(size[0]), f(size[1])).intersect(full);
            if (!r.empty()) crop = r;
        }
    }

    Sensor s;
    s.format = format;
    s.make = make;
    s.model = model;
    s.width = plane.width;
    s.height = plane.height;
    s.samples = 1;
    s.data = std::move(plane.data);
    s.cfa = std::move(cfa);
    s.linearization = std::nullopt;
    s.black = std::move(black);
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = full;
    s.crop = crop;
    s.color = ColorInfo{};
    s.cameraWb = cameraWb;
    s.orientation = orientationOf(t.tagUint(ifds.empty() ? Ifd{} : ifds[0], tag::ORIENTATION));
    s.baselineExposure = 0.0;
    s.gainMaps.clear();
    s.toneCurve.clear();
    s.warnings = std::move(warnings);
    applyCameraProfile(s);
    return s;
}

}  // namespace raw
}  // namespace montage
