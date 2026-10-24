// M23a：DNG（Adobe DNG Specification 1.7）解码——raw IFD 选择、CFA 描述、
// 线性化、黑/白电平、活动区、默认裁剪、颜色校准与白平衡。
// 翻译自 photocraft crates/raw/src/dng.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include "io/raw/raw_color.h"
#include "io/raw/raw_opcodes.h"

namespace montage {
namespace raw {

namespace {

constexpr uint32_t kPhotometricCfa = 32803;
constexpr uint32_t kPhotometricLinearRaw = 34892;

// 主 raw 图像：NewSubFileType 0 且 CFA 或 LinearRaw 数据，大者优先。
std::optional<Ifd> rawIfd(const Tiff& t, const std::vector<Ifd>& ifds) {
    const Ifd* best = nullptr;
    uint64_t bestArea = 0;
    for (const Ifd& i : ifds) {
        uint32_t subfile = t.tagUint(i, tag::NEW_SUBFILE_TYPE).value_or(0);
        if (subfile != 0) continue;
        uint32_t photo = t.tagUint(i, tag::PHOTOMETRIC).value_or(0);
        if (photo != kPhotometricCfa && photo != kPhotometricLinearRaw) continue;
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

std::optional<Mat3> matrixAt(const Tiff& t, const Ifd& ifd, uint16_t tg) {
    std::vector<double> v = t.tagFloats(ifd, tg);
    if (v.size() != 9) return std::nullopt;
    return matFromSlice(v.data());
}

std::optional<std::array<double, 3>> tripleAt(const Tiff& t, const Ifd& ifd, uint16_t tg) {
    std::vector<double> v = t.tagFloats(ifd, tg);
    if (v.size() != 3) return std::nullopt;
    for (double x : v)
        if (!std::isfinite(x) || x <= 0.0) return std::nullopt;
    return std::array<double, 3>{v[0], v[1], v[2]};
}

// IFD0 的颜色校准。
ColorInfo colorInfo(const Tiff& t, const Ifd& ifd0) {
    ColorInfo info;
    const std::pair<std::array<uint16_t, 5>, double> groups[2] = {
        {{tag::COLOR_MATRIX_1, tag::CAMERA_CALIBRATION_1, tag::FORWARD_MATRIX_1, tag::CALIBRATION_ILLUMINANT_1, 0}, 2856.0},
        {{tag::COLOR_MATRIX_2, tag::CAMERA_CALIBRATION_2, tag::FORWARD_MATRIX_2, tag::CALIBRATION_ILLUMINANT_2, 0}, 6504.0},
    };
    for (const auto& g : groups) {
        auto colorMatrix = matrixAt(t, ifd0, g.first[0]);
        if (!colorMatrix) continue;
        if (!matInvert(*colorMatrix)) continue;
        double temperature = g.second;
        if (auto ill = t.tagUint(ifd0, g.first[3])) {
            if (auto temp = illuminantTemperature(*ill)) temperature = *temp;
        }
        Calibration cal;
        cal.temperature = temperature;
        cal.colorMatrix = *colorMatrix;
        if (auto fm = matrixAt(t, ifd0, g.first[2])) {
            if (matInvert(*fm)) cal.forwardMatrix = fm;
        }
        if (auto cc = matrixAt(t, ifd0, g.first[1])) {
            if (matInvert(*cc)) cal.cameraCalibration = *cc;
        }
        info.calibrations.push_back(cal);
    }
    std::sort(info.calibrations.begin(), info.calibrations.end(),
              [](const Calibration& a, const Calibration& b) { return a.temperature < b.temperature; });
    if (info.calibrations.size() == 2 && info.calibrations[0].forwardMatrix.has_value() != info.calibrations[1].forwardMatrix.has_value()) {
        for (Calibration& c : info.calibrations) c.forwardMatrix.reset();
    }
    {
        std::vector<double> v = t.tagFloats(ifd0, tag::AS_SHOT_WHITE_XY);
        if (v.size() == 2 && v[0] > 0.0 && v[0] < 1.0 && v[1] > 0.0 && v[1] < 1.0) {
            info.asShotWhiteXY = std::array<double, 2>{v[0], v[1]};
        }
    }
    info.analogBalance = tripleAt(t, ifd0, tag::ANALOG_BALANCE);
    info.asShotNeutral = tripleAt(t, ifd0, tag::AS_SHOT_NEUTRAL);
    return info;
}

// DNG raw IFD 的 CFA 图案，锚定在活动区原点。
Cfa cfaOf(const Tiff& t, const Ifd& ifd, const Rect& active) {
    std::vector<uint32_t> dim = t.tagUints(ifd, tag::CFA_REPEAT_PATTERN_DIM);
    size_t rows = 2, cols = 2;
    if (dim.size() == 2) {
        rows = dim[0];
        cols = dim[1];
    }
    std::vector<uint32_t> pattern = t.tagUints(ifd, tag::CFA_PATTERN);
    if (rows == 0 || cols == 0 || rows > 16 || cols > 16 || pattern.size() != rows * cols) {
        throw RawError::malformed("bad CFA pattern");
    }
    std::vector<uint32_t> planes = t.tagUints(ifd, tag::CFA_PLANE_COLOR);
    if (planes.empty()) planes = {0, 1, 2};
    if (planes.size() != 3 || planes[0] != 0 || planes[1] != 1 || planes[2] != 2) {
        throw RawError::unsupported("CFA plane colours other than red, green and blue");
    }
    if (t.tagUint(ifd, tag::CFA_LAYOUT).value_or(1) != 1) {
        throw RawError::unsupported("non-rectangular CFA layout");
    }
    Cfa c;
    c.width = cols;
    c.height = rows;
    c.colors.reserve(pattern.size());
    for (uint32_t code : pattern) {
        if (code > 2) throw RawError::unsupported("CFA colours other than red, green and blue");
        c.colors.push_back(static_cast<uint8_t>(code));
    }
    c.originX = active.x;
    c.originY = active.y;
    if (!c.isBayer()) {
        throw RawError::unsupported(std::to_string(rows) + "x" + std::to_string(cols) + " non-Bayer CFA (e.g. X-Trans)");
    }
    return c;
}

Rect rectFrom(const Tiff& t, const Ifd& ifd, size_t width, size_t height) {
    Rect full = Rect::make(0, 0, width, height);
    std::vector<uint32_t> a = t.tagUints(ifd, tag::ACTIVE_AREA);
    if (a.size() == 4 && a[2] > a[0] && a[3] > a[1]) {
        return Rect::make(a[1], a[0], a[3] - a[1], a[2] - a[0]).intersect(full);
    }
    return full;
}

size_t cropValue(double value, const char* name) {
    if (!std::isfinite(value) || value < 0.0) {
        throw RawError::malformed(std::string("invalid DNG ") + name);
    }
    double rounded = std::round(value);
    if (rounded >= 4294967296.0) {
        throw RawError::malformed(std::string("DNG ") + name + " is not representable");
    }
    return static_cast<size_t>(rounded);
}

}  // namespace

Sensor decodeDng(const Tiff& t, const Limits& limits) {
    std::vector<Ifd> ifds = t.allIfds();
    auto ifd0Opt = t.ifdAt(t.firstIfd, 0);
    if (!ifd0Opt) throw RawError::malformed("DNG has no IFD0");
    const Ifd& ifd0 = *ifd0Opt;
    auto rawOpt = rawIfd(t, ifds);
    if (!rawOpt) throw RawError::malformed("DNG has no raw image");
    const Ifd& raw = *rawOpt;
    std::vector<std::string> warnings;
    uint32_t photometric = t.tagUint(raw, tag::PHOTOMETRIC).value_or(0);
    Plane plane = readPlane(t, raw, limits, JpegLayout::Flat);
    size_t samples = plane.samples;
    bool isCfa = photometric == kPhotometricCfa;
    if (isCfa && samples != 1) {
        throw RawError::unsupported("CFA data with " + std::to_string(samples) + " samples per pixel");
    }
    // LinearRaw：三个颜色样本，或单色相机/扫描仪的一个样本（DNG 1.7 ColorPlanes 1）。
    if (!isCfa && samples != 1 && samples != 3) {
        throw RawError::unsupported("LinearRaw data with " + std::to_string(samples) + " samples per pixel");
    }
    Rect active = rectFrom(t, raw, plane.width, plane.height);
    if (active.empty()) throw RawError::malformed("empty active area");
    std::optional<Cfa> cfa;
    if (isCfa) cfa = cfaOf(t, raw, active);

    // 默认裁剪，相对活动区。每个标签有自己的默认值（DNG 1.7.1.0）。
    Rect crop = active;
    if (raw.has(tag::DEFAULT_CROP_ORIGIN) || raw.has(tag::DEFAULT_CROP_SIZE)) {
        // 出现的标签必须有两个值；缺失的取默认。
        auto pair = [&](uint16_t tg, std::array<double, 2> def) -> std::optional<std::array<double, 2>> {
            if (!raw.has(tg)) return def;
            std::vector<double> v = t.tagFloats(raw, tg);
            if (v.size() == 2) return std::array<double, 2>{v[0], v[1]};
            return std::nullopt;
        };
        auto origin = pair(tag::DEFAULT_CROP_ORIGIN, {0.0, 0.0});
        auto size = pair(tag::DEFAULT_CROP_SIZE, {static_cast<double>(plane.width), static_cast<double>(plane.height)});
        if (origin && size) {
            // cropValue 失败必须传播 Malformed（上游 ?），不吞成警告。
            size_t ox = cropValue((*origin)[0], "DefaultCropOrigin");
            size_t oy = cropValue((*origin)[1], "DefaultCropOrigin");
            size_t cw = cropValue((*size)[0], "DefaultCropSize");
            size_t ch = cropValue((*size)[1], "DefaultCropSize");
            bool inRange = active.x <= SIZE_MAX - ox && active.y <= SIZE_MAX - oy;
            if (inRange) {
                size_t x = active.x + ox, y = active.y + oy;
                size_t right = x + cw, bottom = y + ch;
                size_t aRight = active.x + active.width, aBottom = active.y + active.height;
                if (x >= active.x && y >= active.y && right <= aRight && bottom <= aBottom && cw > 0 && ch > 0) {
                    crop = Rect::make(x, y, cw, ch);
                } else {
                    warnings.push_back("out-of-range or empty DNG default crop; active area used");
                }
            } else {
                warnings.push_back("out-of-range or empty DNG default crop; active area used");
            }
        } else {
            warnings.push_back("incomplete DNG default crop; active area used");
        }
    }
    {
        std::vector<double> scale = t.tagFloats(raw, tag::DEFAULT_SCALE);
        if (scale.size() == 2 && std::abs(scale[0] - scale[1]) > 1e-6) {
            warnings.push_back("non-square pixels (DefaultScale) are not resampled");
        }
    }
    // opcode 列表：OpcodeList2 增益图（镜头阴影）应用；其余报告。
    std::vector<GainMap> gainMaps;
    const std::pair<uint16_t, int> opLists[3] = {{tag::OPCODE_LIST_1, 1}, {tag::OPCODE_LIST_2, 2}, {tag::OPCODE_LIST_3, 3}};
    for (const auto& op : opLists) {
        const Entry* e = raw.get(op.first);
        if (!e) continue;
        size_t l = 0;
        const uint8_t* bytes = t.raw(*e, l);
        if (!bytes) continue;
        OpcodeListResult r = parseOpcodeList(bytes, l);
        if (op.second == 2) {
            gainMaps = std::move(r.maps);
        } else if (!r.maps.empty()) {
            warnings.push_back("DNG OpcodeList" + std::to_string(op.second) + ": GainMap is not applied");
        }
        if (!r.skipped.empty()) {
            std::string joined;
            for (const std::string& s : r.skipped) {
                if (!joined.empty()) joined += ", ";
                joined += s;
            }
            warnings.push_back("DNG OpcodeList" + std::to_string(op.second) + ": " + joined + " not applied");
        }
    }
    // DNG 1.6 ProfileGainTableMap：手机的局部色调映射；缺它阴影偏暗。
    if (ifd0.has(tag::PROFILE_GAIN_TABLE_MAP) || raw.has(tag::PROFILE_GAIN_TABLE_MAP)) {
        warnings.push_back("DNG ProfileGainTableMap (local tone mapping) is not applied");
    }

    float max = static_cast<float>((1ull << std::min<uint32_t>(plane.bits, 31)) - 1);
    std::optional<std::vector<uint16_t>> linearization;
    {
        const Entry* e = raw.get(tag::LINEARIZATION_TABLE);
        if (e) {
            std::vector<uint32_t> v = t.uints(*e);
            if (!v.empty() && v.size() <= 65536) {
                std::vector<uint16_t> lv;
                lv.reserve(v.size());
                for (uint32_t x : v) lv.push_back(static_cast<uint16_t>(std::min<uint32_t>(x, 65535)));
                linearization = std::move(lv);
            }
        }
    }
    float whiteDefault = max;
    if (linearization) {
        uint16_t linMax = 0;
        for (uint16_t v : *linearization) linMax = std::max(linMax, v);
        whiteDefault = static_cast<float>(linMax);
    }
    float white[3] = {whiteDefault, whiteDefault, whiteDefault};
    {
        std::vector<double> wl = t.tagFloats(raw, tag::WHITE_LEVEL);
        for (int s = 0; s < 3; ++s) {
            const double* v = nullptr;
            if (!wl.empty()) v = &wl[wl.size() > 1 ? std::min<size_t>(s, wl.size() - 1) : 0];
            if (v && *v > 0.0) white[s] = static_cast<float>(*v);
        }
    }

    size_t rows = 1, cols = 1;
    {
        std::vector<uint32_t> dim = t.tagUints(raw, tag::BLACK_LEVEL_REPEAT_DIM);
        if (dim.size() == 2 && dim[0] >= 1 && dim[0] <= 16 && dim[1] >= 1 && dim[1] <= 16) {
            rows = dim[0];
            cols = dim[1];
        }
    }
    std::vector<double> bl = t.tagFloats(raw, tag::BLACK_LEVEL);
    BlackLevels black;
    if (bl.size() == rows * cols * samples) {
        black.rows = rows;
        black.cols = cols;
        black.values.reserve(bl.size());
        for (double v : bl) black.values.push_back(static_cast<float>(v));
    } else {
        black = BlackLevels::uniform(bl.empty() ? 0.0f : static_cast<float>(bl[0]));
    }
    {
        std::vector<double> dh = t.tagFloats(raw, tag::BLACK_LEVEL_DELTA_H);
        if (dh.size() == active.width) {
            black.deltaH.reserve(dh.size());
            for (double v : dh) black.deltaH.push_back(static_cast<float>(v));
        }
        std::vector<double> dv = t.tagFloats(raw, tag::BLACK_LEVEL_DELTA_V);
        if (dv.size() == active.height) {
            black.deltaV.reserve(dv.size());
            for (double v : dv) black.deltaV.push_back(static_cast<float>(v));
        }
    }

    ColorInfo color = colorInfo(t, ifd0);
    // 单色图像无颜色可校准：显影为中性灰。
    if (color.calibrations.empty() && (isCfa || samples != 1)) {
        warnings.push_back("the DNG has no colour matrix; colours are approximate");
    }
    double baselineExposure = 0.0;
    {
        std::vector<double> v = t.tagFloats(ifd0, tag::BASELINE_EXPOSURE);
        if (!v.empty()) baselineExposure += v[0];
        v = t.tagFloats(raw, tag::BASELINE_EXPOSURE_OFFSET);
        if (!v.empty()) baselineExposure += v[0];
    }
    if (!std::isfinite(baselineExposure)) baselineExposure = 0.0;
    baselineExposure = std::clamp(baselineExposure, -10.0, 10.0);

    Sensor s;
    s.format = RawFormat::Dng;
    s.make = t.tagAscii(ifd0, tag::MAKE);
    s.model = t.tagAscii(ifd0, tag::UNIQUE_CAMERA_MODEL);
    if (!s.model) s.model = t.tagAscii(ifd0, tag::MODEL);
    s.width = plane.width;
    s.height = plane.height;
    s.samples = samples;
    s.data = std::move(plane.data);
    s.cfa = std::move(cfa);
    s.linearization = std::move(linearization);
    s.black = std::move(black);
    s.white[0] = white[0];
    s.white[1] = white[1];
    s.white[2] = white[2];
    s.active = active;
    s.crop = crop;
    s.color = std::move(color);
    s.cameraWb = std::nullopt;
    s.orientation = orientationOf(t.tagUint(ifd0, tag::ORIENTATION));
    s.baselineExposure = baselineExposure;
    s.gainMaps = std::move(gainMaps);
    s.toneCurve.clear();
    s.warnings = std::move(warnings);
    return s;
}

}  // namespace raw
}  // namespace montage
