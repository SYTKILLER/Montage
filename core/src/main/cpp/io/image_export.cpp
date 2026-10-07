#include "io/image_export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <unistd.h>
#include <zlib.h>

namespace montage {
namespace io {

namespace {

constexpr uint32_t kTile = kTileSize;

// 像素取数：网格局部坐标（网格覆盖 doc 的 origin+local）。返回 false = 网格外/空瓦片。
inline bool sampleGrid(const TileGrid& grid, double originX, double originY, int docX, int docY,
                       float out[4]) {
    if (grid.cols == 0 || grid.rows == 0) {
        return false;
    }
    const int lx = docX - static_cast<int>(std::lround(originX));
    const int ly = docY - static_cast<int>(std::lround(originY));
    if (lx < 0 || ly < 0) {
        return false;
    }
    const uint32_t ux = static_cast<uint32_t>(lx);
    const uint32_t uy = static_cast<uint32_t>(ly);
    if (ux >= grid.cols * kTile || uy >= grid.rows * kTile) {
        return false;
    }
    auto it = grid.tiles.find((uy / kTile) * grid.cols + (ux / kTile));
    if (it == grid.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
        return false;
    }
    const PixelSource* ps = it->second->pixels.get();
    const uint8_t* base = ps->mapCpu();
    if (base == nullptr) {
        return false;
    }
    const uint32_t rb = ps->rowBytes();
    const uint8_t* p = base + (uy % kTile) * rb + (ux % kTile) * 4;
    out[0] = p[0] / 255.0f;
    out[1] = p[1] / 255.0f;
    out[2] = p[2] / 255.0f;
    out[3] = p[3] / 255.0f;
    return true;
}

// ---------- 混合模式（gen_shaders.py BODIES 逐公式直译；Cs=源层，Cb=acc） ----------

inline float lum(const float c[3]) {
    return c[0] * 0.3f + c[1] * 0.59f + c[2] * 0.11f;
}

inline void clipColor(float c[3]) {
    const float l = lum(c);
    const float n = std::min(c[0], std::min(c[1], c[2]));
    const float x = std::max(c[0], std::max(c[1], c[2]));
    if (x > 1.0f) {
        for (int i = 0; i < 3; ++i) {
            c[i] = l + (c[i] - l) * ((1.0f - l) / std::max(1e-6f, x - l));
        }
    }
    if (n < 0.0f) {
        for (int i = 0; i < 3; ++i) {
            c[i] = l + (c[i] - l) * (l / std::max(1e-6f, l - n));
        }
    }
}

inline void setLum(float c[3], float l) {
    const float dl = l - lum(c);
    c[0] += dl;
    c[1] += dl;
    c[2] += dl;
    clipColor(c);
}

inline float sat(const float c[3]) {
    return std::max(c[0], std::max(c[1], c[2])) - std::min(c[0], std::min(c[1], c[2]));
}

inline void setSat(float c[3], float s) {
    float mn = std::min(c[0], std::min(c[1], c[2]));
    float mx = std::max(c[0], std::max(c[1], c[2]));
    float md = c[0] + c[1] + c[2] - mn - mx;
    float nv = mx > mn ? (md - mn) * s / (mx - mn) : 0.0f;
    if (c[0] == mx) {
        c[0] = s;
        c[1] = nv;
        c[2] = 0.0f;
    } else if (c[1] == mx) {
        c[0] = 0.0f;
        c[1] = s;
        c[2] = nv;
    } else {
        c[0] = nv;
        c[1] = 0.0f;
        c[2] = s;
    }
}

inline float stepHalf(float v) {
    return v >= 0.5f ? 1.0f : 0.0f;
}

inline float vclamp01(float v) {
    return std::min(1.0f, std::max(0.0f, v));
}

// mix(x, y, a) 按 GLSL 逐通道
inline float mix(float x, float y, float a) {
    return x + (y - x) * a;
}

// blendB 返回 B（写入 out[3]）
void blendBody(BlendMode mode, const float Cb[3], const float Cs[3], float out[3]) {
    switch (mode) {
        case BlendMode::Normal:
            out[0] = Cs[0];
            out[1] = Cs[1];
            out[2] = Cs[2];
            break;
        case BlendMode::Darken:
            for (int i = 0; i < 3; ++i) out[i] = std::min(Cb[i], Cs[i]);
            break;
        case BlendMode::Multiply:
            for (int i = 0; i < 3; ++i) out[i] = Cb[i] * Cs[i];
            break;
        case BlendMode::ColorBurn:
            for (int i = 0; i < 3; ++i)
                out[i] = std::max(1.0f - std::min(1.0f, (1.0f - Cb[i]) / std::max(1e-5f, Cs[i])), 0.0f);
            break;
        case BlendMode::LinearBurn:
            for (int i = 0; i < 3; ++i) out[i] = std::max(Cb[i] + Cs[i] - 1.0f, 0.0f);
            break;
        case BlendMode::Lighten:
            for (int i = 0; i < 3; ++i) out[i] = std::max(Cb[i], Cs[i]);
            break;
        case BlendMode::Screen:
            for (int i = 0; i < 3; ++i) out[i] = Cb[i] + Cs[i] - Cb[i] * Cs[i];
            break;
        case BlendMode::ColorDodge:
            for (int i = 0; i < 3; ++i)
                out[i] = std::max(std::min(1.0f, Cb[i] / std::max(1e-5f, 1.0f - Cs[i])), 0.0f);
            break;
        case BlendMode::LinearDodge:
            for (int i = 0; i < 3; ++i) out[i] = std::min(Cb[i] + Cs[i], 1.0f);
            break;
        case BlendMode::Overlay:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cb[i]);
                out[i] = mix(2.0f * Cb[i] * Cs[i], 1.0f - 2.0f * (1.0f - Cb[i]) * (1.0f - Cs[i]), a);
            }
            break;
        case BlendMode::SoftLight: {
            // GLSL: mix(Cb + (2Cs-1)*(mix((16Cb-12)Cb+4, sqrt(Cb), step(0.25, Cb)) - Cb),
            //           Cb - (1-2Cs)*Cb*(1-Cb), step(0.5, Cs))
            for (int i = 0; i < 3; ++i) {
                const float inner = mix((16.0f * Cb[i] - 12.0f) * Cb[i] + 4.0f,
                                        std::sqrt(std::max(0.0f, Cb[i])), stepHalf(Cb[i] - 0.25f));
                const float a = stepHalf(Cs[i]);
                out[i] = mix(Cb[i] + (2.0f * Cs[i] - 1.0f) * (inner - Cb[i]),
                             Cb[i] - (1.0f - 2.0f * Cs[i]) * Cb[i] * (1.0f - Cb[i]), a);
            }
            break;
        }
        case BlendMode::HardLight:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                out[i] = mix(2.0f * Cs[i] * Cb[i], 1.0f - 2.0f * (1.0f - Cs[i]) * (1.0f - Cb[i]), a);
            }
            break;
        case BlendMode::VividLight:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                const float t1 = 1.0f - std::min(1.0f, (1.0f - Cb[i]) / std::max(1e-5f, 2.0f * Cs[i]));
                const float t2 = std::min(1.0f, Cb[i] / std::max(1e-5f, 2.0f * (1.0f - Cs[i])));
                out[i] = std::max(mix(t1, t2, a), 0.0f);
            }
            break;
        case BlendMode::LinearLight:
            for (int i = 0; i < 3; ++i) out[i] = vclamp01(Cb[i] + 2.0f * Cs[i] - 1.0f);
            break;
        case BlendMode::PinLight:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                out[i] = mix(std::min(Cb[i], 2.0f * Cs[i]), std::max(Cb[i], 2.0f * Cs[i] - 1.0f), a);
            }
            break;
        case BlendMode::HardMix:
            for (int i = 0; i < 3; ++i) {
                const float a = stepHalf(Cs[i]);
                const float m = mix(std::min(Cb[i], 2.0f * Cs[i]),
                                    std::max(Cb[i], 2.0f * Cs[i] - 1.0f), a);
                out[i] = stepHalf(m - 0.5f);
            }
            break;
        case BlendMode::Difference:
            for (int i = 0; i < 3; ++i) out[i] = std::fabs(Cb[i] - Cs[i]);
            break;
        case BlendMode::Exclusion:
            for (int i = 0; i < 3; ++i) out[i] = Cb[i] + Cs[i] - 2.0f * Cb[i] * Cs[i];
            break;
        case BlendMode::Subtract:
            for (int i = 0; i < 3; ++i) out[i] = std::max(Cb[i] - Cs[i], 0.0f);
            break;
        case BlendMode::Divide:
            for (int i = 0; i < 3; ++i) out[i] = std::min(1.0f, Cs[i] / std::max(1e-5f, 1.0f - Cb[i]));
            break;
        case BlendMode::Hue: {
            float c[3] = {Cs[0], Cs[1], Cs[2]};
            setSat(c, sat(Cb));
            setLum(c, lum(Cb));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        case BlendMode::Saturation: {
            float c[3] = {Cb[0], Cb[1], Cb[2]};
            setSat(c, sat(Cs));
            setLum(c, lum(Cb));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        case BlendMode::Color: {
            float c[3] = {Cs[0], Cs[1], Cs[2]};
            setLum(c, lum(Cb));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        case BlendMode::Luminosity: {
            float c[3] = {Cb[0], Cb[1], Cb[2]};
            setLum(c, lum(Cs));
            out[0] = c[0];
            out[1] = c[1];
            out[2] = c[2];
            break;
        }
        default:
            out[0] = Cs[0];
            out[1] = Cs[1];
            out[2] = Cs[2];
            break;
    }
}

// 调整层 256 LUT（与 tile_renderer.cpp buildAdjustmentLut 逐公式一致）
void buildLut(const LayerAdjustment& adj, float lut[256]) {
    if (adj.kind == AdjustmentKind::Levels) {
        const float lo = std::min(adj.inBlack, adj.inWhite - 0.001f);
        const float hi = std::max(adj.inWhite, lo + 0.001f);
        const float g = std::min(10.0f, std::max(0.1f, adj.gamma));
        for (int i = 0; i < 256; ++i) {
            float t = (static_cast<float>(i) / 255.0f - lo) / (hi - lo);
            t = vclamp01(t);
            lut[i] = std::pow(t, 1.0f / g);
        }
        return;
    }
    const auto& xs = adj.curveX;
    const auto& ys = adj.curveY;
    if (xs.size() < 2 || xs.size() != ys.size()) {
        for (int i = 0; i < 256; ++i) {
            lut[i] = static_cast<float>(i) / 255.0f;
        }
        return;
    }
    for (int i = 0; i < 256; ++i) {
        const float x = static_cast<float>(i) / 255.0f;
        if (x <= xs.front()) {
            lut[i] = ys.front();
            continue;
        }
        if (x >= xs.back()) {
            lut[i] = ys.back();
            continue;
        }
        for (size_t k = 0; k + 1 < xs.size(); ++k) {
            if (x >= xs[k] && x <= xs[k + 1]) {
                const float t = (x - xs[k]) / std::max(1e-6f, xs[k + 1] - xs[k]);
                lut[i] = ys[k] + (ys[k + 1] - ys[k]) * t;
                break;
            }
        }
    }
}

// ---------- PNG 直写（sig + IHDR + 多 IDAT + IEND；字节序/大端 CRC 纪律见 NOTICE M4b） ----------

void putU32BE(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

uint32_t crc32b(const uint8_t* data, size_t n) {
    uLong c = crc32(0L, Z_NULL, 0);
    c = crc32(c, data, static_cast<uInt>(n));
    return static_cast<uint32_t>(c);
}

bool writeAll(int fd, const uint8_t* p, size_t n) {
    size_t off = 0;
    while (off < n) {
        const ssize_t w = ::write(fd, p + off, n - off);
        if (w <= 0) {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
}

bool writeChunk(int fd, const char* type, const uint8_t* data, size_t n) {
    std::vector<uint8_t> buf(12 + n);
    putU32BE(buf.data(), static_cast<uint32_t>(n));
    std::memcpy(buf.data() + 4, type, 4);
    if (n > 0) {
        std::memcpy(buf.data() + 8, data, n);
    }
    const uint32_t crc = crc32b(buf.data() + 4, 4 + n);
    putU32BE(buf.data() + 8 + n, crc);
    return writeAll(fd, buf.data(), buf.size());
}

}  // namespace

FlattenJob BuildFlattenJob(const Document& doc, int regionX, int regionY, int regionW, int regionH) {
    FlattenJob job;
    job.docWidth = doc.width;
    job.docHeight = doc.height;
    (void)regionX;
    (void)regionY;
    (void)regionW;
    (void)regionH;  // region 在 FlattenVisible 阶段生效
    const size_t n = doc.layers.size();
    for (size_t i = 0; i < n; ++i) {
        const Layer& l = doc.layers[i];
        if (!l.visible) {
            continue;
        }
        FlatLayer fl;
        fl.blend = l.blendMode;
        fl.opacity = l.opacity;
        fl.clipping = l.clipping;
        fl.visible = true;
        fl.isAdjustment = l.adjustment != nullptr;
        fl.originX = l.transform.originX;
        fl.originY = l.transform.originY;
        if (fl.isAdjustment) {
            fl.adjustment = *l.adjustment;
        } else {
            fl.pixels = l.effectivePixels();
            if (fl.pixels == nullptr) {
                continue;  // 空图层跳过
            }
            if (fl.clipping) {
                // 剪贴基 = 下方最近可见非剪贴层（无有效基 → 剪贴组隐藏，PS 语义）
                for (size_t j = i; j-- > 0;) {
                    const Layer& b = doc.layers[j];
                    if (!b.visible || b.clipping) {
                        continue;
                    }
                    if (b.adjustment != nullptr) {
                        continue;  // v1：调整层不可作剪贴基
                    }
                    fl.clipBasePixels = b.effectivePixels();
                    fl.clipBaseOriginX = b.transform.originX;
                    fl.clipBaseOriginY = b.transform.originY;
                    break;
                }
            }
        }
        job.layers.push_back(std::move(fl));
    }
    return job;
}

FlattenResult FlattenVisible(const FlattenJob& job, int regionX, int regionY, int regionW, int regionH) {
    FlattenResult res;
    int rx = regionX;
    int ry = regionY;
    int rw = regionW;
    int rh = regionH;
    if (rw <= 0 || rh <= 0) {
        rx = 0;
        ry = 0;
        rw = static_cast<int>(job.docWidth);
        rh = static_cast<int>(job.docHeight);
    }
    const int x0 = std::max(0, rx);
    const int y0 = std::max(0, ry);
    const int x1 = std::min(static_cast<int>(job.docWidth), rx + rw);
    const int y1 = std::min(static_cast<int>(job.docHeight), ry + rh);
    if (x1 <= x0 || y1 <= y0) {
        res.error = "empty region";
        return res;  // ok=false
    }
    res.ok = true;
    res.width = static_cast<uint32_t>(x1 - x0);
    res.height = static_cast<uint32_t>(y1 - y0);
    res.rgba.assign(static_cast<size_t>(res.width) * res.height * 4, 0);

    const int w = static_cast<int>(res.width);
    float lut[256];
    for (const FlatLayer& fl : job.layers) {
        if (fl.isAdjustment) {
            // 调整层：全帧 LUT（kAdjustFrag 同构；opacity lerp，alpha 不变）
            buildLut(fl.adjustment, lut);
            const float op = static_cast<float>(fl.opacity);
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = res.rgba.data() + (static_cast<size_t>(y - y0) * w) * 4;
                for (int x = x0; x < x1; ++x) {
                    uint8_t* p = row + (x - x0) * 4;
                    if (p[3] == 0) {
                        continue;
                    }
                    for (int c = 0; c < 3; ++c) {
                        const int idx = p[c];
                        const float adjv = lut[idx];
                        p[c] = static_cast<uint8_t>(std::lround(
                            (p[c] / 255.0f + (adjv - p[c] / 255.0f) * op) * 255.0f));
                    }
                }
            }
            continue;
        }
        const bool clipped = fl.clipping;
        const bool noBase = clipped && fl.clipBasePixels == nullptr;
        const TileGrid* grid = fl.pixels.get();
        const TileGrid* baseGrid = clipped ? fl.clipBasePixels.get() : nullptr;
        const float op = static_cast<float>(fl.opacity);
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = res.rgba.data() + (static_cast<size_t>(y - y0) * w) * 4;
            for (int x = x0; x < x1; ++x) {
                uint8_t* p = row + (x - x0) * 4;
                float src[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                if (noBase || !sampleGrid(*grid, fl.originX, fl.originY, x, y, src)) {
                    continue;
                }
                float dst[4] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
                if (clipped) {
                    float baseAlpha = 0.0f;
                    float tmp[4];
                    if (sampleGrid(*baseGrid, fl.clipBaseOriginX, fl.clipBaseOriginY, x, y, tmp)) {
                        baseAlpha = tmp[3];
                    }
                    src[3] *= baseAlpha;
                    if (src[3] <= 0.0f) {
                        continue;
                    }
                }
                const float as = src[3] * op;
                const float ab = dst[3];
                float B[3];
                blendBody(fl.blend, dst, src, B);
                float Cr[3];
                for (int c = 0; c < 3; ++c) {
                    Cr[c] = (1.0f - ab) * src[c] + ab * B[c];
                }
                const float ao = as + ab * (1.0f - as);
                if (ao > 0.0001f) {
                    for (int c = 0; c < 3; ++c) {
                        const float coP = as * Cr[c] + (1.0f - as) * dst[c] * ab;
                        p[c] = static_cast<uint8_t>(std::lround(coP / ao * 255.0f));
                    }
                    p[3] = static_cast<uint8_t>(std::lround(ao * 255.0f));
                } else {
                    p[0] = p[1] = p[2] = p[3] = 0;
                }
            }
        }
    }
    return res;
}

bool WritePngToFd(int fd, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                  uint64_t* bytesOut, std::string* err) {
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!writeAll(fd, kSig, 8)) {
        if (err != nullptr) *err = "png: write sig failed";
        return false;
    }
    uint8_t ihdr[13];
    putU32BE(ihdr + 0, width);
    putU32BE(ihdr + 4, height);
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // RGBA
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    if (!writeChunk(fd, "IHDR", ihdr, sizeof(ihdr))) {
        if (err != nullptr) *err = "png: write IHDR failed";
        return false;
    }
    z_stream zs{};
    if (deflateInit(&zs, Z_DEFAULT_COMPRESSION) != Z_OK) {
        if (err != nullptr) *err = "png: deflateInit failed";
        return false;
    }
    uint64_t total = 8 + (12 + 13) + 12;
    std::vector<uint8_t> out(64 * 1024);
    std::vector<uint8_t> row(1 + static_cast<size_t>(width) * 4);
    bool fail = false;
    auto pump = [&](int flush) -> bool {
        zs.next_out = out.data();
        zs.avail_out = static_cast<uInt>(out.size());
        while (true) {
            const int rc = deflate(&zs, flush);
            if (rc == Z_STREAM_ERROR) {
                return false;
            }
            const size_t have = out.size() - zs.avail_out;
            if (have > 0) {
                if (!writeAll(fd, out.data(), have)) {
                    return false;
                }
                total += have;
                zs.next_out = out.data();
                zs.avail_out = static_cast<uInt>(out.size());
            }
            if (rc == Z_STREAM_END) {
                return true;
            }
            if (zs.avail_out > 0) {
                return true;  // 待压缩输入/输出已耗尽
            }
        }
    };
    for (uint32_t y = 0; y < height && !fail; ++y) {
        row[0] = 0;  // filter 0
        std::memcpy(row.data() + 1, rgba.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<size_t>(width) * 4);
        zs.next_in = row.data();
        zs.avail_in = static_cast<uInt>(row.size());
        if (!pump(Z_NO_FLUSH)) {
            fail = true;
        }
    }
    if (!fail) {
        zs.next_in = nullptr;
        zs.avail_in = 0;
        fail = !pump(Z_FINISH);
    }
    deflateEnd(&zs);
    if (fail) {
        if (err != nullptr) *err = "png: deflate/write failed";
        return false;
    }
    if (!writeChunk(fd, "IEND", nullptr, 0)) {
        if (err != nullptr) *err = "png: write IEND failed";
        return false;
    }
    total += 12;
    if (bytesOut != nullptr) *bytesOut = total;
    return true;
}

}  // namespace io
}  // namespace montage
