// M23a：Fujifilm RAF。
// 头为大端：魔数 FUJIFILMCCD-RAW、格式版本、相机 id、型号名（28 起 32 字节），
// 然后是相机 JPEG 预览（84）、元数据目录（92）与 CFA 区（100）的 (偏移, 长度) 对。
// 元数据目录为大端记录数 + (tag, size, data) 记录。约 2010 年起 CFA 区是小型 TIFF，
// IFD0 经 0xF000 指向 Fuji IFD（0xF001/F002 全宽高、0xF003 位深、0xF007/F008 strip、
// 0xF00A 黑电平、0xF00E WB_GRBLevels），偏移相对 CFA 区。
// 样本存储按 strip 字节数区分：16 位容器（TIFF 字节序）/ 12 位小端位流 LSB first /
// 14 位小端 32 位字 MSB first；短 strip 为 Fuji 私有无损压缩（IS 开头），不支持。
// XTransLayout/FujiLayout 列表逆序存储（末字节是数据 (0,0) 的颜色）。
// 翻译自 photocraft crates/raw/src/raf.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace montage {
namespace raw {

namespace {

constexpr uint16_t kCropTopLeft = 0x0110;
constexpr uint16_t kCroppedSize = 0x0111;
constexpr uint16_t kFujiLayout = 0x0130;
constexpr uint16_t kXtransLayout = 0x0131;
constexpr uint16_t kWbGrgbLevels = 0x2FF0;
constexpr uint16_t kRawExposureBias = 0x9650;

constexpr uint16_t kFujiIfd = 0xF000;
constexpr uint16_t kRawWidth = 0xF001;
constexpr uint16_t kRawHeight = 0xF002;
constexpr uint16_t kRawBits = 0xF003;
constexpr uint16_t kStripOffset = 0xF007;
constexpr uint16_t kStripByteCount = 0xF008;
constexpr uint16_t kFujiBlackLevel = 0xF00A;
constexpr uint16_t kWbGrbLevels = 0xF00E;

// 最多读取的元数据记录数。
constexpr size_t kMaxRecords = 512;

std::optional<size_t> be32s(const uint8_t* b, size_t len, size_t at) {
    if (at > len || len - at < 4) return std::nullopt;
    return (static_cast<size_t>(b[at]) << 24 | b[at + 1] << 16 | b[at + 2] << 8 | b[at + 3]);
}

std::optional<uint16_t> be16v(const uint8_t* b, size_t len, size_t at) {
    if (at > len || len - at < 2) return std::nullopt;
    return static_cast<uint16_t>(b[at] << 8 | b[at + 1]);
}

// 样本存储方式。
enum class RafPacking { U16Le, U16Be, LsbFirst, Words32MsbFirst };

// 打包流中第 i 个 bits 位（≤16）样本（src 持整条流；越界读 0）。
inline uint16_t packedSample(const uint8_t* src, size_t srcLen, size_t i, uint32_t bits, RafPacking packing) {
    size_t at = i * bits;
    size_t byte = at / 8;
    uint32_t shift = static_cast<uint32_t>(at % 8);
    uint32_t nb = std::min<uint32_t>(bits, 16);
    uint32_t mask = (1u << nb) - 1;
    auto byteAt = [&](size_t k) { return k < srcLen ? src[k] : 0; };
    switch (packing) {
        case RafPacking::LsbFirst: {
            uint32_t w = 0;
            for (int k = 0; k < 4; ++k) w |= static_cast<uint32_t>(byteAt(byte + k)) << (8 * k);
            return static_cast<uint16_t>((w >> shift) & mask);
        }
        case RafPacking::Words32MsbFirst: {
            // 逻辑字节 k 的物理字节：k 在 32 位字内的位置反转。
            auto phys = [&](size_t k) { return byteAt((k & ~static_cast<size_t>(3)) | (3 - (k & 3))); };
            uint32_t w = 0;
            for (int k = 0; k < 4; ++k) w = (w << 8) | phys(byte + k);
            uint32_t sh = 32 - shift - nb;
            return static_cast<uint16_t>((w >> sh) & mask);
        }
        case RafPacking::U16Le: {
            uint16_t v = static_cast<uint16_t>(byteAt(2 * i) | byteAt(2 * i + 1) << 8);
            return v;
        }
        default: {
            uint16_t v = static_cast<uint16_t>(byteAt(2 * i) << 8 | byteAt(2 * i + 1));
            return v;
        }
    }
}

}  // namespace

// 相机预览 JPEG 的偏移与长度（头 84/88 字节）。
std::optional<std::pair<size_t, size_t>> rafPreviewRange(const uint8_t* bytes, size_t len) {
    auto off = be32s(bytes, len, 84);
    auto l = be32s(bytes, len, 88);
    if (!off || !l) return std::nullopt;
    return std::make_pair(*off, *l);
}

namespace {

// 元数据目录的 (tag, data) 记录。
struct RafRecord {
    uint16_t tag = 0;
    const uint8_t* data = nullptr;
    size_t len = 0;
};
std::vector<RafRecord> rafRecords(const uint8_t* bytes, size_t len) {
    std::vector<RafRecord> out;
    auto at = be32s(bytes, len, 92);
    auto cnt = be32s(bytes, len, 96);
    if (!at || !cnt) return out;
    size_t end = *at + *cnt;
    if (*at > len || end > len) return out;
    const uint8_t* dir = bytes + *at;
    size_t dirLen = end - *at;
    auto dirBe32 = [&](size_t o) -> std::optional<size_t> { return be32s(dir, dirLen, o); };
    auto dirBe16 = [&](size_t o) -> std::optional<uint16_t> { return be16v(dir, dirLen, o); };
    auto nOpt = dirBe32(0);
    if (!nOpt) return out;
    size_t n = std::min<size_t>(*nOpt, kMaxRecords);
    size_t p = 4;
    for (size_t i = 0; i < n; ++i) {
        auto tag = dirBe16(p);
        auto size = dirBe16(p + 2);
        if (!tag || !size) break;
        size_t start = p + 4;
        if (start > dirLen || *size > dirLen - start) break;
        out.push_back(RafRecord{*tag, dir + start, *size});
        p = start + *size;
    }
    return out;
}

const RafRecord* findRecord(const std::vector<RafRecord>& recs, uint16_t tag) {
    for (const RafRecord& r : recs)
        if (r.tag == tag) return &r;
    return nullptr;
}

// 两个大端 16 位值的记录。
std::optional<std::pair<size_t, size_t>> recordPair(const std::vector<RafRecord>& recs, uint16_t tag) {
    const RafRecord* r = findRecord(recs, tag);
    if (!r || r->len < 4) return std::nullopt;
    auto a = be16v(r->data, r->len, 0);
    auto b = be16v(r->data, r->len, 2);
    if (!a || !b) return std::nullopt;
    return std::make_pair(static_cast<size_t>(*a), static_cast<size_t>(*b));
}

// XTransLayout（6×6）或 FujiLayout（2×2）的 CFA，两者均逆序存。
std::optional<Cfa> layoutCfa(const std::vector<RafRecord>& recs) {
    if (const RafRecord* d = findRecord(recs, kXtransLayout)) {
        if (d->len == 36) {
            Cfa c;
            c.width = 6;
            c.height = 6;
            c.originX = 0;
            c.originY = 0;
            c.colors.resize(36);
            for (size_t i = 0; i < 36; ++i) {
                uint8_t v = d->data[35 - i];
                if (v > 2) return std::nullopt;
                c.colors[i] = v;
            }
            return c;
        }
    }
    const RafRecord* d = findRecord(recs, kFujiLayout);
    if (!d || d->len != 4) return std::nullopt;
    Cfa c;
    c.width = 2;
    c.height = 2;
    c.originX = 0;
    c.originY = 0;
    for (size_t i = 0; i < 4; ++i) {
        uint8_t v = d->data[3 - i];
        c.colors.push_back((v & 3) == 3 ? 1 : static_cast<uint8_t>(v & 3));
    }
    if (!c.isBayer()) return std::nullopt;
    return c;
}

// 预览 JPEG EXIF（APP1）的 orientation、make、model。
struct RafExif {
    std::optional<uint16_t> orientation;
    std::optional<std::string> make;
    std::optional<std::string> model;
};
RafExif previewExif(const uint8_t* bytes, size_t len) {
    RafExif out;
    auto range = rafPreviewRange(bytes, len);
    if (!range) return out;
    auto [off, l] = *range;
    if (off > len || l > len - off) return out;
    const uint8_t* jpeg = bytes + off;
    if (l < 18 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || jpeg[2] != 0xFF || jpeg[3] != 0xE1 ||
        std::memcmp(jpeg + 6, "Exif\0\0", 6) != 0) {
        return out;
    }
    auto t = Tiff::create(jpeg + 12, l - 12);
    if (!t) return out;
    auto ifd0 = t->ifdAt(t->firstIfd, 0);
    if (!ifd0) return out;
    if (auto o = t->tagUint(*ifd0, tag::ORIENTATION)) {
        if (*o >= 1 && *o <= 8) out.orientation = static_cast<uint16_t>(*o);
    }
    out.make = t->tagAscii(*ifd0, tag::MAKE);
    out.model = t->tagAscii(*ifd0, tag::MODEL);
    return out;
}

// RAF 头（28..60）里的型号名。
std::optional<std::string> rafHeaderModel(const uint8_t* bytes, size_t len) {
    if (len < 60) return std::nullopt;
    const uint8_t* b = bytes + 28;
    size_t end = 0;
    while (end < 32 && b[end] != 0) ++end;
    std::string s(reinterpret_cast<const char*>(b), end);
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::nullopt;
    size_t z = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, z - a + 1);
    if (s.empty()) return std::nullopt;
    return s;
}

}  // namespace

Sensor decodeRaf(const uint8_t* bytes, size_t len, const Limits& limits) {
    auto cfaAtOpt = be32s(bytes, len, 100);
    if (!cfaAtOpt) throw RawError::malformed("RAF header is truncated");
    size_t cfaAt = *cfaAtOpt;
    size_t cfaLen = be32s(bytes, len, 104).value_or(0);
    if (cfaAt > len) throw RawError::malformed("RAF CFA region lies outside the file");
    size_t regionLen = len - cfaAt;
    if (cfaLen > 0 && cfaLen < regionLen) regionLen = cfaLen;
    auto tOpt = Tiff::create(bytes + cfaAt, regionLen);
    if (!tOpt) {
        throw RawError::unsupported("Fujifilm RAF of an early FinePix / SuperCCD body (no CFA TIFF) is not decoded yet");
    }
    const Tiff& t = *tOpt;
    auto ifd0Opt = t.ifdAt(t.firstIfd, 0);
    if (!ifd0Opt) throw RawError::malformed("RAF CFA TIFF has no IFD0");
    const Ifd& ifd0 = *ifd0Opt;
    auto fujiAtOpt = t.tagUint(ifd0, kFujiIfd);
    if (!fujiAtOpt) throw RawError::unsupported("RAF without a Fuji raw IFD");
    auto fujiOpt = t.ifdAt(*fujiAtOpt, 0);
    if (!fujiOpt) throw RawError::malformed("RAF Fuji raw IFD is unreadable");
    const Ifd& fuji = *fujiOpt;

    auto wOpt = t.tagUint(fuji, kRawWidth);
    auto hOpt = t.tagUint(fuji, kRawHeight);
    if (!wOpt) throw RawError::malformed("RAF has no raw width");
    if (!hOpt) throw RawError::malformed("RAF has no raw height");
    size_t width = *wOpt, height = *hOpt;
    uint32_t bits = t.tagUint(fuji, kRawBits).value_or(14);
    if (bits < 8 || bits > 16) throw RawError::unsupported("RAF with " + std::to_string(bits) + "-bit samples");
    limits.check(width, height, 2);
    auto offOpt = t.tagUint(fuji, kStripOffset);
    if (!offOpt) throw RawError::malformed("RAF has no strip offset");
    size_t offset = *offOpt;
    size_t count = t.tagUint(fuji, kStripByteCount).value_or(0);
    if (offset > regionLen) throw RawError::malformed("RAF raw data lies outside the file");
    const uint8_t* src = t.data + offset;
    size_t srcLen = regionLen - offset;
    if (count > 0 && count < srcLen) srcLen = count;
    size_t pixels = width * height;
    size_t packed = (pixels * bits + 7) / 8;
    RafPacking packing;
    if (count >= pixels * 2) {
        packing = t.le ? RafPacking::U16Le : RafPacking::U16Be;
    } else if (count >= packed && bits == 12) {
        packing = RafPacking::LsbFirst;
    } else if (count >= packed && bits == 14) {
        packing = RafPacking::Words32MsbFirst;
    } else if (count >= packed) {
        throw RawError::unsupported("Fujifilm RAF with packed " + std::to_string(bits) + "-bit samples");
    } else {
        throw RawError::unsupported("Fujifilm compressed RAF is not decoded (no public description of the format)");
    }
    size_t need = (packing == RafPacking::U16Le || packing == RafPacking::U16Be) ? pixels * 2 : packed;
    if (srcLen < need) throw RawError::malformed("RAF raw data is truncated");
    std::vector<uint16_t> data(pixels, 0);
    for (size_t i = 0; i < pixels; ++i) data[i] = packedSample(src, srcLen, i, bits, packing);

    std::vector<RafRecord> recs = rafRecords(bytes, len);
    auto cfaOpt = layoutCfa(recs);
    if (!cfaOpt) throw RawError::unsupported("Fujifilm RAF without a recognised CFA layout");
    Rect full = Rect::make(0, 0, width, height);
    std::vector<std::string> warnings;

    BlackLevels black;
    {
        std::vector<double> bl = t.tagFloats(fuji, kFujiBlackLevel);
        bool bad = bl.empty();
        for (double v : bl)
            if (v < 0.0) bad = true;
        if (bad) {
            warnings.push_back("RAF: black level not recorded; assumed 0");
            black = BlackLevels::uniform(0.0f);
        } else {
            // 每 CFA 位置一个值；顺序未确立，观察值至多差几个单位，取均值。
            double sum = 0;
            for (double v : bl) sum += v;
            black = BlackLevels::uniform(static_cast<float>(sum / static_cast<double>(bl.size())));
        }
    }
    float white = clipLevel(data, bits);

    std::optional<std::array<double, 3>> cameraWb;
    {
        std::vector<double> wb = t.tagFloats(fuji, kWbGrbLevels);
        std::optional<std::array<double, 3>> m;
        if (wb.size() >= 3 && wb[0] > 0.0) {
            m = std::array<double, 3>{wb[1] / wb[0], 1.0, wb[2] / wb[0]};
        } else if (const RafRecord* r = findRecord(recs, kWbGrgbLevels)) {
            std::vector<double> v;
            for (size_t k = 0; k < 4; ++k) {
                auto x = be16v(r->data, r->len, 2 * k);
                if (!x) {
                    v.clear();
                    break;
                }
                v.push_back(static_cast<double>(*x));
            }
            if (v.size() == 4 && v[0] + v[2] > 0.0) {
                m = std::array<double, 3>{2.0 * v[1] / (v[0] + v[2]), 1.0, 2.0 * v[3] / (v[0] + v[2])};
            }
        }
        if (m) {
            bool ok = true;
            for (double v : *m)
                if (!(v >= 0.25 && v < 8.0)) ok = false;
            if (ok) cameraWb = m;
        }
    }

    Rect crop = full;
    {
        auto topLeft = recordPair(recs, kCropTopLeft);
        auto size = recordPair(recs, kCroppedSize);
        if (topLeft && size) {
            Rect r = Rect::make(topLeft->second, topLeft->first, size->second, size->first).intersect(full);
            if (!r.empty()) crop = r;
        }
    }

    // RawExposureBias：raw 相对相机渲染的欠曝量（动态范围模式）；
    // 与 DNG BaselineExposure 一样撤销。
    double baselineExposure = 0.0;
    if (const RafRecord* r = findRecord(recs, kRawExposureBias); r && r->len >= 4) {
        auto n = be16v(r->data, r->len, 0);
        auto d = be16v(r->data, r->len, 2);
        if (n && d && *d != 0) {
            double v = -static_cast<double>(static_cast<int16_t>(*n)) / static_cast<double>(static_cast<int16_t>(*d));
            if (std::isfinite(v) && std::abs(v) <= 8.0) baselineExposure = v;
        }
    }

    RafExif exif = previewExif(bytes, len);

    Sensor s;
    s.format = RawFormat::Raf;
    s.make = exif.make;
    if (!s.make) s.make = std::string("FUJIFILM");
    s.model = exif.model;
    if (!s.model) s.model = rafHeaderModel(bytes, len);
    s.width = width;
    s.height = height;
    s.samples = 1;
    s.data = std::move(data);
    s.cfa = std::move(*cfaOpt);
    s.linearization = std::nullopt;
    s.black = std::move(black);
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = full;
    s.crop = crop;
    s.color = ColorInfo{};
    s.cameraWb = cameraWb;
    s.orientation = exif.orientation.value_or(1);
    s.baselineExposure = baselineExposure;
    s.gainMaps.clear();
    s.toneCurve.clear();
    s.warnings = std::move(warnings);
    return s;
}

}  // namespace raw
}  // namespace montage
