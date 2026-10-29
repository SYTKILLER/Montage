#include "io/psd/psd_deep.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "io/cms/convert.h"

namespace montage {
namespace psd {

size_t deepRowBytes(size_t width, uint16_t depth) {
    if (depth == 1) {
        return (width + 7) / 8;
    }
    return width * (depth / 8);
}

std::vector<uint8_t> unpackBitsPlane(const uint8_t* data, size_t len, size_t width, size_t height,
                                     uint8_t (*map)(uint8_t)) {
    const size_t rb = deepRowBytes(width, 1);
    std::vector<uint8_t> out;
    out.reserve(width * height);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            uint8_t b = 0;
            const size_t idx = y * rb + x / 8;
            if (idx < len) {
                b = data[idx];
            }
            out.push_back(map((b >> (7 - (x % 8))) & 1));
        }
    }
    return out;
}

std::optional<std::vector<uint8_t>> planeToU8(const uint8_t* data, size_t len, uint16_t depth, size_t width,
                                              size_t height, std::string& err) {
    const size_t n = width * height;
    switch (depth) {
        case 1: {
            // PSD 位图：bit=1 → 0（黑）、bit=0 → 255（白）。
            return unpackBitsPlane(
                data, len, width, height, [](uint8_t bit) { return bit == 1 ? static_cast<uint8_t>(0) : static_cast<uint8_t>(255); });
        }
        case 8: {
            std::vector<uint8_t> out(n, 0);
            std::memcpy(out.data(), data, std::min(len, n));
            return out;
        }
        case 16: {
            if (len < n * 2) {
                err = "truncated";
                return std::nullopt;
            }
            std::vector<uint8_t> out(n);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t v = static_cast<uint32_t>(data[2 * i]) << 8 | data[2 * i + 1];
                out[i] = static_cast<uint8_t>((v * 255u + 32767u) / 65535u);
            }
            return out;
        }
        case 32: {
            if (len < n * 4) {
                err = "truncated";
                return std::nullopt;
            }
            std::vector<uint8_t> out(n);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t bits = static_cast<uint32_t>(data[4 * i]) << 24 |
                                      static_cast<uint32_t>(data[4 * i + 1]) << 16 |
                                      static_cast<uint32_t>(data[4 * i + 2]) << 8 | data[4 * i + 3];
                float v = 0.0f;
                std::memcpy(&v, &bits, 4);
                if (std::isnan(v)) {
                    out[i] = 0;
                } else {
                    out[i] = static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f);
                }
            }
            return out;
        }
        default:
            err = "unsupported depth " + std::to_string(depth);
            return std::nullopt;
    }
}

namespace {

// PackBits 行解压（字节行；对拍 unpackRLE 语义——rowBytes 视作「宽」）。
bool rleToBytes(const uint8_t* data, size_t len, size_t& offset, size_t rowBytes, size_t height, bool largeDoc,
                uint8_t* plane, std::string& err) {
    std::vector<size_t> counts(height);
    for (size_t row = 0; row < height; ++row) {
        if (largeDoc) {
            if (offset + 4 > len) {
                err = "truncated";
                return false;
            }
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                v = v << 8 | data[offset++];
            }
            counts[row] = v;
        } else {
            if (offset + 2 > len) {
                err = "truncated";
                return false;
            }
            counts[row] = static_cast<size_t>(data[offset] << 8 | data[offset + 1]);
            offset += 2;
        }
    }
    std::vector<uint8_t> rowBuf(rowBytes);
    for (size_t row = 0; row < height; ++row) {
        const size_t end = offset + counts[row];
        if (end > len) {
            err = "truncated";
            return false;
        }
        size_t written = 0;
        while (written < rowBytes) {
            if (offset >= end) {
                err = "truncated";
                return false;
            }
            const int8_t n = static_cast<int8_t>(data[offset]);
            offset += 1;
            if (n >= 0) {
                const size_t count = static_cast<size_t>(n) + 1;
                if (written + count > rowBytes || offset + count > end) {
                    err = "truncated";
                    return false;
                }
                std::memcpy(rowBuf.data() + written, data + offset, count);
                offset += count;
                written += count;
            } else if (n != -128) {
                const size_t count = static_cast<size_t>(1 - static_cast<int>(n));
                if (written + count > rowBytes || offset >= end) {
                    err = "truncated";
                    return false;
                }
                std::memset(rowBuf.data() + written, data[offset], count);
                offset += 1;
                written += count;
            }
        }
        std::memcpy(plane + row * rowBytes, rowBuf.data(), rowBytes);
    }
    return true;
}

}  // namespace

std::optional<std::vector<float>> planeToF32(const uint8_t* data, size_t len, uint16_t depth,
                                             size_t width, size_t height, std::string& err) {
    const size_t n = width * height;
    switch (depth) {
        case 16: {
            if (len < n * 2) {
                err = "truncated";
                return std::nullopt;
            }
            std::vector<float> out(n);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t v = static_cast<uint32_t>(data[2 * i]) << 8 | data[2 * i + 1];
                out[i] = static_cast<float>(v) * (1.0f / 65535.0f);
            }
            return out;
        }
        case 32: {
            if (len < n * 4) {
                err = "truncated";
                return std::nullopt;
            }
            std::vector<float> out(n);
            for (size_t i = 0; i < n; ++i) {
                const uint32_t bits = static_cast<uint32_t>(data[4 * i]) << 24 |
                                      static_cast<uint32_t>(data[4 * i + 1]) << 16 |
                                      static_cast<uint32_t>(data[4 * i + 2]) << 8 | data[4 * i + 3];
                float v = 0.0f;
                std::memcpy(&v, &bits, 4);
                out[i] = std::isnan(v) ? 0.0f : v;
            }
            return out;
        }
        default:
            err = "planeToF32: unsupported depth " + std::to_string(depth);
            return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> decodeDeepChannel(int compression, size_t width, size_t height,
                                                      uint16_t depth, bool largeDoc, const uint8_t* payload,
                                                      size_t len, const int* crop, std::string& err) {
    if (width == 0 || height == 0) {
        return std::vector<uint8_t>{};
    }
    const size_t rb = deepRowBytes(width, depth);
    const int cx = crop != nullptr ? crop[0] : 0;
    const int cy = crop != nullptr ? crop[1] : 0;
    const int cw = crop != nullptr ? crop[2] : static_cast<int>(width);
    const int ch = crop != nullptr ? crop[3] : static_cast<int>(height);
    if (cw <= 0 || ch <= 0) {
        return std::vector<uint8_t>{};
    }
    if (crop != nullptr &&
        (cx < 0 || cy < 0 || cx + cw > static_cast<int>(width) || cy + ch > static_cast<int>(height))) {
        err = "truncated";
        return std::nullopt;
    }
    const size_t outRb = deepRowBytes(static_cast<size_t>(cw), depth);
    std::vector<uint8_t> out(outRb * static_cast<size_t>(ch), 0);
    if (compression == 0) {
        if (len < rb * height) {
            err = "truncated";
            return std::nullopt;
        }
        for (int row = 0; row < ch; ++row) {
            const size_t srcRow = static_cast<size_t>(cy + row) * rb +
                                  (crop != nullptr ? deepRowBytes(static_cast<size_t>(cx), depth) : 0);
            std::memcpy(out.data() + static_cast<size_t>(row) * outRb, payload + srcRow, outRb);
        }
        return out;
    }
    if (compression == 1) {
        // 整面 RLE 解压（行字节粒度），再裁剪列带。
        std::vector<uint8_t> full(rb * height, 0);
        size_t offset = 0;
        if (!rleToBytes(payload, len, offset, rb, height, largeDoc, full.data(), err)) {
            return std::nullopt;
        }
        for (int row = 0; row < ch; ++row) {
            const size_t srcRow = static_cast<size_t>(cy + row) * rb +
                                  (crop != nullptr ? deepRowBytes(static_cast<size_t>(cx), depth) : 0);
            std::memcpy(out.data() + static_cast<size_t>(row) * outRb, full.data() + srcRow, outRb);
        }
        return out;
    }
    err = "unsupported compression";
    return std::nullopt;
}

std::vector<uint8_t> cropPlane8(const std::vector<uint8_t>& plane, size_t width, size_t height, int x, int y,
                                int w, int h) {
    if (w <= 0 || h <= 0) {
        return {};
    }
    if (x == 0 && y == 0 && w == static_cast<int>(width) && h == static_cast<int>(height)) {
        return plane;
    }
    std::vector<uint8_t> out(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    for (int row = 0; row < h; ++row) {
        const size_t srcRow = static_cast<size_t>(y + row);
        if (srcRow >= height) {
            break;
        }
        const size_t copyW = std::min(static_cast<size_t>(w), width - static_cast<size_t>(x));
        std::memcpy(out.data() + static_cast<size_t>(row) * static_cast<size_t>(w),
                    plane.data() + srcRow * width + x, copyW);
    }
    return out;
}

size_t deepColorChannels(uint16_t mode) {
    switch (mode) {
        case 3:  // RGB
        case 9:  // Lab
            return 3;
        case 4:  // CMYK
            return 4;
        case 0:  // Bitmap
        case 1:  // Grayscale
        case 2:  // Indexed
        case 8:  // Duotone
            return 1;
        default:
            return 0;  // Multichannel/未知不支持
    }
}

std::optional<std::vector<uint8_t>> interleaveToRgba(uint16_t mode,
                                                     const std::vector<std::vector<uint8_t>>& colorPlanes,
                                                     const std::vector<uint8_t>* alpha, const uint8_t* palette,
                                                     size_t n, std::string& err) {
    const size_t cc = deepColorChannels(mode);
    if (cc == 0) {
        err = "unsupported color mode " + std::to_string(mode);
        return std::nullopt;
    }
    if (colorPlanes.size() != cc) {
        err = "channel size mismatch";
        return std::nullopt;
    }
    for (const auto& p : colorPlanes) {
        if (p.size() != n) {
            err = "channel size mismatch";
            return std::nullopt;
        }
    }
    if (alpha != nullptr && alpha->size() != n) {
        err = "channel size mismatch";
        return std::nullopt;
    }
    std::vector<uint8_t> out(n * 4, 255);
    if (mode == 3) {
        for (size_t i = 0; i < n; ++i) {
            out[i * 4] = colorPlanes[0][i];
            out[i * 4 + 1] = colorPlanes[1][i];
            out[i * 4 + 2] = colorPlanes[2][i];
            out[i * 4 + 3] = alpha != nullptr ? (*alpha)[i] : 255;
        }
        return out;
    }
    if (mode == 4) {
        // CMYK：存储域（反相）→ 墨量 → 受管 sRGB（相对色度 + BPC；文档内嵌 profile 经
        // CmykScope 生效，否则内置涂布 CMYK）。
        for (size_t i = 0; i < n; ++i) {
            const std::array<float, 4> cmyk = {1.0f - colorPlanes[0][i] / 255.0f,
                                               1.0f - colorPlanes[1][i] / 255.0f,
                                               1.0f - colorPlanes[2][i] / 255.0f,
                                               1.0f - colorPlanes[3][i] / 255.0f};
            const auto rgb = cms::cmykToRgb(cmyk);
            out[i * 4] = static_cast<uint8_t>(std::min(std::max(rgb[0], 0.0f), 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 1] = static_cast<uint8_t>(std::min(std::max(rgb[1], 0.0f), 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 2] = static_cast<uint8_t>(std::min(std::max(rgb[2], 0.0f), 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 3] = alpha != nullptr ? (*alpha)[i] : 255;
        }
        return out;
    }
    if (mode == 9) {
        // Lab：8-bit 存储（L* = v×100/255；a/b = v−128）→ 受管 sRGB（D50 公式）。
        for (size_t i = 0; i < n; ++i) {
            const std::array<float, 3> lab = {colorPlanes[0][i] * 100.0f / 255.0f,
                                              static_cast<float>(colorPlanes[1][i]) - 128.0f,
                                              static_cast<float>(colorPlanes[2][i]) - 128.0f};
            const auto rgb = cms::labToSrgb(lab);
            out[i * 4] = static_cast<uint8_t>(std::min(std::max(rgb[0], 0.0f), 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 1] = static_cast<uint8_t>(std::min(std::max(rgb[1], 0.0f), 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 2] = static_cast<uint8_t>(std::min(std::max(rgb[2], 0.0f), 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 3] = alpha != nullptr ? (*alpha)[i] : 255;
        }
        return out;
    }
    if (mode == 2) {
        // Indexed：调色板平面序（R 表 256 / G 表 256 / B 表 256）。
        if (palette == nullptr) {
            err = "indexed mode without palette";
            return std::nullopt;
        }
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = colorPlanes[0][i];
            out[i * 4] = palette[idx];
            out[i * 4 + 1] = palette[256 + idx];
            out[i * 4 + 2] = palette[512 + idx];
            out[i * 4 + 3] = alpha != nullptr ? (*alpha)[i] : 255;
        }
        return out;
    }
    // Grayscale / Bitmap / Duotone：复制。
    for (size_t i = 0; i < n; ++i) {
        const uint8_t g = colorPlanes[0][i];
        out[i * 4] = g;
        out[i * 4 + 1] = g;
        out[i * 4 + 2] = g;
        out[i * 4 + 3] = alpha != nullptr ? (*alpha)[i] : 255;
    }
    return out;
}

std::optional<std::vector<std::vector<uint8_t>>> rgbaToModePlanes(uint16_t mode, const uint8_t* rgba, size_t n,
                                                                  const uint8_t* palette) {
    const size_t cc = deepColorChannels(mode);
    if (cc == 0 || rgba == nullptr) {
        return std::nullopt;
    }
    std::vector<std::vector<uint8_t>> planes(cc);
    for (auto& p : planes) {
        p.resize(n);
    }
    switch (mode) {
        case 3:
            for (size_t i = 0; i < n; ++i) {
                planes[0][i] = rgba[i * 4];
                planes[1][i] = rgba[i * 4 + 1];
                planes[2][i] = rgba[i * 4 + 2];
            }
            break;
        case 4:
            for (size_t i = 0; i < n; ++i) {
                const std::array<float, 3> rgb = {rgba[i * 4] / 255.0f, rgba[i * 4 + 1] / 255.0f,
                                                  rgba[i * 4 + 2] / 255.0f};
                const auto cmyk = cms::rgbToCmyk(rgb);
                planes[0][i] = static_cast<uint8_t>(std::round((1.0f - cmyk[0]) * 255.0f));
                planes[1][i] = static_cast<uint8_t>(std::round((1.0f - cmyk[1]) * 255.0f));
                planes[2][i] = static_cast<uint8_t>(std::round((1.0f - cmyk[2]) * 255.0f));
                planes[3][i] = static_cast<uint8_t>(std::round((1.0f - cmyk[3]) * 255.0f));
            }
            break;
        case 9:
            for (size_t i = 0; i < n; ++i) {
                const auto lab = cms::srgbToLab({rgba[i * 4] / 255.0f, rgba[i * 4 + 1] / 255.0f,
                                                 rgba[i * 4 + 2] / 255.0f});
                planes[0][i] = static_cast<uint8_t>(std::round(std::min(std::max(lab[0], 0.0f), 100.0f) * 255.0f / 100.0f));
                planes[1][i] = static_cast<uint8_t>(std::min(std::max(std::round(lab[1]), -128.0f), 127.0f)) + 128;
                planes[2][i] = static_cast<uint8_t>(std::min(std::max(std::round(lab[2]), -128.0f), 127.0f)) + 128;
            }
            break;
        case 1:
        case 8: {
            for (size_t i = 0; i < n; ++i) {
                planes[0][i] = static_cast<uint8_t>(std::round(0.299f * rgba[i * 4] + 0.587f * rgba[i * 4 + 1] +
                                                               0.114f * rgba[i * 4 + 2]));
            }
            break;
        }
        case 2: {
            if (palette == nullptr) {
                return std::nullopt;
            }
            for (size_t i = 0; i < n; ++i) {
                // 最近调色板项（欧氏 RGB）。
                int best = 0;
                int bestD = 1 << 30;
                for (size_t k = 0; k < 256; ++k) {
                    const int dr = static_cast<int>(rgba[i * 4]) - palette[k];
                    const int dg = static_cast<int>(rgba[i * 4 + 1]) - palette[256 + k];
                    const int db = static_cast<int>(rgba[i * 4 + 2]) - palette[512 + k];
                    const int d = dr * dr + dg * dg + db * db;
                    if (d < bestD) {
                        bestD = d;
                        best = static_cast<int>(k);
                    }
                }
                planes[0][i] = static_cast<uint8_t>(best);
            }
            break;
        }
        default:
            return std::nullopt;
    }
    return planes;
}

std::vector<uint8_t> planeToDepth(const std::vector<uint8_t>& plane8, uint16_t depth, size_t width,
                                  size_t height) {
    const size_t n = width * height;
    switch (depth) {
        case 8:
            return plane8;
        case 16: {
            std::vector<uint8_t> out(n * 2, 0);
            for (size_t i = 0; i < n && i < plane8.size(); ++i) {
                const uint32_t v = (static_cast<uint32_t>(plane8[i]) * 65535u + 127u) / 255u;
                out[2 * i] = static_cast<uint8_t>(v >> 8);
                out[2 * i + 1] = static_cast<uint8_t>(v);
            }
            return out;
        }
        case 32: {
            std::vector<uint8_t> out(n * 4, 0);
            for (size_t i = 0; i < n && i < plane8.size(); ++i) {
                const float v = plane8[i] / 255.0f;
                uint32_t bits = 0;
                std::memcpy(&bits, &v, 4);
                out[4 * i] = static_cast<uint8_t>(bits >> 24);
                out[4 * i + 1] = static_cast<uint8_t>(bits >> 16);
                out[4 * i + 2] = static_cast<uint8_t>(bits >> 8);
                out[4 * i + 3] = static_cast<uint8_t>(bits);
            }
            return out;
        }
        case 1: {
            const size_t rb = deepRowBytes(width, 1);
            std::vector<uint8_t> out(rb * height, 0);
            for (size_t i = 0; i < n && i < plane8.size(); ++i) {
                if (plane8[i] >= 128) {
                    out[(i / width) * rb + (i % width) / 8] |= static_cast<uint8_t>(0x80u >> (i % 8));
                }
            }
            return out;
        }
        default:
            return plane8;
    }
}

std::vector<uint8_t> rgbaToGrayPlane(const uint8_t* rgba, size_t n) {
    std::vector<uint8_t> out(n, 0);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(std::round(0.299f * rgba[i * 4] + 0.587f * rgba[i * 4 + 1] +
                                                 0.114f * rgba[i * 4 + 2]));
    }
    return out;
}

}  // namespace psd
}  // namespace montage
