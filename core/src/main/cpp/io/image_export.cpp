#include "io/image_export.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <unistd.h>
#include <zlib.h>

#include "engine/filter_ops.h"
#include "engine/blend_cpu.h"

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
// M21 起公式唯一来源 = engine/blend_cpu.h（flatten 与智能滤镜共用；下方仅再导出）

using blend_cpu::blendBody;
using blend_cpu::lum;
using blend_cpu::clipColor;
using blend_cpu::setLum;
using blend_cpu::sat;
using blend_cpu::setSat;
using blend_cpu::vclamp01;

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
    (void)regionX;
    (void)regionY;
    (void)regionW;
    (void)regionH;  // region 在 FlattenVisible 阶段生效
    return BuildFlattenJobRange(doc, 0, static_cast<int>(doc.layers.size()));
}

// M21：单层 job（堆栈模式帧；effectivePixels 快照 + 直通 Normal——调用方保证可见/有像素）
FlattenJob BuildFlattenJobSingle(const Layer& layer, int regionX, int regionY, int regionW,
                                 int regionH) {
    (void)regionX;
    (void)regionY;
    (void)regionW;
    (void)regionH;
    FlattenJob job;
    FlatLayer fl;
    fl.blend = BlendMode::Normal;
    fl.opacity = 1.0;
    fl.clipping = false;
    fl.visible = true;
    fl.isAdjustment = false;
    fl.pixels = layer.effectivePixels();
    fl.originX = layer.effectiveOriginX();
    fl.originY = layer.effectiveOriginY();
    if (fl.pixels != nullptr) {
        job.layers.push_back(std::move(fl));
    }
    return job;
}

// 层段内容包围盒并集（组递归计可见子层；全段无可见像素返回 false）
bool rangeContentBBox(const Document& doc, int start, int end, int& bx, int& by, int& bw, int& bh) {
    bool any = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const auto grow = [&](int lx, int ly, int lw, int lh) -> void {
        const int rx0 = std::max(0, lx);
        const int ry0 = std::max(0, ly);
        const int rx1 = std::min(static_cast<int>(doc.width), lx + lw);
        const int ry1 = std::min(static_cast<int>(doc.height), ly + lh);
        if (rx1 <= rx0 || ry1 <= ry0) {
            return;
        }
        if (!any) {
            x0 = rx0;
            y0 = ry0;
            x1 = rx1;
            y1 = ry1;
            any = true;
            return;
        }
        x0 = std::min(x0, rx0);
        y0 = std::min(y0, ry0);
        x1 = std::max(x1, rx1);
        y1 = std::max(y1, ry1);
    };
    for (int i = start; i < end; ++i) {
        const Layer& l = doc.layers[static_cast<size_t>(i)];
        if (l.isGroup) {
            if (!l.visible) {
                continue;  // 隐藏组整棵子树跳过（渲染同语义）
            }
            const int ce = groupChildEnd(doc.layers, i);
            int gx = 0, gy = 0, gw = 0, gh = 0;
            if (rangeContentBBox(doc, i + 1, ce, gx, gy, gw, gh)) {
                grow(gx, gy, gw, gh);
            }
            continue;
        }
        if (!l.visible || l.adjustment != nullptr) {
            continue;
        }
        const std::shared_ptr<const TileGrid> px = l.effectivePixels();
        if (px == nullptr) {
            continue;
        }
        uint32_t ux = 0, uy = 0, uw = 0, uh = 0;
        if (!layerContentBBox(*px, ux, uy, uw, uh)) {
            continue;
        }
        grow(static_cast<int>(std::lround(l.transform.originX)) + static_cast<int>(ux),
             static_cast<int>(std::lround(l.transform.originY)) + static_cast<int>(uy),
             static_cast<int>(uw), static_cast<int>(uh));
    }
    if (!any) {
        return false;
    }
    bx = x0;
    by = y0;
    bw = x1 - x0;
    bh = y1 - y0;
    return true;
}

namespace {

// 组子纹理发布序号（进程内单调；组纹理不入文档，只求唯一缓存键）
std::atomic<uint64_t> g_groupTexRev{1};

// 递归追加层段 [start, end)（bottom→top）。组节点：折叠 → 整段跳过；
// 穿透（Normal + opacity≥1）→ 子层直入主栈（PS 默认语义，与 GL composeLayerRange 同构）；
// 其余组 → 子段先合成组子纹理（子段内容 bbox 域），再以组 blend/opacity 作为一层混入。
void appendLayerRange(const Document& doc, int start, int end, FlattenJob& job, int depth) {
    if (depth > 16) {
        return;  // 嵌套上限防御（GL 渲染同款深度 8；此处放更宽）
    }
    for (int i = start; i < end; ++i) {
        const Layer& l = doc.layers[static_cast<size_t>(i)];
        if (l.isGroup) {
            if (!l.visible) {
                i = groupChildEnd(doc.layers, i) - 1;  // 隐藏组整棵子树跳过
                continue;
            }
            const int ce = groupChildEnd(doc.layers, i);
            if (l.collapsed || ce <= i + 1) {
                i = ce - 1;  // 折叠/空组：整段跳过
                continue;
            }
            const bool passThrough = l.blendMode == BlendMode::Normal && l.opacity >= 1.0;
            if (passThrough) {
                appendLayerRange(doc, i + 1, ce, job, depth + 1);
            } else {
                int gx = 0, gy = 0, gw = 0, gh = 0;
                if (!rangeContentBBox(doc, i + 1, ce, gx, gy, gw, gh)) {
                    i = ce - 1;
                    continue;
                }
                FlattenJob sub;
                sub.docWidth = doc.width;
                sub.docHeight = doc.height;
                appendLayerRange(doc, i + 1, ce, sub, depth + 1);
                FlattenResult res = FlattenVisible(sub, gx, gy, gw, gh);
                if (!res.ok) {
                    i = ce - 1;
                    continue;
                }
                FlatLayer tex;
                tex.blend = l.blendMode;
                tex.opacity = l.opacity;
                tex.visible = true;
                tex.pixels = filterStripToGrid(res.rgba, res.width, res.height,
                                               g_groupTexRev.fetch_add(1));
                tex.originX = gx;
                tex.originY = gy;
                job.layers.push_back(std::move(tex));
            }
            i = ce - 1;  // 子树已整体消费（穿透递归/组纹理），外层循环不可再访
            continue;
        }
        if (!l.visible) {
            continue;
        }
        FlatLayer fl;
        fl.blend = l.blendMode;
        fl.opacity = l.opacity;
        fl.clipping = l.clipping;
        fl.visible = true;
        fl.isAdjustment = l.adjustment != nullptr;
        fl.originX = l.effectiveOriginX();
        fl.originY = l.effectiveOriginY();
        if (fl.isAdjustment) {
            fl.adjustment = *l.adjustment;
        } else {
            fl.pixels = l.effectivePixels();
            if (fl.pixels == nullptr) {
                continue;  // 空图层跳过
            }
            if (fl.clipping) {
                // 剪贴基 = 段内下方最近可见非剪贴层（剪贴不跨组/段边界，PS 语义；
                // 无有效基 → 剪贴组隐藏）。调整层不可作剪贴基（v1）。
                for (int j = i; j-- > start;) {
                    const Layer& b = doc.layers[static_cast<size_t>(j)];
                    if (b.isGroup || !b.visible || b.clipping || b.adjustment != nullptr) {
                        continue;
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
}

}  // namespace

FlattenJob BuildFlattenJobRange(const Document& doc, int start, int end) {
    FlattenJob job;
    job.docWidth = doc.width;
    job.docHeight = doc.height;
    appendLayerRange(doc, start, end, job, 0);
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
    // 拼合语义（opaqueWhite）：透明区填白（PS 拼合图像），其余场合从透明起合成
    res.rgba.assign(static_cast<size_t>(res.width) * res.height * 4,
                    job.opaqueWhite ? 255 : 0);
    if (job.opaqueWhite) {
        for (size_t k = 3; k < res.rgba.size(); k += 4) {
            res.rgba[k] = 255;
        }
    }

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
    std::vector<uint8_t> idat;  // 压缩流缓冲后经 writeChunk 写（IDAT 块头 + CRC）
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
                idat.insert(idat.end(), out.data(), out.data() + have);
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
        if (err != nullptr) *err = "png: deflate failed";
        return false;
    }
    // M24 批处理对拍抓出：M7.5 起缺 IDAT 块头与 CRC，导出 PNG 恒为非法文件
    if (!writeChunk(fd, "IDAT", idat.data(), idat.size())) {
        if (err != nullptr) *err = "png: write IDAT failed";
        return false;
    }
    total += 12 + idat.size();
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
