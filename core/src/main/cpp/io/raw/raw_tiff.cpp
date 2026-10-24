// M23a RAW 解码内核：TIFF 结构读取器实现。翻译自 photocraft crates/raw/src/tiff.rs。

#include "io/raw/raw_tiff.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {
namespace raw {

size_t typeSize(uint16_t typ) {
    switch (typ) {
        case 1: case 2: case 6: case 7: return 1;
        case 3: case 8: return 2;
        case 4: case 9: case 11: case 13: return 4;
        case 5: case 10: case 12: return 8;
        default: return 0;
    }
}

size_t Entry::unit() const { return typeSize(typ); }

const Entry* Ifd::get(uint16_t t) const {
    for (const Entry& e : entries) {
        if (e.tag == t) return &e;
    }
    return nullptr;
}

std::optional<Tiff> Tiff::create(const uint8_t* data, size_t len) {
    if (len < 8 || data == nullptr) return std::nullopt;
    bool le;
    if (std::memcmp(data, "II*\0", 4) == 0 || std::memcmp(data, "IIRO", 4) == 0 ||
        std::memcmp(data, "IIRS", 4) == 0 || std::memcmp(data, "IIU\0", 4) == 0) {
        le = true;
    } else if (std::memcmp(data, "MM\0*", 4) == 0 || std::memcmp(data, "MMOR", 4) == 0) {
        le = false;
    } else {
        return std::nullopt;
    }
    Tiff t;
    t.data = data;
    t.len = len;
    t.le = le;
    auto off = t.u32At(4);
    if (!off) return std::nullopt;
    t.firstIfd = static_cast<size_t>(*off);
    return t;
}

std::optional<size_t> Tiff::rangeAt(size_t at, size_t l) const {
    if (at > len || l > len - at) return std::nullopt;
    return at;
}

const uint8_t* Tiff::bytes(size_t at, size_t l) const {
    auto r = rangeAt(at, l);
    if (!r) return nullptr;
    return data + *r;
}

std::optional<uint16_t> Tiff::u16At(size_t at) const {
    const uint8_t* s = bytes(at, 2);
    if (!s) return std::nullopt;
    return le ? static_cast<uint16_t>(s[0] | s[1] << 8) : static_cast<uint16_t>(s[0] << 8 | s[1]);
}

std::optional<uint32_t> Tiff::u32At(size_t at) const {
    const uint8_t* s = bytes(at, 4);
    if (!s) return std::nullopt;
    if (le) return static_cast<uint32_t>(s[0]) | static_cast<uint32_t>(s[1]) << 8 |
               static_cast<uint32_t>(s[2]) << 16 | static_cast<uint32_t>(s[3]) << 24;
    return static_cast<uint32_t>(s[0]) << 24 | static_cast<uint32_t>(s[1]) << 16 |
           static_cast<uint32_t>(s[2]) << 8 | static_cast<uint32_t>(s[3]);
}

std::optional<uint64_t> Tiff::u64At(size_t at) const {
    const uint8_t* s = bytes(at, 8);
    if (!s) return std::nullopt;
    uint64_t v = 0;
    if (le) {
        for (int i = 7; i >= 0; --i) v = v << 8 | s[i];
    } else {
        for (int i = 0; i < 8; ++i) v = v << 8 | s[i];
    }
    return v;
}

std::optional<Ifd> Tiff::ifdAt(size_t offset, size_t base) const {
    auto nOpt = u16At(offset);
    if (!nOpt) return std::nullopt;
    size_t n = *nOpt;
    if (n == 0 || n > kMaxEntries) return std::nullopt;
    Ifd ifd;
    ifd.offset = offset;
    ifd.entries.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        size_t e = offset + 2 + 12 * i;
        if (e > len || 12 > len - e) break;
        auto tg = u16At(e);
        auto ty = u16At(e + 2);
        auto ct = u32At(e + 4);
        if (!tg || !ty || !ct) break;
        size_t size = typeSize(*ty) * static_cast<size_t>(*ct);
        size_t at;
        if (size <= 4) {
            at = e + 8;
        } else {
            auto p = u32At(e + 8);
            if (!p) break;
            at = static_cast<size_t>(*p) + base;
        }
        ifd.entries.push_back(Entry{*tg, *ty, *ct, at});
    }
    return ifd;
}

size_t Tiff::nextIfd(const Ifd& ifd) const {
    auto v = u32At(ifd.offset + 2 + 12 * ifd.entries.size());
    return v ? static_cast<size_t>(*v) : 0;
}

std::vector<Ifd> Tiff::allIfds() const {
    std::vector<Ifd> out;
    std::vector<size_t> seen;
    std::vector<size_t> stack{firstIfd};
    while (!stack.empty()) {
        size_t off = stack.back();
        stack.pop_back();
        if (off == 0 || std::find(seen.begin(), seen.end(), off) != seen.end() || seen.size() >= kMaxIfds) {
            continue;
        }
        seen.push_back(off);
        auto ifdOpt = ifdAt(off, 0);
        if (!ifdOpt) continue;
        Ifd ifd = *ifdOpt;
        size_t next = nextIfd(ifd);
        if (next != 0) stack.push_back(next);
        for (uint16_t t : {tag::SUB_IFDS, tag::EXIF_IFD}) {
            const Entry* e = ifd.get(t);
            if (!e) continue;
            // 逆序压栈，使第一个 SubIFD 先被访问。
            std::vector<uint32_t> v = uints(*e);
            if (v.size() > 16) v.resize(16);
            for (auto it = v.rbegin(); it != v.rend(); ++it) stack.push_back(static_cast<size_t>(*it));
        }
        out.push_back(std::move(ifd));
    }
    return out;
}

std::optional<uint32_t> Tiff::uint(const Entry& e, size_t i) const {
    if (i >= e.count) return std::nullopt;
    size_t u = e.unit();
    if (u == 0) return std::nullopt;
    if (i > (kMaxValues * 8)) return std::nullopt;  // 防御：i*unit 溢出面
    size_t at = e.at + i * u;
    switch (e.typ) {
        case 1: case 7: {
            const uint8_t* b = bytes(at, 1);
            if (!b) return std::nullopt;
            return static_cast<uint32_t>(b[0]);
        }
        case 6: {
            const uint8_t* b = bytes(at, 1);
            if (!b) return std::nullopt;
            int8_t v = static_cast<int8_t>(b[0]);
            if (v < 0) return std::nullopt;
            return static_cast<uint32_t>(v);
        }
        case 3: {
            auto v = u16At(at);
            if (!v) return std::nullopt;
            return static_cast<uint32_t>(*v);
        }
        case 8: {
            auto v = u16At(at);
            if (!v) return std::nullopt;
            int16_t sv = static_cast<int16_t>(*v);
            if (sv < 0) return std::nullopt;
            return static_cast<uint32_t>(sv);
        }
        case 4: case 13:
            return u32At(at);
        case 9: {
            auto v = u32At(at);
            if (!v) return std::nullopt;
            int32_t sv = static_cast<int32_t>(*v);
            if (sv < 0) return std::nullopt;
            return static_cast<uint32_t>(sv);
        }
        default:
            return std::nullopt;
    }
}

std::optional<double> Tiff::floatv(const Entry& e, size_t i) const {
    if (i >= e.count) return std::nullopt;
    size_t u = e.unit();
    if (u == 0) return std::nullopt;
    if (i > (kMaxValues * 8)) return std::nullopt;
    size_t at = e.at + i * u;
    double v;
    switch (e.typ) {
        case 1: case 7: {
            const uint8_t* b = bytes(at, 1);
            if (!b) return std::nullopt;
            v = static_cast<double>(b[0]);
            break;
        }
        case 6: {
            const uint8_t* b = bytes(at, 1);
            if (!b) return std::nullopt;
            v = static_cast<double>(static_cast<int8_t>(b[0]));
            break;
        }
        case 3: {
            auto x = u16At(at);
            if (!x) return std::nullopt;
            v = static_cast<double>(*x);
            break;
        }
        case 8: {
            auto x = u16At(at);
            if (!x) return std::nullopt;
            v = static_cast<double>(static_cast<int16_t>(*x));
            break;
        }
        case 4: case 13: {
            auto x = u32At(at);
            if (!x) return std::nullopt;
            v = static_cast<double>(*x);
            break;
        }
        case 9: {
            auto x = u32At(at);
            if (!x) return std::nullopt;
            v = static_cast<double>(static_cast<int32_t>(*x));
            break;
        }
        case 5: case 10: {
            auto n = u32At(at);
            auto d = u32At(at + 4);
            if (!n || !d) return std::nullopt;
            if (*d == 0) return std::nullopt;
            if (e.typ == 5) {
                v = static_cast<double>(*n) / static_cast<double>(*d);
            } else {
                v = static_cast<double>(static_cast<int32_t>(*n)) / static_cast<double>(static_cast<int32_t>(*d));
            }
            break;
        }
        case 11: {
            auto x = u32At(at);
            if (!x) return std::nullopt;
            uint32_t bits = *x;
            float f;
            std::memcpy(&f, &bits, 4);
            v = static_cast<double>(f);
            break;
        }
        case 12: {
            auto x = u64At(at);
            if (!x) return std::nullopt;
            uint64_t bits = *x;
            double d;
            std::memcpy(&d, &bits, 8);
            v = d;
            break;
        }
        default:
            return std::nullopt;
    }
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

std::vector<uint32_t> Tiff::uints(const Entry& e) const {
    size_t n = std::min<size_t>(e.count, kMaxValues);
    std::vector<uint32_t> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto v = uint(e, i);
        if (!v) return {};
        out.push_back(*v);
    }
    return out;
}

std::vector<double> Tiff::floats(const Entry& e) const {
    size_t n = std::min<size_t>(e.count, kMaxValues);
    std::vector<double> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto v = floatv(e, i);
        if (!v) return {};
        out.push_back(*v);
    }
    return out;
}

const uint8_t* Tiff::raw(const Entry& e, size_t& outLen) const {
    size_t u = e.unit();
    if (u == 0 || e.count > (kMaxValues * 8)) return nullptr;
    size_t l = u * static_cast<size_t>(e.count);
    const uint8_t* b = bytes(e.at, l);
    if (!b) return nullptr;
    outLen = l;
    return b;
}

std::optional<std::string> Tiff::ascii(const Entry& e) const {
    size_t l = 0;
    const uint8_t* b = raw(e, l);
    if (!b) return std::nullopt;
    size_t end = 0;
    while (end < l && b[end] != 0) ++end;
    std::string s(reinterpret_cast<const char*>(b), end);
    // 去首尾空白。
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::nullopt;
    size_t z = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, z - a + 1);
    if (s.empty()) return std::nullopt;
    return s;
}

std::optional<uint32_t> Tiff::tagUint(const Ifd& ifd, uint16_t t) const {
    const Entry* e = ifd.get(t);
    if (!e) return std::nullopt;
    return uint(*e, 0);
}

std::vector<uint32_t> Tiff::tagUints(const Ifd& ifd, uint16_t t) const {
    const Entry* e = ifd.get(t);
    if (!e) return {};
    return uints(*e);
}

std::vector<double> Tiff::tagFloats(const Ifd& ifd, uint16_t t) const {
    const Entry* e = ifd.get(t);
    if (!e) return {};
    return floats(*e);
}

std::optional<std::string> Tiff::tagAscii(const Ifd& ifd, uint16_t t) const {
    const Entry* e = ifd.get(t);
    if (!e) return std::nullopt;
    return ascii(*e);
}

const Entry* Tiff::tagEntry(const Ifd& ifd, uint16_t t) const { return ifd.get(t); }

uint16_t orientationOf(std::optional<uint32_t> v) {
    if (v && *v >= 1 && *v <= 8) return static_cast<uint16_t>(*v);
    return 1;
}

}  // namespace raw
}  // namespace montage
