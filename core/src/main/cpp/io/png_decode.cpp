#include "io/png_decode.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <zlib.h>

namespace montage {
namespace io {
namespace {

constexpr size_t kMaxPngBytes = 512ull * 1024 * 1024;
constexpr size_t kMaxDims = 30000;

// 大端读
uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Paeth 预测器（PNG spec §6）
uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    const int pa = p - a < 0 ? a - p : p - a;
    const int pb = p - b < 0 ? b - p : p - b;
    const int pc = p - c < 0 ? c - p : p - c;
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

}  // namespace

std::shared_ptr<const TileGrid> decodePngToGrid(const uint8_t* data, size_t size, std::string& err) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 8 + 12 + 13 || size > kMaxPngBytes || std::memcmp(data, kSig, 8) != 0) {
        err = "not a png";
        return nullptr;
    }
    uint32_t width = 0;
    uint32_t height = 0;
    int channels = 0;
    bool idatSeen = false;
    std::vector<uint8_t> idat;
    size_t pos = 8;
    while (pos + 12 <= size) {
        const uint32_t len = be32(data + pos);
        const char* type = reinterpret_cast<const char*>(data + pos + 4);
        if (len > size - pos - 12) {
            err = "png chunk overflow";
            return nullptr;
        }
        const uint8_t* payload = data + pos + 8;
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (len != 13) {
                err = "bad IHDR";
                return nullptr;
            }
            width = be32(payload);
            height = be32(payload + 4);
            const int depth = payload[8];
            const int colorType = payload[9];
            const int interlace = payload[12];
            if (width == 0 || height == 0 || width > kMaxDims || height > kMaxDims) {
                err = "png size out of range";
                return nullptr;
            }
            if (depth != 8 || interlace != 0) {
                err = "unsupported png (depth/interlace)";
                return nullptr;
            }
            switch (colorType) {
                case 0: channels = 1; break;  // 灰度
                case 4: channels = 2; break;  // 灰度+A
                case 2: channels = 3; break;  // RGB
                case 6: channels = 4; break;  // RGBA
                default:
                    err = "unsupported png color type";
                    return nullptr;
            }
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            err = "unsupported png palette";
            return nullptr;
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            idatSeen = true;
            idat.insert(idat.end(), payload, payload + len);
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }
    if (!idatSeen || width == 0 || height == 0) {
        err = "png missing IHDR/IDAT";
        return nullptr;
    }

    // zlib 流 → 过滤前原始扫描线（每行前 1 filter 字节）
    const size_t stride = static_cast<size_t>(width) * channels;
    std::vector<uint8_t> raw((stride + 1) * height);
    uLongf rawLen = raw.size();
    if (uncompress(raw.data(), &rawLen, idat.data(), idat.size()) != Z_OK ||
        rawLen != raw.size()) {
        err = "png idat inflate failed";
        return nullptr;
    }

    // 解过滤 + 展开为 RGBA8888 行缓冲，按瓦片行 memcpy 进 builder
    TileGridBuilder builder((width + kTileSize - 1u) / kTileSize,
                            (height + kTileSize - 1u) / kTileSize);
    std::vector<uint8_t> prev(stride, 0);
    std::vector<uint8_t> rgbaRow(static_cast<size_t>(width) * 4u);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t filter = raw[y * (stride + 1)];
        uint8_t* row = raw.data() + y * (stride + 1) + 1;
        for (size_t i = 0; i < stride; ++i) {
            const uint8_t a = i >= static_cast<size_t>(channels) ? row[i - channels] : 0;
            const uint8_t b = prev[i];
            const uint8_t c = i >= static_cast<size_t>(channels) ? prev[i - channels] : 0;
            switch (filter) {
                case 0: break;
                case 1: row[i] = static_cast<uint8_t>(row[i] + a); break;
                case 2: row[i] = static_cast<uint8_t>(row[i] + b); break;
                case 3: row[i] = static_cast<uint8_t>(row[i] + ((a + b) >> 1)); break;
                case 4: row[i] = static_cast<uint8_t>(row[i] + paeth(a, b, c)); break;
                default:
                    err = "bad png filter";
                    return nullptr;
            }
        }
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* s = row + static_cast<size_t>(x) * channels;
            uint8_t* d = rgbaRow.data() + static_cast<size_t>(x) * 4u;
            switch (channels) {
                case 1: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
                case 2: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
                case 3: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
                default: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
            }
        }
        // 行 → 瓦片（与保存拼装对称：逐瓦片段拷贝）
        const uint32_t ty = y / kTileSize;
        const uint32_t inTileY = y % kTileSize;
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            const uint32_t x0 = tx * kTileSize;
            const uint32_t copyW = std::min(kTileSize, width - x0);
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            std::memcpy(buf.mapCpuWrite() + static_cast<size_t>(buf.rowBytes()) * inTileY,
                        rgbaRow.data() + static_cast<size_t>(x0) * 4u,
                        static_cast<size_t>(copyW) * 4u);
        }
        std::memcpy(prev.data(), row, stride);
    }
    return builder.publish(1);
}

}  // namespace io
}  // namespace montage
