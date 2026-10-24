// M23a：Canon CR2 解码。
// 翻译自 photocraft crates/raw/src/cr2.rs（MIT OR Apache-2.0）。
// CR2 是头 8–15 字节携带 `CR` + 版本 + raw IFD（IFD3）偏移的 TIFF。raw IFD 存一条
// 无损 JPEG（T.81 process 14），解码样本按竖直切片填充传感器图像：私有标签 0xC640
// 给切片数、切片宽与末切片宽；切片 0 逐行填满，然后切片 1，依此。sRAW/mRAW 不支持。
// 几何与 as-shot 白平衡来自 Canon maker note（0x00E0 SensorInfo、0x4001 ColorData）。
// 黑电平测自遮蔽边界；白电平取数据中的剪裁点。CFA 行相位从数据测量（见 measuredRedRow）。

#include "io/raw/raw_internal.h"

#include <algorithm>
#include <cmath>

#include "io/raw/raw_ljpeg.h"

namespace montage {
namespace raw {

namespace {

constexpr uint16_t kCanonSensorInfo = 0x00E0;
constexpr uint16_t kCanonColorData = 0x4001;
constexpr uint16_t kCanonModelId = 0x0010;

// raw 数据以绿/蓝行开始的 Canon 机型 ID（MakerNote 0x0010），来自实拍样本。
// 仅在测量不确定时参考。
struct GreenFirstModel {
    uint32_t id;
    const char* model;
};
constexpr GreenFirstModel kGreenFirstModels[5] = {
    {0x80000218u, "EOS 5D Mark II"}, {0x80000250u, "EOS 7D"}, {0x80000270u, "EOS 550D / Rebel T2i / Kiss X4"},
    {0x80000286u, "EOS 600D / Rebel T3i / Kiss X5"}, {0x80000327u, "EOS 1200D / Rebel T5 / Kiss X70"},
};

}  // namespace

// 红色所在的数据行（0 = 偶，1 = 奇），在 `area` 上测量；两个线索（见模块注释）
// 不一致或未剪裁信号太少时为空。隔行对采样；红在偶数据列。
std::optional<size_t> measuredRedRow(const std::vector<uint16_t>& data, size_t width, Rect area, float black, float white) {
    double diff[2] = {0, 0};
    double upper[2] = {0, 0};
    double lower[2] = {0, 0};
    uint64_t n[2] = {0, 0};
    float lo = black + 1.0f;
    float hi = black + (white - black) * 0.98f;
    if (width == 0) return std::nullopt;
    size_t xEnd = std::min(area.x + area.width, width);
    if (xEnd == 0) return std::nullopt;
    xEnd -= 1;
    if (area.height == 0 || area.y > SIZE_MAX - area.height) return std::nullopt;
    size_t yEnd = area.y + area.height - 1;
    auto row = [&](size_t y) -> const uint16_t* {
        if (y >= SIZE_MAX - width) return nullptr;
        if (y * width > data.size() || data.size() - y * width < width) return nullptr;
        return data.data() + y * width;
    };
    size_t y = area.y + (area.y & 1);
    while (y < yEnd) {
        const uint16_t* r0 = row(y);
        const uint16_t* r1 = y + 1 <= SIZE_MAX ? row(y + 1) : nullptr;
        if (!r0 || !r1) break;
        for (size_t x = area.x; x < xEnd; ++x) {
            float a = r0[x];
            float b = r1[x + 1];
            if (a < lo || b < lo || a > hi || b > hi) continue;
            size_t p = x & 1;
            diff[p] += std::abs(static_cast<double>(a) - b);
            upper[p] += a - black;
            lower[p] += b - black;
            ++n[p];
        }
        y = y + 4 <= SIZE_MAX ? y + 4 : yEnd;
    }
    if (n[0] < 256 || n[1] < 256) return std::nullopt;
    double d[2] = {diff[0] / static_cast<double>(n[0]), diff[1] / static_cast<double>(n[1])};
    double rel[2];
    for (int p = 0; p < 2; ++p) rel[p] = std::abs(upper[p] - lower[p]) / std::max(upper[p] + lower[p], 1.0);
    if (d[0] < 0.8 * d[1] && rel[0] < rel[1]) {
        return 1;
    }
    if (d[1] < 0.8 * d[0] && rel[1] < rel[0]) {
        return 0;
    }
    return std::nullopt;
}

namespace {

// Canon maker note IFD（若存在）。
std::optional<Ifd> makerNote(const Tiff& t, const std::vector<Ifd>& ifds) {
    for (const Ifd& i : ifds) {
        const Entry* e = i.get(tag::MAKER_NOTE);
        if (e) return t.ifdAt(e->at, 0);
    }
    return std::nullopt;
}

// ColorData 的 as-shot 白平衡倍率（R, G, B）。
std::optional<std::array<double, 3>> asShotWb(const Tiff& t, const Ifd& mn) {
    const Entry* e = mn.get(kCanonColorData);
    if (!e) return std::nullopt;
    // 条目是 UNDEFINED：字节为 16 位字表（文件字节序），偏移按字计（非字节）。
    // 读取限制在条目自身字节计数内。
    size_t u = e->unit();
    if (u == 0) return std::nullopt;
    size_t bytes = u * static_cast<size_t>(e->count);
    auto word = [&](size_t off) -> std::optional<double> {
        if (off > SIZE_MAX / 2 - 1) return std::nullopt;
        size_t at = off * 2;
        if (e->at > t.len || at > bytes || bytes - at < 2 || e->at + at > t.len - 2) return std::nullopt;
        auto v = t.u16At(e->at + at);
        if (!v) return std::nullopt;
        return static_cast<double>(*v);
    };
    // 各 ColorData 版本的 WB_RGGBLevelsAsShot 字偏移。
    for (size_t off : {static_cast<size_t>(0x3F), static_cast<size_t>(0x47), static_cast<size_t>(0x19), static_cast<size_t>(0x22)}) {
        auto r = word(off);
        auto g1 = word(off + 1);
        auto g2 = word(off + 2);
        auto b = word(off + 3);
        if (!r || !g1 || !g2 || !b) continue;
        double g = (*g1 + *g2) / 2.0;
        bool plausible = g >= 256.0 && g < 16384.0 && std::abs(*g1 - *g2) <= g * 0.05 && *r / g >= 0.25 && *r / g < 8.0 &&
                         *b / g >= 0.25 && *b / g < 8.0;
        if (plausible) return std::array<double, 3>{*r / g, 1.0, *b / g};
    }
    return std::nullopt;
}

}  // namespace

// 白（剪裁）电平：有意义的样本份额堆积在最大值时取它，否则取声明精度的满程。
float clipLevel(const std::vector<uint16_t>& data, uint32_t precision) {
    float full = static_cast<float>((1ull << std::min<uint32_t>(precision, 16)) - 1);
    size_t step = std::max<size_t>(data.size() / 2000000, 1);
    uint16_t max = 0;
    for (size_t i = 0; i < data.size(); i += step) max = std::max(max, data[i]);
    if (max == 0) return full;
    size_t near = 0;
    size_t sampled = 0;
    for (size_t i = 0; i < data.size(); i += step) {
        ++sampled;
        if (static_cast<uint32_t>(data[i]) + static_cast<uint32_t>(max / 256) >= static_cast<uint32_t>(max)) ++near;
    }
    if (near * 2000 >= sampled) {
        // 顶部至少 0.05% 的样本：这就是剪裁点。略退使剪裁平台映射到白。
        return static_cast<float>(max) * 0.995f;
    }
    return full;
}

namespace {

// 区域均值按 2×2 位置。
std::optional<std::array<float, 4>> maskedBlack(const std::vector<uint16_t>& data, size_t width, Rect area) {
    if (area.empty() || area.width < 2 || area.height < 2) return std::nullopt;
    if (width == 0 || area.y > SIZE_MAX - area.height || area.x > SIZE_MAX - area.width) return std::nullopt;
    uint64_t sum[4] = {0, 0, 0, 0};
    uint64_t n[4] = {0, 0, 0, 0};
    for (size_t y = area.y; y < area.y + area.height; ++y) {
        if (y >= SIZE_MAX / std::max<size_t>(width, 1)) return std::nullopt;
        if (y * width >= data.size()) return std::nullopt;
        for (size_t x = area.x; x < area.x + area.width; ++x) {
            if (x >= width) return std::nullopt;
            uint16_t v = data[y * width + x];
            size_t k = (y & 1) * 2 + (x & 1);
            sum[k] += v;
            ++n[k];
        }
    }
    for (int k = 0; k < 4; ++k)
        if (n[k] == 0) return std::nullopt;
    std::array<float, 4> out{};
    for (int k = 0; k < 4; ++k) out[k] = static_cast<float>(static_cast<double>(sum[k]) / static_cast<double>(n[k]));
    return out;
}

}  // namespace

// 按数据奇偶 2×2 位置测得的黑电平，重排为从活动区原点索引。
BlackLevels realignBlack(BlackLevels b, Rect active) {
    if (b.rows != 2 || b.cols != 2 || b.values.size() != 4) return b;
    const auto& v = b.values;
    std::vector<float> values(4);
    for (size_t k = 0; k < 4; ++k) {
        size_t ax = k & 1, ay = k >> 1;
        size_t x = (active.x + ax) & 1;
        size_t y = (active.y + ay) & 1;
        values[k] = v[y * 2 + x];
    }
    b.values = std::move(values);
    return b;
}

Sensor decodeCr2(const Tiff& t, const Limits& limits) {
    std::vector<Ifd> ifds = t.allIfds();
    auto ifd0Opt = t.ifdAt(t.firstIfd, 0);
    if (!ifd0Opt) throw RawError::malformed("CR2 has no IFD0");
    const Ifd& ifd0 = *ifd0Opt;
    size_t rawOff = t.u32At(12).value_or(0);
    auto rawOpt = t.ifdAt(rawOff, 0);
    if (!rawOpt) throw RawError::malformed("CR2 raw IFD is missing");
    const Ifd& raw = *rawOpt;
    auto offOpt = t.tagUint(raw, tag::STRIP_OFFSETS);
    if (!offOpt) throw RawError::malformed("CR2 raw data offset missing");
    size_t off = *offOpt;
    size_t len = t.len > off ? t.len - off : 0;
    {
        auto lens = t.tagUints(raw, tag::STRIP_BYTE_COUNTS);
        if (!lens.empty()) len = lens[0];
    }
    if (off > t.len || len > t.len - off) throw RawError::malformed("CR2 raw data lies outside the file");
    const uint8_t* src = t.data + off;

    LjpegFrame frame = [&] {
        try {
            return ljpegFrame(src, len);
        } catch (const RawError& e) {
            if (e.kind == RawError::Unsupported && e.msg.find("subsampled") != std::string::npos) {
                throw RawError::unsupported("Canon sRAW / mRAW");
            }
            throw;
        }
    }();
    auto totalOpt = frame.samples();
    if (!totalOpt) throw RawError::malformed("CR2 frame too large");
    size_t total = *totalOpt;
    std::vector<uint32_t> slices = t.tagUints(raw, tag::CR2_SLICE);
    size_t width = 0;
    std::vector<size_t> widths;
    if (slices.size() == 3 && slices[0] < 64 && slices[1] > 0 && slices[2] > 0) {
        size_t n = slices[0], w = slices[1], last = slices[2];
        widths.assign(n, w);
        widths.push_back(last);
        width = n * w + last;
    } else {
        width = frame.width * frame.components;
        widths.assign(1, width);
    }
    if (width == 0 || total % width != 0) throw RawError::malformed("CR2 slices do not match the JPEG frame");
    size_t height = total / width;
    limits.check(width, height, 2);
    auto [_f, samples] = ljpegDecode(src, len, total);

    // 去切片。
    std::vector<uint16_t> data(width * height, 0);
    {
        size_t pos = 0;
        size_t x0 = 0;
        for (size_t sw : widths) {
            for (size_t y = 0; y < height; ++y) {
                if (pos + sw > samples.size() || x0 + sw > width) throw RawError::malformed("CR2 slice data is short");
                std::memcpy(data.data() + y * width + x0, samples.data() + pos, sw * sizeof(uint16_t));
                pos += sw;
            }
            x0 += sw;
        }
    }

    std::vector<std::string> warnings;
    auto mnOpt = makerNote(t, ifds);
    Rect full = Rect::make(0, 0, width, height);
    // SensorInfo: [1] 宽 [2] 高 [5..8] 左/上/右/下边界（含）。
    std::vector<uint32_t> info;
    if (mnOpt) info = t.tagUints(*mnOpt, kCanonSensorInfo);
    Rect active = full;
    if (info.size() >= 9 && info[7] > info[5] && info[8] > info[6]) {
        Rect r = Rect::make(info[5], info[6], info[7] - info[5] + 1, info[8] - info[6] + 1).intersect(full);
        if (!r.empty()) active = r;
    }
    // 黑电平：图像左侧的遮蔽列（跳过几列边缘）。
    std::optional<std::array<float, 4>> black4;
    if (active.x >= 16 && active.x / 2 > 4) {
        Rect area = Rect::make(4, active.y, active.x / 2 - 4, active.height);
        black4 = maskedBlack(data, width, area);
    }
    BlackLevels black;
    if (black4) {
        black.rows = 2;
        black.cols = 2;
        black.values.assign(black4->begin(), black4->end());
    } else {
        warnings.push_back("no masked sensor area; black level assumed to be 0");
        black = BlackLevels::uniform(0.0f);
    }
    float white = clipLevel(data, frame.precision);
    // 红在偶数据列；红行测量（见模块注释）。
    float meanBlack = 0.0f;
    for (float v : black.values) meanBlack += v;
    meanBlack /= static_cast<float>(std::max<size_t>(black.values.size(), 1));
    size_t redRow = 0;
    if (auto measured = measuredRedRow(data, width, active, meanBlack, white)) {
        redRow = *measured;
    } else {
        std::optional<uint32_t> id;
        if (mnOpt) id = t.tagUint(*mnOpt, kCanonModelId);
        bool known = false;
        if (id) {
            for (const auto& m : kGreenFirstModels)
                if (m.id == *id) known = true;
        }
        if (!known) {
            warnings.push_back("CR2 colour filter phase could not be measured; assumed red/green in the first row");
        }
        redRow = known ? 1 : 0;
    }
    Cfa cfa;
    cfa.width = 2;
    cfa.height = 2;
    cfa.originX = 0;
    cfa.originY = 0;
    if (redRow == 0) {
        cfa.colors = {0, 1, 1, 2};
    } else {
        cfa.colors = {1, 2, 0, 1};
    }
    std::optional<std::array<double, 3>> cameraWb;
    if (mnOpt) cameraWb = asShotWb(t, *mnOpt);
    black = realignBlack(std::move(black), active);

    Sensor s;
    s.format = RawFormat::Cr2;
    s.make = t.tagAscii(ifd0, tag::MAKE);
    s.model = t.tagAscii(ifd0, tag::MODEL);
    s.width = width;
    s.height = height;
    s.samples = 1;
    s.data = std::move(data);
    s.cfa = std::move(cfa);
    s.linearization = std::nullopt;
    s.black = std::move(black);
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = active;
    s.crop = active;
    s.color = ColorInfo{};
    s.cameraWb = cameraWb;
    s.orientation = orientationOf(t.tagUint(ifd0, tag::ORIENTATION));
    s.baselineExposure = 0.0;
    s.gainMaps.clear();
    s.toneCurve.clear();
    s.warnings = std::move(warnings);
    return s;
}

}  // namespace raw
}  // namespace montage
