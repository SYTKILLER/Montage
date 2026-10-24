// M23a RAW 解码入口分发。翻译自 photocraft crates/raw/src/lib.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_decode.h"

#include <cstring>

#include "io/raw/raw_internal.h"

namespace montage {
namespace raw {

namespace {

// 无色彩描述格式按 Make 推容器族。
RawFormat makeFormat(const std::optional<std::string>& make) {
    std::string m = make.value_or("");
    for (char& c : m) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (m.rfind("NIKON", 0) == 0) return RawFormat::Nef;
    if (m.rfind("SONY", 0) == 0) return RawFormat::Arw;
    if (m.rfind("PENTAX", 0) == 0 || m.rfind("RICOH", 0) == 0) return RawFormat::Pef;
    if (m.rfind("OLYMPUS", 0) == 0 || m.rfind("OM DIGITAL", 0) == 0) return RawFormat::Orf;
    return RawFormat::TiffEp;
}

}  // namespace

std::optional<RawFormat> identify(const uint8_t* bytes, size_t len) {
    if (len >= 12 && std::memcmp(bytes + 4, "ftypcrx ", 8) == 0) return RawFormat::Cr3;
    if (len >= 15 && std::memcmp(bytes, kRafMagic, 15) == 0) return RawFormat::Raf;
    auto t = Tiff::create(bytes, len);
    if (!t) return std::nullopt;
    if (len >= 11 && std::memcmp(bytes + 8, "CR\x02", 3) == 0) return RawFormat::Cr2;
    // ORF / RW2 的 TIFF 变体魔数短路（先于 IFD 内容判断）。
    if (len >= 4) {
        if (std::memcmp(bytes, "IIRO", 4) == 0 || std::memcmp(bytes, "IIRS", 4) == 0 || std::memcmp(bytes, "MMOR", 4) == 0)
            return RawFormat::Orf;
        if (std::memcmp(bytes, "IIU\0", 4) == 0) return RawFormat::Rw2;
    }
    std::vector<Ifd> ifds = t->allIfds();
    for (const Ifd& i : ifds)
        if (i.has(tag::DNG_VERSION)) return RawFormat::Dng;
    for (const Ifd& i : ifds) {
        uint32_t photo = t->tagUint(i, tag::PHOTOMETRIC).value_or(0);
        if (photo == 32803 || photo == 34892) {
            // Make 从任一 IFD 取（上游 find_map），raw IFD 常无 Make。
            std::optional<std::string> make;
            for (const Ifd& j : ifds) {
                make = t->tagAscii(j, tag::MAKE);
                if (make) break;
            }
            return makeFormat(make);
        }
    }
    // Sony 无 CFA 的减尺寸变体（识别但不解码）。
    bool hasSonyType = false;
    for (const Ifd& i : ifds)
        if (i.has(kSonyRawFileType)) hasSonyType = true;
    if (hasSonyType) {
        for (const Ifd& i : ifds) {
            auto m = t->tagAscii(i, tag::MAKE);
            if (m) {
                std::string up = *m;
                for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (up.rfind("SONY", 0) == 0) return RawFormat::Arw;
            }
        }
    }
    return std::nullopt;
}

Sensor decodeRaw(const uint8_t* bytes, size_t len, const Limits& limits) {
    std::optional<RawFormat> formatOpt = identify(bytes, len);
    if (!formatOpt) throw RawError::notRaw();
    RawFormat format = *formatOpt;
    if (format == RawFormat::Cr3) throw RawError::unsupported("Canon CR3 (ISO BMFF / CRX) is not decoded yet");
    if (format == RawFormat::Raf) return decodeRaf(bytes, len, limits);
    auto t = Tiff::create(bytes, len);
    if (!t) throw RawError::notRaw();
    switch (format) {
        case RawFormat::Dng:
            return decodeDng(*t, limits);
        case RawFormat::Cr2:
            return decodeCr2(*t, limits);
        case RawFormat::Rw2:
            return decodeRw2(*t, limits);
        case RawFormat::Orf:
            return decodeOrf(*t, limits);
        default:
            return decodeTiffEp(*t, format, limits);
    }
}

}  // namespace raw
}  // namespace montage
