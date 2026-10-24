// M23a：Olympus ORF 未压缩传感器数据（E-1、E-400 等 Four Thirds 旧机身）。
// 容器为 IIRO/IIRS/MMOR 头的 TIFF；传感器数据是 IFD0 的 strip，16 位样本、
// 高 ValidBits 位有效。CFA 来自 EXIF CFAPattern。电平来自公开文档的 maker note
// ImageProcessing 标签（ExifTool Olympus 表）。新机身为 Olympus 私有压缩，不解码。
// 翻译自 photocraft crates/raw/src/orf.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <algorithm>
#include <cstring>

namespace montage {
namespace raw {

namespace {

constexpr uint16_t kCameraSettings = 0x2020;
constexpr uint16_t kPreviewImageStart = 0x0101;
constexpr uint16_t kPreviewImageLength = 0x0102;
constexpr uint16_t kImageProcessing = 0x2040;
constexpr uint16_t kOrfWbRbLevels = 0x0100;
constexpr uint16_t kOrfWbGLevel = 0x011F;
constexpr uint16_t kOrfBlackLevel2 = 0x0600;
constexpr uint16_t kOrfValidBits = 0x0611;
constexpr uint16_t kOrfCrop[4] = {0x0612, 0x0613, 0x0614, 0x0615};

// Olympus maker note 的 sub-IFD（0x2020 CameraSettings、0x2040 ImageProcessing…）。
// 新式 note（OLYMPUS\0 + 字节序）偏移相对 note；旧式（OLYMP\0）相对文件。
struct MakerSubIfd {
    Tiff view;
    Ifd ifd;
    size_t base = 0;
};
std::optional<MakerSubIfd> makerSubIfd(const Tiff& t, const std::vector<Ifd>& ifds, uint16_t sub) {
    const Entry* e = nullptr;
    for (const Ifd& i : ifds) {
        const Entry* c = i.get(tag::MAKER_NOTE);
        if (c) {
            e = c;
            break;
        }
    }
    if (!e) return std::nullopt;
    if (e->at > t.len || t.len - e->at < 12) return std::nullopt;
    const uint8_t* head = t.data + e->at;
    MakerSubIfd out;
    if (std::memcmp(head, "OLYMPUS\0", 8) == 0) {
        bool le;
        if (std::memcmp(head + 8, "II", 2) == 0) {
            le = true;
        } else if (std::memcmp(head + 8, "MM", 2) == 0) {
            le = false;
        } else {
            return std::nullopt;
        }
        Tiff inner;
        inner.data = t.data + e->at;
        inner.len = t.len - e->at;
        inner.le = le;
        inner.firstIfd = 12;
        auto ifd = inner.ifdAt(12, 0);
        if (!ifd) return std::nullopt;
        out.view = inner;
        out.ifd = *ifd;
        out.base = e->at;
    } else if (std::memcmp(head, "OLYMP\0", 6) == 0) {
        if (e->at > SIZE_MAX - 8) return std::nullopt;
        auto ifd = t.ifdAt(e->at + 8, 0);
        if (!ifd) return std::nullopt;
        out.view = t;
        out.ifd = *ifd;
        out.base = 0;
    } else {
        return std::nullopt;
    }
    const Entry* ip = out.ifd.get(sub);
    if (!ip) return std::nullopt;
    size_t at;
    if (ip->typ == 4 || ip->typ == 13) {
        auto v = out.view.uint(*ip, 0);
        if (!v) return std::nullopt;
        at = *v;
    } else {
        at = ip->at;
    }
    auto ifd = out.view.ifdAt(at, 0);
    if (!ifd) return std::nullopt;
    out.ifd = *ifd;
    return out;
}

}  // namespace

// 相机预览 JPEG 的绝对偏移与长度（maker note CameraSettings 0x0101/0x0102）。
std::optional<std::pair<size_t, size_t>> orfPreviewRange(const Tiff& t) {
    auto sub = makerSubIfd(t, t.allIfds(), kCameraSettings);
    if (!sub) return std::nullopt;
    auto start = sub->view.tagUint(sub->ifd, kPreviewImageStart);
    auto len = sub->view.tagUint(sub->ifd, kPreviewImageLength);
    if (!start || !len) return std::nullopt;
    if (sub->base > SIZE_MAX - *start) return std::nullopt;
    return std::make_pair(sub->base + *start, static_cast<size_t>(*len));
}

Sensor decodeOrf(const Tiff& t, const Limits& limits) {
    std::vector<Ifd> ifds = t.allIfds();
    auto rawOpt = t.ifdAt(t.firstIfd, 0);
    if (!rawOpt) throw RawError::malformed("ORF has no IFD0");
    const Ifd& raw = *rawOpt;
    uint64_t width = t.tagUint(raw, tag::IMAGE_WIDTH).value_or(0);
    uint64_t height = t.tagUint(raw, tag::IMAGE_LENGTH).value_or(0);
    // 每个 ORF（无论压缩与否）都声明 strip 字节计数。缺失/不可读意味着 IFD0 被截短
    // （部分下载或损坏文件），而非压缩变体：空和不可误读为"每样本不足 16 位"。
    std::vector<uint32_t> counts = t.tagUints(raw, tag::STRIP_BYTE_COUNTS);
    if (counts.empty()) throw RawError::malformed("ORF IFD0 has no readable strip byte counts");
    uint64_t stored = 0;
    for (uint32_t c : counts) stored += c;
    if (t.tagUint(raw, tag::COMPRESSION).value_or(1) != 1 || stored < width * height * 2) {
        throw RawError::unsupported("Olympus compressed (or packed) ORF is not decoded");
    }
    Plane plane = readPlane(t, raw, limits, JpegLayout::Flat);
    if (plane.samples != 1) {
        throw RawError::unsupported("ORF with " + std::to_string(plane.samples) + " samples per pixel");
    }
    auto cfaOpt = tiffEpCfa(t, raw, ifds);
    if (!cfaOpt) throw RawError::unsupported("ORF: CFA pattern not recorded");
    Cfa cfa = *cfaOpt;
    if (!cfa.isBayer()) throw RawError::unsupported("ORF: non-Bayer CFA");
    Rect full = Rect::make(0, 0, plane.width, plane.height);
    std::vector<std::string> warnings;
    auto ip = makerSubIfd(t, ifds, kImageProcessing);
    auto floats = [&](uint16_t tg) -> std::vector<double> {
        if (!ip) return {};
        return ip->view.tagFloats(ip->ifd, tg);
    };

    BlackLevels black;
    if (auto v = rggbByPosition(cfa, floats(kOrfBlackLevel2))) {
        black.rows = 2;
        black.cols = 2;
        black.values.assign(v->begin(), v->end());
    } else {
        warnings.push_back("ORF: black level not recorded; assumed 0");
        black = BlackLevels::uniform(0.0f);
    }
    std::vector<uint16_t> data = std::move(plane.data);
    float white;
    {
        std::vector<double> vb = floats(kOrfValidBits);
        bool handled = false;
        if (!vb.empty() && vb[0] >= 8.0 && vb[0] <= 16.0) {
            uint32_t bits = static_cast<uint32_t>(vb[0]);
            uint32_t top = (1u << bits) - 1;
            // 观察：E-1/E-400 把有效位左对齐存进 16 位容器。
            bool anyOver = false;
            for (uint16_t v : data)
                if (v > top) {
                    anyOver = true;
                    break;
                }
            if (anyOver) {
                uint32_t shift = 16 - bits;
                for (uint16_t& v : data) v = static_cast<uint16_t>(v >> shift);
            }
            white = static_cast<float>(top);
            handled = true;
        }
        if (!handled) white = clipLevel(data, plane.bits);
    }
    double g = 256.0;
    {
        std::vector<double> vg = floats(kOrfWbGLevel);
        if (!vg.empty() && vg[0] > 0.0) g = vg[0];
    }
    std::optional<std::array<double, 3>> cameraWb;
    {
        std::vector<double> wb = floats(kOrfWbRbLevels);
        if (wb.size() >= 2) {
            std::array<double, 3> m = {wb[0] / g, 1.0, wb[1] / g};
            bool ok = true;
            for (double v : m)
                if (!(v >= 0.25 && v < 8.0)) ok = false;
            if (ok) cameraWb = m;
        }
    }
    Rect crop = full;
    {
        std::optional<double> c[4];
        for (int i = 0; i < 4; ++i) {
            std::vector<double> v = floats(kOrfCrop[i]);
            if (!v.empty()) c[i] = v[0];
        }
        if (c[0] && c[1] && c[2] && c[3]) {
            bool ok = true;
            for (int i = 0; i < 4; ++i)
                if (!(*c[i] >= 0.0 && *c[i] < 1e6)) ok = false;
            if (ok) {
                Rect r = Rect::make(static_cast<size_t>(*c[0]), static_cast<size_t>(*c[1]), static_cast<size_t>(*c[2]),
                                    static_cast<size_t>(*c[3]))
                             .intersect(full);
                if (!r.empty()) crop = r;
            }
        }
    }

    Sensor s;
    s.format = RawFormat::Orf;
    for (const Ifd& i : ifds) {
        if (!s.make) s.make = t.tagAscii(i, tag::MAKE);
        if (!s.model) s.model = t.tagAscii(i, tag::MODEL);
    }
    s.width = plane.width;
    s.height = plane.height;
    s.samples = 1;
    s.data = std::move(data);
    s.cfa = std::move(cfa);
    s.linearization = std::nullopt;
    s.black = std::move(black);
    s.white[0] = s.white[1] = s.white[2] = white;
    s.active = full;
    s.crop = crop;
    s.color = ColorInfo{};
    s.cameraWb = cameraWb;
    s.orientation = orientationOf(t.tagUint(raw, tag::ORIENTATION));
    s.baselineExposure = 0.0;
    s.gainMaps.clear();
    s.toneCurve.clear();
    s.warnings = std::move(warnings);
    return s;
}

}  // namespace raw
}  // namespace montage
