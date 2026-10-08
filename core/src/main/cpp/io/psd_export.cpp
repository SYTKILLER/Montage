#include "io/psd_export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

#include <unistd.h>

#include "io/image_export.h"

namespace montage {
namespace io {

namespace {

constexpr uint32_t kTile = kTileSize;

// ---------- 大端写助手（PSD 全程大端） ----------

class BeWriter {
  public:
    explicit BeWriter(int fd) : fd_(fd) {}

    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v >> 8));
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void u32(uint32_t v) {
        buf_.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void bytes(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
    void ascii(const char* s, size_t n) { buf_.insert(buf_.end(), s, s + n); }
    void zeros(size_t n) { buf_.insert(buf_.end(), n, 0); }
    void raw(const std::vector<uint8_t>& v) { buf_.insert(buf_.end(), v.begin(), v.end()); }
    void pad2In(size_t sectionStart) {
        if ((buf_.size() - sectionStart) % 2 != 0) {
            buf_.push_back(0);
        }
    }
    size_t size() const { return buf_.size(); }
    void patchU32(size_t at, uint32_t v) {
        buf_[at + 0] = static_cast<uint8_t>((v >> 24) & 0xFF);
        buf_[at + 1] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf_[at + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf_[at + 3] = static_cast<uint8_t>(v & 0xFF);
    }

    bool flushTo(int fd, std::string* err) const {
        size_t off = 0;
        while (off < buf_.size()) {
            const size_t chunk = std::min<size_t>(buf_.size() - off, 256 * 1024);
            if (!writeAll(fd, buf_.data() + off, chunk)) {
                if (err != nullptr) {
                    *err = "psd: write failed";
                }
                return false;
            }
            off += chunk;
        }
        return true;
    }

  private:
    static bool writeAll(int fd, const uint8_t* p, size_t n) {
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
    int fd_;
    std::vector<uint8_t> buf_;
};

// ---------- PackBits 行编码（Apple 标准，PSD 逐行独立） ----------

std::vector<uint8_t> packBitsRow(const uint8_t* row, size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n + n / 8 + 16);
    size_t i = 0;
    std::vector<uint8_t> lit;
    auto flushLiteral = [&]() {
        while (!lit.empty()) {
            const size_t take = std::min<size_t>(lit.size(), 128);
            out.push_back(static_cast<uint8_t>(take - 1));  // 0..127 = 后随 take+1 字节
            out.insert(out.end(), lit.begin(), lit.begin() + static_cast<long>(take));
            lit.erase(lit.begin(), lit.begin() + static_cast<long>(take));
        }
    };
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && row[i + run] == row[i]) {
            ++run;
        }
        if (run >= 3) {
            flushLiteral();
            out.push_back(static_cast<uint8_t>(257 - run));  // 129..255 = 重复 (257-n)
            out.push_back(row[i]);
            i += run;
        } else {
            for (size_t k = 0; k < run; ++k) {
                lit.push_back(row[i + k]);
            }
            i += run;
            if (lit.size() >= 128) {
                flushLiteral();
            }
        }
    }
    flushLiteral();
    return out;
}

// ---------- 网格行读取（与 engine/transform_ops.cpp gridReadRow 同构，局部副本） ----------

void gridReadRow(const TileGrid& g, uint32_t rowY, uint8_t* dst) {
    const uint32_t w = g.cols * kTile;
    std::memset(dst, 0, static_cast<size_t>(w) * 4u);
    if (rowY >= g.rows * kTile) {
        return;
    }
    const uint32_t ty = rowY / kTile;
    const uint32_t inY = rowY % kTile;
    for (uint32_t tx = 0; tx < g.cols; ++tx) {
        auto it = g.tiles.find(ty * g.cols + tx);
        if (it == g.tiles.end() || it->second == nullptr || it->second->pixels == nullptr) {
            continue;
        }
        const uint8_t* src = it->second->pixels->mapCpu();
        if (src == nullptr) {
            continue;
        }
        const uint32_t rb = it->second->pixels->rowBytes();
        std::memcpy(dst + static_cast<size_t>(tx) * kTile * 4u,
                    src + static_cast<size_t>(inY) * rb, kTile * 4u);
    }
}

// 内容包围盒（图层局部；非空瓦片并集）
bool gridBBox(const TileGrid& g, uint32_t& px, uint32_t& py, uint32_t& pw, uint32_t& ph) {
    uint32_t tx0 = UINT32_MAX;
    uint32_t ty0 = UINT32_MAX;
    uint32_t tx1 = 0;
    uint32_t ty1 = 0;
    for (const auto& [idx, tile] : g.tiles) {
        (void)tile;
        tx0 = std::min(tx0, idx % g.cols);
        ty0 = std::min(ty0, idx / g.cols);
        tx1 = std::max(tx1, idx % g.cols);
        ty1 = std::max(ty1, idx / g.cols);
    }
    if (tx0 == UINT32_MAX) {
        return false;
    }
    px = tx0 * kTile;
    py = ty0 * kTile;
    pw = (tx1 - tx0 + 1u) * kTile;
    ph = (ty1 - ty0 + 1u) * kTile;
    return true;
}

// ---------- 混合键反查（psd_reader blendFromKey 25 键逐项互逆） ----------

const char* blendToKey(BlendMode m) {
    switch (m) {
        case BlendMode::Normal: return "norm";
        case BlendMode::Multiply: return "mul ";
        case BlendMode::Screen: return "scrn";
        case BlendMode::Overlay: return "over";
        case BlendMode::Darken: return "dark";
        case BlendMode::Lighten: return "lite";
        case BlendMode::Difference: return "diff";
        case BlendMode::ColorDodge: return "div ";
        case BlendMode::ColorBurn: return "idiv";
        case BlendMode::Hue: return "hue ";
        case BlendMode::Saturation: return "sat ";
        case BlendMode::Color: return "colr";
        case BlendMode::Luminosity: return "lum ";
        case BlendMode::LinearBurn: return "lbrn";
        case BlendMode::LinearDodge: return "lddg";
        case BlendMode::HardLight: return "hLit";
        case BlendMode::VividLight: return "vLit";
        case BlendMode::LinearLight: return "lLit";
        case BlendMode::PinLight: return "pLit";
        case BlendMode::HardMix: return "hMix";
        case BlendMode::SoftLight: return "sLit";
        case BlendMode::Exclusion: return "smud";
        case BlendMode::Subtract: return "fsub";
        case BlendMode::Divide: return "fdiv";
        default: return "norm";
    }
}

// ---------- 编码产物 ----------

struct Plane {
    std::vector<uint16_t> counts;   // 每行压缩长
    std::vector<uint8_t> payload;   // 全部行压缩数据
    uint32_t totalLen() const {     // 含 compression 头
        return 2u + static_cast<uint32_t>(counts.size()) * 2u +
               static_cast<uint32_t>(payload.size());
    }
    void writeTo(BeWriter& w) const {
        w.u16(1);  // RLE
        for (uint16_t c : counts) {
            w.u16(c);
        }
        w.raw(payload);
    }
};

// 从网格 bbox 提取单通道平面并 RLE（channel: 0=R 1=G 2=B 3=a；网格为 RGBA 直通）
Plane encodePlaneFromGrid(const TileGrid& grid, uint32_t bx, uint32_t by, uint32_t bw,
                          uint32_t bh, int channel) {
    Plane p;
    p.counts.resize(bh);
    std::vector<uint8_t> fullRow(grid.cols * kTile * 4);
    std::vector<uint8_t> ch(bw);
    int nonzero = 0;
    for (uint32_t r = 0; r < bh; ++r) {
        gridReadRow(grid, by + r, fullRow.data());
        for (uint32_t x = 0; x < bw; ++x) {
            ch[x] = fullRow[static_cast<size_t>(bx + x) * 4u + static_cast<size_t>(channel)];
            if (ch[x] != 0) ++nonzero;
        }
        std::vector<uint8_t> packed = packBitsRow(ch.data(), bw);
        p.counts[r] = static_cast<uint16_t>(packed.size());
        p.payload.insert(p.payload.end(), packed.begin(), packed.end());
    }
    return p;
}

// 蒙版 patch 平面（patch 网格 R 通道 = 灰度；宽 maskW）
Plane encodePlaneFromMask(const TileGrid& patch, uint32_t mw, uint32_t mh) {
    Plane p;
    p.counts.resize(mh);
    std::vector<uint8_t> fullRow(patch.cols * kTile * 4);
    for (uint32_t r = 0; r < mh; ++r) {
        gridReadRow(patch, r, fullRow.data());
        std::vector<uint8_t> ch(mw);
        for (uint32_t x = 0; x < mw; ++x) {
            ch[x] = fullRow[x * 4];  // 灰度在 R
        }
        std::vector<uint8_t> packed = packBitsRow(ch.data(), mw);
        p.counts[r] = static_cast<uint16_t>(packed.size());
        p.payload.insert(p.payload.end(), packed.begin(), packed.end());
    }
    return p;
}

struct EncodedLayer {
    std::string name;
    int top = 0, left = 0, bottom = 0, right = 0;
    const char* key = "norm";
    uint8_t opacity = 255;
    uint8_t clipping = 0;
    uint8_t flags = 0;  // bit1 = hidden
    Plane red, green, blue, alpha;
    bool hasMask = false;
    int mTop = 0, mLeft = 0, mBottom = 0, mRight = 0;
    uint8_t mDefault = 255;
    uint8_t mFlags = 0;
    Plane maskPlane;
};

}  // namespace

PsdExportResult exportPsd(int fd, const PsdDocSnap& snap) {
    PsdExportResult res;
    if (snap.width == 0 || snap.height == 0) {
        res.error = "psd: empty document";
        close(fd);
        return res;
    }

    // ---- Pass 1：层编码（bottom→top 遍历；写出时逆序 top-first） ----
    std::vector<EncodedLayer> layers;
    res.notes.clear();
    for (const PsdLayerSnap& l : snap.layers) {
        if (l.isAdjustment) {
            res.notes.push_back("调整层 \"" + l.name + "\" 不支持 PSD 导出，已跳过。");
            continue;
        }
        if (l.pixels == nullptr || l.pixels->tiles.empty()) {
            continue;
        }
        uint32_t px = 0, py = 0, pw = 0, ph = 0;
        if (!gridBBox(*l.pixels, px, py, pw, ph)) {
            continue;
        }
        EncodedLayer el;
        el.name = l.name;
        el.left = static_cast<int>(l.originX) + static_cast<int>(px);
        el.top = static_cast<int>(l.originY) + static_cast<int>(py);
        el.right = el.left + static_cast<int>(pw);
        el.bottom = el.top + static_cast<int>(ph);
        el.key = blendToKey(l.blendMode);
        el.opacity = static_cast<uint8_t>(
            std::lround(std::min(1.0, std::max(0.0, l.opacity)) * 255.0));
        el.clipping = l.clipping ? 1 : 0;
        el.flags = l.visible ? 0 : 2;
        el.red = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 0);
        el.green = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 1);
        el.blue = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 2);
        el.alpha = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 3);
        if (l.maskGrid != nullptr && l.maskW > 0 && l.maskH > 0) {
            el.hasMask = true;
            el.mLeft = static_cast<int>(l.originX) + l.maskOffsetX;
            el.mTop = static_cast<int>(l.originY) + l.maskOffsetY;
            el.mRight = el.mLeft + static_cast<int>(l.maskW);
            el.mBottom = el.mTop + static_cast<int>(l.maskH);
            el.mDefault = static_cast<uint8_t>(l.maskOutside);
            el.mFlags = l.maskEnabled ? 0 : 2;
            el.maskPlane = encodePlaneFromMask(*l.maskGrid, l.maskW, l.maskH);
        }
        layers.push_back(std::move(el));
    }
    res.layers = static_cast<uint32_t>(layers.size());

    // ---- 合成图（ImageData；快照未携带时现场扁平化） ----
    std::vector<uint8_t> flat;
    if (snap.flattenedRgba != nullptr) {
        flat = *snap.flattenedRgba;
    } else {
        FlattenJob job;
        job.docWidth = snap.width;
        job.docHeight = snap.height;
        for (const PsdLayerSnap& l : snap.layers) {
            if (!l.visible || l.isAdjustment || l.pixels == nullptr) {
                continue;
            }
            FlatLayer fl;
            fl.blend = l.blendMode;
            fl.opacity = l.opacity;
            fl.clipping = l.clipping;
            fl.visible = true;
            fl.pixels = l.pixels;  // bridge 拍快照时取 effectivePixels（蒙版已合成）
            fl.originX = l.originX;
            fl.originY = l.originY;
            if (fl.clipping) {
                for (size_t j = job.layers.size(); j-- > 0;) {
                    if (job.layers[j].clipping) {
                        continue;
                    }
                    fl.clipBasePixels = job.layers[j].pixels;
                    fl.clipBaseOriginX = job.layers[j].originX;
                    fl.clipBaseOriginY = job.layers[j].originY;
                    break;
                }
            }
            job.layers.push_back(std::move(fl));
        }
        FlattenResult fr = FlattenVisible(job, 0, 0, 0, 0);
        if (!fr.ok || fr.rgba.size() < static_cast<size_t>(snap.width) * snap.height * 4) {
            fr = FlattenResult{};
            fr.ok = true;
            fr.width = snap.width;
            fr.height = snap.height;
            fr.rgba.assign(static_cast<size_t>(snap.width) * snap.height * 4, 0);
        }
        flat = std::move(fr.rgba);
    }
    // 合成图三平面 RLE（planar RGB，直通——PS ImageData 段无 alpha 语义混淆，a 舍弃）
    Plane flatR;
    Plane flatG;
    Plane flatB;
    {
        const size_t n = static_cast<size_t>(snap.width) * snap.height;
        std::vector<uint8_t> plane(n);
        for (int c = 0; c < 3; ++c) {
            for (size_t i = 0; i < n; ++i) {
                plane[i] = flat[i * 4 + static_cast<size_t>(c)];
            }
            Plane p;
            p.counts.resize(snap.height);
            for (uint32_t r = 0; r < snap.height; ++r) {
                std::vector<uint8_t> packed = packBitsRow(
                    plane.data() + static_cast<size_t>(r) * snap.width, snap.width);
                p.counts[r] = static_cast<uint16_t>(packed.size());
                p.payload.insert(p.payload.end(), packed.begin(), packed.end());
            }
            if (c == 0) {
                flatR = std::move(p);
            } else if (c == 1) {
                flatG = std::move(p);
            } else {
                flatB = std::move(p);
            }
        }
    }

    // ---- Pass 2/3：写文件 ----
    BeWriter w(fd);
    // Header
    w.ascii("8BPS", 4);
    w.u16(1);
    w.zeros(6);
    w.u16(4);
    w.u32(snap.height);
    w.u32(snap.width);
    w.u16(8);
    w.u16(3);
    // Color Mode Data / Image Resources
    w.u32(0);
    w.u32(0);

    // Layer & Mask Info（占位回填）
    const size_t lmLenAt = w.size();
    w.u32(0);
    const size_t liLenAt = w.size();
    w.u32(0);
    const size_t countAt = w.size();
    w.i16(static_cast<int16_t>(layers.size()));  // 正数 = 透明度不含合并图

    // 层记录（top-first）
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const EncodedLayer& el = *it;
        w.i32(el.top);
        w.i32(el.left);
        w.i32(el.bottom);
        w.i32(el.right);
        const int chanCount = el.hasMask ? 5 : 4;
        w.u16(static_cast<uint16_t>(chanCount));
        w.i16(0);
        w.u32(el.red.totalLen());
        w.i16(1);
        w.u32(el.green.totalLen());
        w.i16(2);
        w.u32(el.blue.totalLen());
        w.i16(-1);
        w.u32(el.alpha.totalLen());
        if (el.hasMask) {
            w.i16(-2);
            w.u32(el.maskPlane.totalLen());
        }
        w.ascii("8BIM", 4);
        w.ascii(el.key, 4);
        w.u8(el.opacity);
        w.u8(el.clipping);
        w.u8(el.flags);
        w.u8(0);
        // extra data
        const size_t extraAt = w.size();
        w.u32(0);
        // mask section
        if (el.hasMask) {
            const size_t mAt = w.size();
            w.u32(0);
            w.i32(el.mTop);
            w.i32(el.mLeft);
            w.i32(el.mBottom);
            w.i32(el.mRight);
            w.u8(el.mDefault);
            w.u8(el.mFlags);
            w.u16(0);  // pad（基础 mask section = 20 字节）
            w.patchU32(mAt, 20);
        } else {
            w.u32(0);
        }
        // blending ranges（空）
        w.u32(0);
        // pascal name（ASCII 兜底；UTF-8 全名走 luni）
        std::string asciiName;
        for (const char ch : el.name) {
            const unsigned char u = static_cast<unsigned char>(ch);
            asciiName.push_back(u >= 0x20 && u < 0x7F ? ch : '?');
        }
        if (asciiName.empty()) {
            asciiName = "Layer";
        }
        if (asciiName.size() > 60) {
            asciiName.resize(60);
        }
        w.u8(static_cast<uint8_t>(asciiName.size()));
        w.ascii(asciiName.data(), asciiName.size());
        const size_t nameWritten = 1 + asciiName.size();
        for (size_t pad = (4 - nameWritten % 4) % 4; pad > 0; --pad) {
            w.u8(0);
        }
        // luni（Unicode 名 UTF-16BE；reader 对拍 unicodeName 同构，中文名保真）
        {
            std::vector<uint16_t> units;
            for (size_t i = 0; i < el.name.size();) {
                const unsigned char b0 = static_cast<unsigned char>(el.name[i]);
                if (b0 < 0x80) {
                    units.push_back(b0);
                    i += 1;
                } else if ((b0 & 0xE0) == 0xC0 && i + 1 < el.name.size()) {
                    units.push_back(static_cast<uint16_t>((b0 & 0x1F) << 6 |
                        (static_cast<unsigned char>(el.name[i + 1]) & 0x3F)));
                    i += 2;
                } else if ((b0 & 0xF0) == 0xE0 && i + 2 < el.name.size()) {
                    units.push_back(static_cast<uint16_t>(
                        ((b0 & 0x0F) << 12) |
                        ((static_cast<unsigned char>(el.name[i + 1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(el.name[i + 2]) & 0x3F)));
                    i += 3;
                } else {
                    units.push_back(static_cast<uint16_t>('?'));
                    i += 1;
                }
            }
            w.ascii("8BIM", 4);
            w.ascii("luni", 4);
            const size_t lenAt = w.size();
            w.u32(0);
            const size_t payloadAt = w.size();
            w.u32(static_cast<uint32_t>(units.size()));
            for (uint16_t u : units) {
                w.u16(u);
            }
            w.pad2In(payloadAt);
            w.patchU32(lenAt, static_cast<uint32_t>(w.size() - payloadAt));
        }
        w.patchU32(extraAt, static_cast<uint32_t>(w.size() - extraAt - 4));
    }

    // 通道数据（与记录同序：top-first）
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        const EncodedLayer& el = *it;
        el.red.writeTo(w);
        el.green.writeTo(w);
        el.blue.writeTo(w);
        el.alpha.writeTo(w);
        if (el.hasMask) {
            el.maskPlane.writeTo(w);
        }
    }

    const uint32_t layerInfoLen =
        static_cast<uint32_t>(w.size() - countAt);  // count + records + channel data
    w.pad2In(liLenAt + 4);  // LayerInfo 数据须偶数（liLenAt 指向长度字段，数据从 +4 起）
    w.patchU32(liLenAt, static_cast<uint32_t>(w.size() - liLenAt - 4));
    // global layer mask info（空段）
    w.u32(0);
    w.pad2In(lmLenAt + 4);
    w.patchU32(lmLenAt, static_cast<uint32_t>(w.size() - lmLenAt - 4));

    // ---- Image Data（合并图） ----
    flatR.writeTo(w);
    flatG.writeTo(w);
    flatB.writeTo(w);

    if (!w.flushTo(fd, &res.error)) {
        close(fd);
        return res;
    }
    res.bytes = w.size();
    res.ok = true;
    close(fd);
    return res;
}

}  // namespace io
}  // namespace montage
