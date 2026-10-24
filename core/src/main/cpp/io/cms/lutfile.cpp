#include "io/cms/lutfile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace montage {
namespace cms {

namespace {

bool checkedDataLen(size_t size, size_t& entries, size_t& dataLen, std::string& err) {
    if (size < 2 || size > kLutFileMaxSize) {
        err = "unsupported LUT size " + std::to_string(size);
        return false;
    }
    if (size > std::numeric_limits<size_t>::max() / (size * 3)) {
        err = "unsupported LUT size " + std::to_string(size);
        return false;
    }
    entries = size * size * size;
    dataLen = entries * 3;
    return true;
}

std::optional<LutFile> checked(LutFile l, std::string& err) {
    size_t entries = 0, dataLen = 0;
    if (!checkedDataLen(l.size, entries, dataLen, err)) {
        return std::nullopt;
    }
    if (l.data.size() != dataLen) {
        err = "expected " + std::to_string(entries) + " entries, found " + std::to_string(l.data.size() / 3);
        return std::nullopt;
    }
    for (float v : l.data) {
        if (!std::isfinite(v)) {
            err = "non-finite LUT value";
            return std::nullopt;
        }
    }
    return l;
}

}  // namespace

LutFile LutFile::identity(size_t size) {
    return fromFn("Identity", size, [](const std::array<float, 3>& c) { return std::array<float, 3>{c[0], c[1], c[2]}; });
}

std::optional<LutFile> parseLutFileName(const std::string& fileName, const uint8_t* bytes, size_t len,
                                        std::string& err) {
    std::string lower = fileName;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    // 文本嗅探：NUL 截断（二进制防御）。
    std::string text;
    text.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        text.push_back(bytes[i] == 0 ? ' ' : static_cast<char>(bytes[i]));
    }
    auto startsWithLt = [&] {
        size_t i = 0;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
            ++i;
        }
        return i < text.size() && text[i] == '<';
    };
    if (lower.size() >= 4 && lower.rfind(".3dl") == lower.size() - 4) {
        return parse3dl(text.data(), text.size(), err);
    }
    if ((lower.size() >= 5 && lower.rfind(".look") == lower.size() - 5) || startsWithLt()) {
        return parseLook(text.data(), text.size(), err);
    }
    return parseCube(text.data(), text.size(), err);
}

std::optional<LutFile> parseCube(const char* text, size_t len, std::string& err) {
    std::string title;
    size_t size3 = 0, size1 = 0;
    float dmin[3] = {0.0f, 0.0f, 0.0f};
    float dmax[3] = {1.0f, 1.0f, 1.0f};
    std::vector<float> rows;
    size_t pos = 0;
    while (pos < len) {
        // 逐行。
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') {
            ++eol;
        }
        // 去注释与空白。
        std::string line;
        for (size_t i = pos; i < eol; ++i) {
            if (text[i] == '#') {
                break;
            }
            line.push_back(text[i]);
        }
        pos = eol + 1;
        // trim
        const char* ws = " \t\r\n";
        size_t b = line.find_first_not_of(ws);
        if (b == std::string::npos) {
            continue;
        }
        size_t e = line.find_last_not_of(ws);
        line = line.substr(b, e - b + 1);
        // 分词。
        std::vector<std::string> tok;
        {
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && std::strchr(ws, line[i]) != nullptr) {
                    ++i;
                }
                size_t s = i;
                while (i < line.size() && std::strchr(ws, line[i]) == nullptr) {
                    ++i;
                }
                if (i > s) {
                    tok.push_back(line.substr(s, i - s));
                }
            }
        }
        if (tok.empty()) {
            continue;
        }
        const std::string& key = tok[0];
        auto nums = [&](size_t from) -> std::optional<std::array<float, 3>> {
            std::array<float, 3> v = {0.0f, 0.0f, 0.0f};
            size_t n = 0;
            for (size_t i = from; i < tok.size() && n < 3; ++i) {
                char* end = nullptr;
                const double d = std::strtod(tok[i].c_str(), &end);
                if (end == tok[i].c_str()) {
                    continue;
                }
                v[n++] = static_cast<float>(d);
            }
            if (n == 3) {
                return v;
            }
            return std::nullopt;
        };
        if (key == "TITLE") {
            std::string t = line.substr(5);
            // trim + 去引号
            size_t tb = t.find_first_not_of(ws);
            size_t te = t.find_last_not_of(ws);
            t = tb == std::string::npos ? "" : t.substr(tb, te - tb + 1);
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
                t = t.substr(1, t.size() - 2);
            }
            title = t;
        } else if (key == "LUT_3D_SIZE") {
            if (tok.size() < 2) {
                err = "bad LUT_3D_SIZE";
                return std::nullopt;
            }
            size3 = static_cast<size_t>(std::strtoul(tok[1].c_str(), nullptr, 10));
        } else if (key == "LUT_1D_SIZE") {
            if (tok.size() < 2) {
                err = "bad LUT_1D_SIZE";
                return std::nullopt;
            }
            size1 = static_cast<size_t>(std::strtoul(tok[1].c_str(), nullptr, 10));
        } else if (key == "DOMAIN_MIN") {
            auto v = nums(1);
            if (!v) {
                err = "bad line `" + line + "`";
                return std::nullopt;
            }
            for (int k = 0; k < 3; ++k) {
                dmin[k] = (*v)[k];
            }
        } else if (key == "DOMAIN_MAX") {
            auto v = nums(1);
            if (!v) {
                err = "bad line `" + line + "`";
                return std::nullopt;
            }
            for (int k = 0; k < 3; ++k) {
                dmax[k] = (*v)[k];
            }
        } else if (key == "LUT_3D_INPUT_RANGE" || key == "LUT_1D_INPUT_RANGE") {
            std::vector<float> v;
            for (size_t i = 1; i < tok.size(); ++i) {
                char* end = nullptr;
                const double d = std::strtod(tok[i].c_str(), &end);
                if (end != tok[i].c_str()) {
                    v.push_back(static_cast<float>(d));
                }
            }
            if (v.size() == 2) {
                for (int k = 0; k < 3; ++k) {
                    dmin[k] = v[0];
                    dmax[k] = v[1];
                }
            }
        } else if (std::isdigit(static_cast<unsigned char>(key[0])) || key[0] == '-' || key[0] == '+' ||
                   key[0] == '.') {
            std::vector<float> v;
            char* end = nullptr;
            const double d = std::strtod(key.c_str(), &end);
            if (end == key.c_str()) {
                err = "bad number in `" + line + "`";
                return std::nullopt;
            }
            v.push_back(static_cast<float>(d));
            for (size_t i = 1; i < tok.size(); ++i) {
                char* end2 = nullptr;
                const double d2 = std::strtod(tok[i].c_str(), &end2);
                if (end2 != tok[i].c_str()) {
                    v.push_back(static_cast<float>(d2));
                }
            }
            if (v.size() != 3) {
                err = "bad data line `" + line + "`";
                return std::nullopt;
            }
            rows.insert(rows.end(), v.begin(), v.end());
        }
        // 未知关键字（如 Resolve 的 LUT_IN_VIDEO_RANGE）按规范忽略。
    }
    bool domainOk = true;
    for (int k = 0; k < 3; ++k) {
        if (!(dmin[k] < dmax[k]) || !std::isfinite(dmin[k]) || !std::isfinite(dmax[k])) {
            domainOk = false;
        }
    }
    if ((size3 > 0 || size1 >= 2) && !domainOk) {
        err = "DOMAIN_MIN must lie below DOMAIN_MAX";
        return std::nullopt;
    }
    if (size3 > 0) {
        LutFile l;
        l.title = title;
        l.size = size3;
        l.data = std::move(rows);
        for (int k = 0; k < 3; ++k) {
            l.domainMin[k] = dmin[k];
            l.domainMax[k] = dmax[k];
        }
        return checked(std::move(l), err);
    }
    if (size1 >= 2) {
        if (size1 > std::numeric_limits<size_t>::max() / 3 / 2) {
            err = "unsupported 1D LUT size " + std::to_string(size1);
            return std::nullopt;
        }
        const size_t expectedRows = size1 * 3;
        if (rows.size() != expectedRows) {
            err = "no LUT_3D_SIZE in .cube file";
            return std::nullopt;
        }
        // 1D 表反正重采样到 0..1 立方体，故其输入域烘焙进重采样：输入 v 读表于
        // (v − min)/(max − min)，域外饱和。
        auto curve = [&](int ch, float v) {
            const float t = (v - dmin[ch]) / (dmax[ch] - dmin[ch]);
            const float x = (std::isfinite(t) ? std::min(std::max(t, 0.0f), 1.0f) : 0.0f) *
                            static_cast<float>(size1 - 1);
            size_t i = static_cast<size_t>(x);
            if (i > size1 - 2) {
                i = size1 - 2;
            }
            const float f = x - static_cast<float>(i);
            return rows[i * 3 + ch] * (1.0f - f) + rows[(i + 1) * 3 + ch] * f;
        };
        LutFile l = LutFile::fromFn("", 33, [&](const std::array<float, 3>& c) {
            return std::array<float, 3>{curve(0, c[0]), curve(1, c[1]), curve(2, c[2])};
        });
        l.title = title;
        return checked(std::move(l), err);
    }
    err = "no LUT_3D_SIZE in .cube file";
    return std::nullopt;
}

std::optional<LutFile> parse3dl(const char* text, size_t len, std::string& err) {
    std::vector<std::array<double, 3>> rows;
    size_t pos = 0;
    while (pos < len) {
        size_t eol = pos;
        while (eol < len && text[eol] != '\n') {
            ++eol;
        }
        std::string line;
        for (size_t i = pos; i < eol; ++i) {
            if (text[i] == '#') {
                break;
            }
            line.push_back(text[i]);
        }
        pos = eol + 1;
        const char* ws = " \t\r\n";
        size_t b = line.find_first_not_of(ws);
        if (b == std::string::npos) {
            continue;
        }
        size_t e = line.find_last_not_of(ws);
        line = line.substr(b, e - b + 1);
        if (std::isalpha(static_cast<unsigned char>(line[0]))) {
            continue;  // 头部行（可选 shaper 假定线性）
        }
        std::vector<double> v;
        {
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && std::strchr(ws, line[i]) != nullptr) {
                    ++i;
                }
                size_t s = i;
                while (i < line.size() && std::strchr(ws, line[i]) == nullptr) {
                    ++i;
                }
                if (i > s) {
                    char* end = nullptr;
                    const double d = std::strtod(line.substr(s, i - s).c_str(), &end);
                    if (end != nullptr && *end == '\0') {
                        v.push_back(d);
                    }
                }
            }
        }
        if (v.size() == 3) {
            rows.push_back({v[0], v[1], v[2]});
        }
        // 多于三个数的行是输入 shaper；假定线性。
    }
    const double cbrtN = std::cbrt(static_cast<double>(rows.size()));
    const size_t n = static_cast<size_t>(std::llround(cbrtN));
    if (n < 2 || n * n * n != rows.size()) {
        err = std::to_string(rows.size()) + " rows is not a cube";
        return std::nullopt;
    }
    double maxV = 0.0;
    for (const auto& r : rows) {
        for (double x : r) {
            maxV = std::max(maxV, x);
        }
    }
    double scale = 1.0;
    if (maxV > 1.0) {
        const double cand[4] = {1023.0, 4095.0, 16383.0, 65535.0};
        scale = maxV;
        for (double s : cand) {
            if (maxV <= s) {
                scale = s;
                break;
            }
        }
    }
    LutFile l;
    l.size = n;
    l.data.assign(n * n * n * 3, 0.0f);
    for (size_t i = 0; i < rows.size(); ++i) {
        const size_t r = i / (n * n), g = (i / n) % n, b = i % n;
        const size_t at = ((b * n + g) * n + r) * 3;
        for (int k = 0; k < 3; ++k) {
            l.data[at + k] = static_cast<float>(rows[i][k] / scale);
        }
    }
    return checked(std::move(l), err);
}

std::optional<LutFile> parseLook(const char* text, size_t len, std::string& err) {
    auto tag = [&](const char* name) -> std::optional<std::string> {
        const std::string open = std::string("<") + name + ">";
        const std::string close = std::string("</") + name + ">";
        const size_t s = std::string(text, len).find(open);
        if (s == std::string::npos) {
            return std::nullopt;
        }
        const size_t cs = std::string(text, len).find(close, s + open.size());
        if (cs == std::string::npos) {
            return std::nullopt;
        }
        std::string v = std::string(text, len).substr(s + open.size(), cs - s - open.size());
        // trim + 去引号
        const char* ws = " \t\r\n";
        size_t b = v.find_first_not_of(ws);
        if (b == std::string::npos) {
            return std::string();
        }
        size_t e = v.find_last_not_of(ws);
        v = v.substr(b, e - b + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            v = v.substr(1, v.size() - 2);
        }
        return v;
    };
    const auto sizeStr = tag("size");
    if (!sizeStr) {
        err = "no <size> in .look";
        return std::nullopt;
    }
    const size_t size = static_cast<size_t>(std::strtoul(sizeStr->c_str(), nullptr, 10));
    size_t entries = 0, dataLen = 0;
    if (!checkedDataLen(size, entries, dataLen, err)) {
        return std::nullopt;
    }
    const auto dataStr = tag("data");
    if (!dataStr) {
        err = "no <data> in .look";
        return std::nullopt;
    }
    std::vector<uint8_t> hex;
    for (char c : *dataStr) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hex.push_back(static_cast<uint8_t>(c));
        }
    }
    auto nib = [](uint8_t c) -> uint8_t {
        if (c >= '0' && c <= '9') {
            return static_cast<uint8_t>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<uint8_t>(c - 'a' + 10);
        }
        if (c >= 'A' && c <= 'F') {
            return static_cast<uint8_t>(c - 'A' + 10);
        }
        return 0;
    };
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(nib(hex[i]) << 4 | nib(hex[i + 1])));
    }
    std::vector<float> floats;
    for (size_t i = 0; i + 3 < bytes.size(); i += 4) {
        float f = 0.0f;
        const uint32_t bits = static_cast<uint32_t>(bytes[i]) | static_cast<uint32_t>(bytes[i + 1]) << 8 |
                              static_cast<uint32_t>(bytes[i + 2]) << 16 |
                              static_cast<uint32_t>(bytes[i + 3]) << 24;
        std::memcpy(&f, &bits, 4);
        floats.push_back(f);
    }
    const size_t rgbaEntries = entries * 4;
    std::vector<float> data;
    if (floats.size() == rgbaEntries) {
        for (size_t i = 0; i < floats.size(); i += 4) {
            data.push_back(floats[i]);
            data.push_back(floats[i + 1]);
            data.push_back(floats[i + 2]);
        }
    } else {
        data = std::move(floats);
    }
    LutFile l;
    l.size = size;
    l.data = std::move(data);
    if (const auto t = tag("title")) {
        l.title = *t;
    }
    return checked(std::move(l), err);
}

std::string writeCube(const LutFile& l) {
    std::string s;
    char buf[64];
    if (!l.title.empty()) {
        std::string t = l.title;
        for (char& c : t) {
            if (c == '"') {
                c = '\'';
            }
        }
        s += "TITLE \"" + t + "\"\n";
    }
    std::snprintf(buf, sizeof(buf), "LUT_3D_SIZE %zu\n", l.size);
    s += buf;
    for (size_t i = 0; i + 2 < l.data.size(); i += 3) {
        std::snprintf(buf, sizeof(buf), "%.6f %.6f %.6f\n", static_cast<double>(l.data[i]),
                      static_cast<double>(l.data[i + 1]), static_cast<double>(l.data[i + 2]));
        s += buf;
    }
    return s;
}

float luma(const std::array<float, 3>& c) {
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

float smoothstep(float x) {
    const float v = std::min(std::max(x, 0.0f), 1.0f);
    return v * v * (3.0f - 2.0f * v);
}

const std::pair<const char*, const char*>* builtinLookTable(size_t& count) {
    static const std::pair<const char*, const char*> kTable[8] = {
        {"warm", "Warm Filter"},         {"cool", "Cool Filter"},
        {"tealOrange", "Teal & Orange"}, {"bleachBypass", "Bleach Bypass"},
        {"fadedFilm", "Faded Film"},     {"dayForNight", "Day for Night"},
        {"monoContrast", "Mono High Contrast"}, {"crossProcess", "Cross Process"},
    };
    count = 8;
    return kTable;
}

std::optional<LutFile> builtinLook(const std::string& id) {
    size_t count = 0;
    const auto* table = builtinLookTable(count);
    const char* label = nullptr;
    for (size_t i = 0; i < count; ++i) {
        if (id == table[i].first) {
            label = table[i].second;
            break;
        }
    }
    if (label == nullptr) {
        return std::nullopt;
    }
    auto fn = [&](const std::array<float, 3>& c) -> std::array<float, 3> {
        if (id == "warm") {
            return {c[0] * 0.92f + 0.08f, c[1] * 0.97f + 0.02f, c[2] * 0.85f};
        }
        if (id == "cool") {
            return {c[0] * 0.86f, c[1] * 0.97f + 0.01f, c[2] * 0.9f + 0.1f};
        }
        if (id == "tealOrange") {
            // 阴影偏青、高光偏橙、肤色中间调保留。
            const float l = luma(c);
            const float t = smoothstep(l);
            const float teal[3] = {0.0f, 0.5f, 0.55f};
            const float orange[3] = {1.0f, 0.62f, 0.3f};
            std::array<float, 3> o{};
            for (int k = 0; k < 3; ++k) {
                const float tint = teal[k] * (1.0f - t) + orange[k] * t;
                o[k] = c[k] * 0.75f + std::min(tint * l * 1.1f, 1.0f) * 0.25f + (smoothstep(c[k]) - c[k]) * 0.3f;
            }
            return o;
        }
        if (id == "bleachBypass") {
            const float l = luma(c);
            const float s = smoothstep(l);
            std::array<float, 3> o{};
            for (int k = 0; k < 3; ++k) {
                const float desat = c[k] * 0.45f + l * 0.55f;
                o[k] = desat * 0.5f + s * 0.5f;
            }
            return o;
        }
        if (id == "fadedFilm") {
            const float l = luma(c);
            const float tint[3] = {1.0f, 0.96f, 0.88f};
            std::array<float, 3> o{};
            for (int k = 0; k < 3; ++k) {
                o[k] = (0.08f + (c[k] * 0.8f + l * 0.2f) * 0.84f) * tint[k] + 0.02f;
            }
            return o;
        }
        if (id == "dayForNight") {
            const float l = luma(c);
            return {l * 0.35f, l * 0.45f + c[1] * 0.05f, l * 0.65f + c[2] * 0.15f};
        }
        if (id == "monoContrast") {
            const float l = smoothstep(smoothstep(luma(c)));
            return {l, l, l};
        }
        // crossProcess
        return {smoothstep(c[0]) * 1.05f, c[1] * 1.1f - 0.03f, c[2] * 0.7f + 0.15f};
    };
    return LutFile::fromFn(label, 33, fn);
}

// ---- M22 收口：进程级用户 LUT 槽 ----

namespace {

struct LookupSlots {
    static constexpr unsigned kSlots = 16;
    std::optional<LutFile> slots[kSlots];
    unsigned next = 0;
    unsigned used = 0;
};

LookupSlots& lookupSlots() {
    static LookupSlots s;
    return s;
}

}  // namespace

std::optional<LutFile> importedLookup(unsigned slot) {
    LookupSlots& s = lookupSlots();
    if (slot >= LookupSlots::kSlots) return std::nullopt;
    return s.slots[slot];
}

unsigned storeImportedLookup(const LutFile& l) {
    LookupSlots& s = lookupSlots();
    const unsigned slot = s.next;
    s.slots[slot] = l;
    s.next = (s.next + 1u) % LookupSlots::kSlots;
    if (s.used < LookupSlots::kSlots) ++s.used;
    return slot;
}

unsigned importedLookupCount() {
    return lookupSlots().used;
}

}  // namespace cms
}  // namespace montage