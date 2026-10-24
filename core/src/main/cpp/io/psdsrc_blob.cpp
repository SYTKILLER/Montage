#include "io/psdsrc_blob.h"

#include <cstring>

namespace montage {
namespace io {

namespace {

void putU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

void putU32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v >> 24));
    b.push_back(static_cast<uint8_t>(v >> 16));
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v));
}

void putU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
}

void putI32(std::vector<uint8_t>& b, int32_t v) { putU32(b, static_cast<uint32_t>(v)); }

void putI16(std::vector<uint8_t>& b, int16_t v) { putU16(b, static_cast<uint16_t>(v)); }

struct Rd {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t at = 0;
    bool need(size_t k) {
        if (at + k > n) {
            return false;
        }
        return true;
    }
    bool u16(uint16_t& v) {
        if (!need(2)) return false;
        v = static_cast<uint16_t>(p[at] << 8 | p[at + 1]);
        at += 2;
        return true;
    }
    bool u32(uint32_t& v) {
        if (!need(4)) return false;
        v = static_cast<uint32_t>(p[at]) << 24 | static_cast<uint32_t>(p[at + 1]) << 16 |
            static_cast<uint32_t>(p[at + 2]) << 8 | p[at + 3];
        at += 4;
        return true;
    }
    bool u64(uint64_t& v) {
        if (!need(8)) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) {
            v = v << 8 | p[at + i];
        }
        at += 8;
        return true;
    }
    bool i16(int16_t& v) {
        uint16_t t;
        if (!u16(t)) return false;
        v = static_cast<int16_t>(t);
        return true;
    }
    bool i32(int32_t& v) {
        uint32_t t;
        if (!u32(t)) return false;
        v = static_cast<int32_t>(t);
        return true;
    }
    bool bytes(std::vector<uint8_t>& out, size_t k) {
        if (!need(k)) return false;
        out.assign(p + at, p + at + k);
        at += k;
        return true;
    }
};

}  // namespace

std::vector<uint8_t> encodePsdSrcBlob(const PsdSrcMeta& m) {
    std::vector<uint8_t> b;
    b.reserve(64 + m.channelTable.size() * 10 + m.section.size());
    b.insert(b.end(), {'P', 'S', 'R', 'C'});
    putU16(b, 1);
    putU16(b, m.depth);
    putU16(b, m.mode);
    putU16(b, m.channels);
    b.push_back(m.cropped ? 1 : 0);
    b.push_back(0);
    b.push_back(0);
    b.push_back(0);
    putU64(b, m.importHash);
    putI32(b, m.srcLeft);
    putI32(b, m.srcTop);
    putI32(b, m.srcRight);
    putI32(b, m.srcBottom);
    putI32(b, m.srcMaskLeft);
    putI32(b, m.srcMaskTop);
    putI32(b, m.srcMaskRight);
    putI32(b, m.srcMaskBottom);
    putU16(b, static_cast<uint16_t>(m.channelTable.size()));
    for (const auto& ch : m.channelTable) {
        putI16(b, ch.first);
        putU64(b, static_cast<uint64_t>(ch.second));
    }
    b.insert(b.end(), m.section.begin(), m.section.end());
    return b;
}

bool decodePsdSrcBlob(const uint8_t* data, size_t len, PsdSrcMeta& out, std::string& err) {
    Rd r{data, len, 0};
    if (!r.need(4) || std::memcmp(data, "PSRC", 4) != 0) {
        err = "bad psdsrc blob magic";
        return false;
    }
    r.at = 4;
    uint16_t ver = 0;
    if (!r.u16(ver) || ver != 1) {
        err = "bad psdsrc blob version";
        return false;
    }
    if (!r.u16(out.depth) || !r.u16(out.mode) || !r.u16(out.channels)) {
        err = "truncated psdsrc blob";
        return false;
    }
    if (!r.need(4)) {
        err = "truncated psdsrc blob";
        return false;
    }
    out.cropped = r.p[r.at] != 0;
    r.at += 4;
    if (!r.u64(out.importHash) || !r.i32(out.srcLeft) || !r.i32(out.srcTop) || !r.i32(out.srcRight) ||
        !r.i32(out.srcBottom) || !r.i32(out.srcMaskLeft) || !r.i32(out.srcMaskTop) ||
        !r.i32(out.srcMaskRight) || !r.i32(out.srcMaskBottom)) {
        err = "truncated psdsrc blob";
        return false;
    }
    uint16_t count = 0;
    if (!r.u16(count)) {
        err = "truncated psdsrc blob";
        return false;
    }
    out.channelTable.resize(count);
    for (size_t i = 0; i < count; ++i) {
        int16_t id = 0;
        uint64_t l = 0;
        if (!r.i16(id) || !r.u64(l)) {
            err = "truncated psdsrc blob";
            return false;
        }
        out.channelTable[i] = {id, static_cast<int64_t>(l)};
    }
    if (!r.bytes(out.section, r.n - r.at)) {
        err = "truncated psdsrc blob";
        return false;
    }
    return true;
}

}  // namespace io
}  // namespace montage
