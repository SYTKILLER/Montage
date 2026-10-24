// M23c：内嵌 JPEG 预览定位实现。翻译自 photocraft crates/raw/src/preview.rs
// （MIT OR Apache-2.0）。par 无涉及（单趟扫描）。

#include "io/raw/raw_preview.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "io/raw/raw_internal.h"
#include "io/raw/raw_tiff.h"

namespace montage {
namespace raw {
namespace {

// DCT 编码（baseline/extended/progressive）JPEG 的帧尺寸；非 JPEG 或
// lossless（raw 数据）返回空。
std::optional<std::pair<uint32_t, uint32_t>> dctJpegSize(const uint8_t* b, size_t len) {
    auto at = [&](size_t i) -> const uint8_t* { return i < len ? b + i : nullptr; };
    if (at(0) == nullptr || at(1) == nullptr || at(2) == nullptr ||
        b[0] != 0xFF || b[1] != 0xD8 || b[2] != 0xFF) {
        return std::nullopt;
    }
    size_t pos = 2;
    for (int guard = 0; guard < 256; ++guard) {
        const uint8_t* p0 = at(pos);
        if (p0 == nullptr || *p0 != 0xFF) return std::nullopt;
        const uint8_t* m = at(pos + 1);
        if (m == nullptr) return std::nullopt;
        if (*m == 0xFF) {
            pos += 1;
            continue;
        }
        const uint8_t* l0 = at(pos + 2);
        const uint8_t* l1 = at(pos + 3);
        if (l0 == nullptr || l1 == nullptr) return std::nullopt;
        size_t segLen = (static_cast<size_t>(*l0) << 8) | *l1;
        bool sof = (*m >= 0xC0 && *m <= 0xC2);
        if (sof) {
            const uint8_t* h0 = at(pos + 5);
            const uint8_t* h1 = at(pos + 6);
            const uint8_t* w0 = at(pos + 7);
            const uint8_t* w1 = at(pos + 8);
            if (h0 == nullptr || h1 == nullptr || w0 == nullptr || w1 == nullptr) {
                return std::nullopt;
            }
            uint32_t h = (static_cast<uint32_t>(*h0) << 8) | *h1;
            uint32_t w = (static_cast<uint32_t>(*w0) << 8) | *w1;
            if (w > 0 && h > 0) return std::make_pair(w, h);
            return std::nullopt;
        }
        bool stop = (*m == 0xC3 || (*m >= 0xC5 && *m <= 0xC7) ||
                     (*m >= 0xC9 && *m <= 0xCB) || (*m >= 0xCD && *m <= 0xCF) ||
                     *m == 0xDA || *m == 0xD9);
        if (stop || segLen < 2) return std::nullopt;
        if (pos > SIZE_MAX - (2 + segLen)) return std::nullopt;
        pos += 2 + segLen;
    }
    return std::nullopt;
}

std::optional<EmbeddedPreview> candidate(const uint8_t* data, size_t len, size_t offset, size_t cl) {
    if (offset > len || cl > len - offset) return std::nullopt;
    auto size = dctJpegSize(data + offset, cl);
    if (!size) return std::nullopt;
    EmbeddedPreview p;
    p.offset = offset;
    p.len = cl;
    p.width = size->first;
    p.height = size->second;
    return p;
}

}  // namespace

std::vector<EmbeddedPreview> embeddedPreviews(const uint8_t* bytes, size_t len) {
    std::vector<EmbeddedPreview> found;
    if (len >= sizeof(kRafMagic) &&
        std::memcmp(bytes, kRafMagic, sizeof(kRafMagic) - 1) == 0) {
        // RAF 头：大端 JPEG 偏移与长度在 84/88 字节。
        if (auto r = rafPreviewRange(bytes, len)) {
            if (auto c = candidate(bytes, len, r->first, r->second)) {
                found.push_back(*c);
            }
        }
        // 去重后返回（RAF 无 TIFF 部分可扫）。
        return found;
    }
    auto t = Tiff::create(bytes, len);
    if (!t) return found;
    // Olympus ORF：预览在 maker note 里。
    if (len >= 4 &&
        (std::memcmp(bytes, "IIRO", 4) == 0 || std::memcmp(bytes, "IIRS", 4) == 0 ||
         std::memcmp(bytes, "MMOR", 4) == 0)) {
        if (auto r = orfPreviewRange(*t)) {
            if (auto c = candidate(bytes, len, r->first, r->second)) {
                found.push_back(*c);
            }
        }
    }
    for (const Ifd& ifd : t->allIfds()) {
        auto off = t->tagUint(ifd, tag::JPEG_INTERCHANGE_FORMAT);
        auto jl = t->tagUint(ifd, tag::JPEG_INTERCHANGE_FORMAT_LENGTH);
        if (off && jl) {
            if (auto c = candidate(bytes, len, *off, *jl)) {
                found.push_back(*c);
            }
        }
        auto comp = t->tagUint(ifd, tag::COMPRESSION);
        if (comp == 6 || comp == 7) {
            std::vector<uint32_t> offs = t->tagUints(ifd, tag::STRIP_OFFSETS);
            std::vector<uint32_t> lens = t->tagUints(ifd, tag::STRIP_BYTE_COUNTS);
            if (offs.size() == 1 && lens.size() == 1) {
                if (auto c = candidate(bytes, len, offs[0], lens[0])) {
                    found.push_back(*c);
                }
            }
        }
        // Panasonic RW2 JpgFromRaw（0x002E）是完整 JPEG。
        if (const Entry* e = ifd.get(0x002E); e != nullptr && e->typ == 7) {
            size_t rawLen = 0;
            const uint8_t* rawBytes = t->raw(*e, rawLen);
            if (rawBytes != nullptr && rawLen > 0) {
                if (auto size = dctJpegSize(rawBytes, rawLen)) {
                    EmbeddedPreview p;
                    p.offset = static_cast<size_t>(rawBytes - bytes);
                    p.len = rawLen;
                    p.width = size->first;
                    p.height = size->second;
                    found.push_back(p);
                }
            }
        }
    }
    // 同一流只取一次。
    std::vector<std::pair<size_t, size_t>> seen;
    found.erase(std::remove_if(found.begin(), found.end(),
                               [&](const EmbeddedPreview& p) {
                                   auto key = std::make_pair(p.offset, p.len);
                                   for (const auto& k : seen) {
                                       if (k == key) return true;
                                   }
                                   seen.push_back(key);
                                   return false;
                               }),
                found.end());
    return found;
}

std::optional<EmbeddedPreview> embeddedPreview(const uint8_t* bytes, size_t len) {
    auto all = embeddedPreviews(bytes, len);
    if (all.empty()) return std::nullopt;
    const EmbeddedPreview* best = &all[0];
    for (const EmbeddedPreview& p : all) {
        if (static_cast<uint64_t>(p.width) * p.height >
            static_cast<uint64_t>(best->width) * best->height) {
            best = &p;
        }
    }
    return *best;
}

std::optional<PreviewColorSpace> embeddedPreviewColorSpace(const uint8_t* bytes, size_t len) {
    auto t = Tiff::create(bytes, len);
    if (!t) return std::nullopt;
    auto ifd0 = t->ifdAt(t->firstIfd, 0);
    if (!ifd0) return std::nullopt;
    if (ifd0->has(tag::DNG_VERSION)) return std::nullopt;
    auto exifOff = t->tagUint(*ifd0, tag::EXIF_IFD);
    if (!exifOff) return std::nullopt;
    auto exif = t->ifdAt(*exifOff, 0);
    if (!exif) return std::nullopt;
    auto cs = t->tagUint(*exif, tag::COLOR_SPACE);
    if (!cs) return std::nullopt;
    if (*cs == 1) return PreviewColorSpace::Srgb;
    if (*cs == 0xFFFF) {
        auto interopOff = t->tagUint(*exif, tag::INTEROP_IFD);
        if (!interopOff) return std::nullopt;
        auto interop = t->ifdAt(*interopOff, 0);
        if (!interop) return std::nullopt;
        const Entry* e = interop->get(tag::INTEROP_INDEX);
        if (e == nullptr) return std::nullopt;
        size_t rl = 0;
        const uint8_t* r = t->raw(*e, rl);
        if (r == nullptr || rl < 3 || std::memcmp(r, "R03", 3) != 0) return std::nullopt;
        return PreviewColorSpace::AdobeRgb;
    }
    return std::nullopt;
}

}  // namespace raw
}  // namespace montage
