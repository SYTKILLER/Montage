#include "io/codecs/hdr_codec.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace montage {
namespace io {
namespace codecs {

namespace {

constexpr uint32_t kLimit = 100ull * 1000 * 1000;

// 读一行（含 \n；返回新 pos）
size_t readLine(const uint8_t* data, size_t len, size_t pos, std::string& line) {
    line.clear();
    while (pos < len) {
        const char c = static_cast<char>(data[pos++]);
        line.push_back(c);
        if (c == '\n') {
            break;
        }
    }
    return pos;
}

}  // namespace

bool hdrDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err) {
    if (len < 11) {
        err = "hdr: truncated";
        return false;
    }
    std::string line;
    size_t pos = 0;
    pos = readLine(data, len, pos, line);
    if (line.rfind("#?RADIANCE\n", 0) != 0 && line.rfind("#?RGBE\n", 0) != 0) {
        err = "hdr: bad magic";
        return false;
    }
    // 头部键值 + EXPOSURE
    float exposure = 1.0f;
    bool formatOk = false;
    while (pos < len) {
        pos = readLine(data, len, pos, line);
        if (line == "\n") {
            break;
        }
        if (line.rfind("EXPOSURE=", 0) == 0) {
            exposure = std::strtof(line.c_str() + 9, nullptr);
            if (!(exposure > 0.0f) || !std::isfinite(exposure)) {
                exposure = 1.0f;
            }
        } else if (line.rfind("FORMAT=", 0) == 0) {
            if (line.find("32-bit_rle_rgbe") == std::string::npos) {
                err = "hdr: unsupported format (need 32-bit_rle_rgbe)";
                return false;
            }
            formatOk = true;
        }
    }
    if (!formatOk) {
        err = "hdr: missing FORMAT";
        return false;
    }
    // 分辨率行（v1 支持 -Y H +X W 与 +X W -Y H）
    pos = readLine(data, len, pos, line);
    int w = 0;
    int h = 0;
    {
        char a1[8] = {0}, a3[8] = {0};
        int v1 = 0, v2 = 0;
        if (std::sscanf(line.c_str(), "%7s %d %7s %d", a1, &v1, a3, &v2) != 4) {
            err = "hdr: bad resolution line";
            return false;
        }
        if (std::strcmp(a1, "-Y") == 0 && std::strcmp(a3, "+X") == 0) {
            h = v1;
            w = v2;
        } else if (std::strcmp(a1, "+X") == 0 && std::strcmp(a3, "-Y") == 0) {
            w = v1;
            h = v2;
        } else {
            err = "hdr: unsupported resolution layout";
            return false;
        }
    }
    if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * h > kLimit) {
        err = "hdr: size out of limits";
        return false;
    }

    out.w = static_cast<uint32_t>(w);
    out.h = static_cast<uint32_t>(h);
    out.rgba.assign(static_cast<uint64_t>(w) * h * 4, 0);

    std::vector<uint8_t> rgbe;
    for (int y = 0; y < h; ++y) {
        rgbe.clear();
        if (pos + 4 <= len && data[pos] == 2 && data[pos + 1] == 2 &&
            (data[pos + 2] > 0 || data[pos + 3] > 0)) {
            // 新式自适应 RLE：每扫描线 4 通道分别 RLE
            const int lineH = (static_cast<int>(data[pos + 2]) << 8) | data[pos + 3];
            if (lineH != h) {
                err = "hdr: scanline height mismatch";
                return false;
            }
            pos += 4;
            rgbe.resize(static_cast<size_t>(w) * 4);
            for (int c = 0; c < 4; ++c) {
                int x = 0;
                while (x < w) {
                    if (pos + 1 >= len) {
                        err = "hdr: adaptive rle truncated";
                        return false;
                    }
                    const int count = data[pos];
                    if (count > 128) {  // run：count-128 重复
                        if (pos + 1 >= len) {
                            err = "hdr: adaptive rle value truncated";
                            return false;
                        }
                        const int n = count - 128;
                        const uint8_t v = data[pos + 1];
                        pos += 2;
                        if (x + n > w) {
                            err = "hdr: adaptive rle run overflow";
                            return false;
                        }
                        for (int i = 0; i < n; ++i) {
                            rgbe[static_cast<size_t>(x + i) * 4 + c] = v;
                        }
                        x += n;
                    } else {  // 字面量 count
                        ++pos;
                        if (pos + count > len) {
                            err = "hdr: adaptive rle literal truncated";
                            return false;
                        }
                        if (x + count > w) {
                            err = "hdr: adaptive rle literal overflow";
                            return false;
                        }
                        for (int i = 0; i < count; ++i) {
                            rgbe[static_cast<size_t>(x + i) * 4 + c] = data[pos + i];
                        }
                        pos += count;
                        x += count;
                    }
                }
            }
        } else {
            // 旧式：整线按 4 元组流（标准 RLE 以 4 元组为单位）
            rgbe.resize(static_cast<size_t>(w) * 4);
            int x = 0;
            while (x < w) {
                if (pos >= len) {
                    err = "hdr: legacy scanline truncated";
                    return false;
                }
                const uint8_t c = data[pos];
                if (c < 128) {  // 字面量 c 个 4 元组
                    ++pos;
                    if (pos + static_cast<size_t>(c) * 4 > len) {
                        err = "hdr: legacy literal truncated";
                        return false;
                    }
                    for (int i = 0; i < c && x < w; ++i) {
                        std::memcpy(rgbe.data() + static_cast<size_t>(x) * 4, data + pos, 4);
                        pos += 4;
                        ++x;
                    }
                } else if (c > 128) {  // run：(c-128) 个同一 4 元组
                    if (pos + 4 >= len + 1) {
                        err = "hdr: legacy run truncated";
                        return false;
                    }
                    const int n = c - 128;
                    ++pos;
                    if (pos + 4 > len) {
                        err = "hdr: legacy run value truncated";
                        return false;
                    }
                    for (int i = 0; i < n && x < w; ++i) {
                        std::memcpy(rgbe.data() + static_cast<size_t>(x) * 4, data + pos, 4);
                        ++x;
                    }
                    pos += 4;
                } else {
                    err = "hdr: legacy rle zero run";
                    return false;
                }
            }
        }
        // RGBE → 线性 → sRGB8
        uint8_t* drow = out.rgba.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            float lin[3];
            rgbeToLinear(rgbe[static_cast<size_t>(x) * 4 + 0], rgbe[static_cast<size_t>(x) * 4 + 1],
                         rgbe[static_cast<size_t>(x) * 4 + 2], rgbe[static_cast<size_t>(x) * 4 + 3],
                         lin);
            drow[x * 4 + 0] = linearToSrgb8(lin[0] * exposure);
            drow[x * 4 + 1] = linearToSrgb8(lin[1] * exposure);
            drow[x * 4 + 2] = linearToSrgb8(lin[2] * exposure);
            drow[x * 4 + 3] = 255;
        }
    }
    return true;
}

}  // namespace codecs
}  // namespace io
}  // namespace montage
