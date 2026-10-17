#include "io/psd_export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "io/image_export.h"

namespace montage {
namespace io {

namespace {

constexpr uint32_t kTile = kTileSize;

// ---------- M17c：lrFX 图层效果写出（Photoshop 5 布局，对拍 photocraft parse_lrfx 契约） ----------

// 五类可表示效果 → lrFX 块数据。dsdw/isdw 41B、oglw/iglw 32B、sofi 24B；
// 未知偏移字段全部写 0（photocraft 读取契约只消费列出的偏移）。
std::vector<uint8_t> buildLrfx(const LayerStyle& st) {
    std::vector<uint8_t> out;
    auto push16 = [&out](uint16_t v) {
        out.push_back(static_cast<uint8_t>(v >> 8));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto push32 = [&out](uint32_t v) {
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    };
    auto pushColor = [&out, &push16](const float c[3]) {
        push16(0);  // space = RGB
        for (int k = 0; k < 4; ++k) {
            const uint32_t comp =
                k < 3 ? static_cast<uint32_t>(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f))
                      : 0;
            push16(static_cast<uint16_t>(comp << 8));
        }
    };
    auto pushFixed = [&out, &push32](double v) {
        push32(static_cast<uint32_t>(static_cast<int64_t>(std::llround(v * 65536.0))));
    };
    struct Item {
        const char* key;
        std::vector<uint8_t> data;
    };
    std::vector<Item> items;
    if (st.dropShadow) {
        std::vector<uint8_t> d(41, 0);
        auto fixed = [&d](size_t o, double v) {
            const uint32_t v32 =
                static_cast<uint32_t>(static_cast<int64_t>(std::llround(v * 65536.0)));
            d[o] = static_cast<uint8_t>(v32 >> 24);
            d[o + 1] = static_cast<uint8_t>(v32 >> 16);
            d[o + 2] = static_cast<uint8_t>(v32 >> 8);
            d[o + 3] = static_cast<uint8_t>(v32);
        };
        auto push16At = [&d](size_t o, uint16_t v) {
            d[o] = static_cast<uint8_t>(v >> 8);
            d[o + 1] = static_cast<uint8_t>(v & 0xFF);
        };
        fixed(4, st.shadowSize);
        fixed(12, st.shadowAngleDeg);
        fixed(16, st.shadowDistance);
        push16At(20, 0);
        for (int k = 0; k < 4; ++k) {
            const uint32_t comp =
                k < 3 ? static_cast<uint32_t>(
                            std::lround(std::clamp(st.shadowColor[k], 0.0f, 1.0f) * 255.0f))
                      : 0;
            push16At(22 + k * 2, static_cast<uint16_t>(comp << 8));
        }
        d[30] = 'N';
        d[31] = 'r';
        d[32] = 'm';
        d[33] = 'l';
        d[38] = 1;  // enabled
        d[39] = st.shadowUseGL ? 1 : 0;
        d[40] = static_cast<uint8_t>(std::lround(std::clamp(st.shadowOpacity, 0.0, 1.0) * 255.0));
        items.push_back({"dsdw", std::move(d)});
    }
    if (st.innerShadow) {
        std::vector<uint8_t> d(41, 0);
        auto fixed = [&d](size_t o, double v) {
            const uint32_t v32 =
                static_cast<uint32_t>(static_cast<int64_t>(std::llround(v * 65536.0)));
            d[o] = static_cast<uint8_t>(v32 >> 24);
            d[o + 1] = static_cast<uint8_t>(v32 >> 16);
            d[o + 2] = static_cast<uint8_t>(v32 >> 8);
            d[o + 3] = static_cast<uint8_t>(v32);
        };
        auto push16At = [&d](size_t o, uint16_t v) {
            d[o] = static_cast<uint8_t>(v >> 8);
            d[o + 1] = static_cast<uint8_t>(v & 0xFF);
        };
        fixed(4, st.isSize);
        fixed(12, st.isAngleDeg);
        fixed(16, st.isDistance);
        push16At(20, 0);
        for (int k = 0; k < 4; ++k) {
            const uint32_t comp =
                k < 3
                    ? static_cast<uint32_t>(std::lround(std::clamp(st.isColor[k], 0.0f, 1.0f) * 255.0f))
                    : 0;
            push16At(22 + k * 2, static_cast<uint16_t>(comp << 8));
        }
        d[30] = 'N';
        d[31] = 'r';
        d[32] = 'm';
        d[33] = 'l';
        d[38] = 1;
        d[39] = st.isUseGL ? 1 : 0;
        d[40] = static_cast<uint8_t>(std::lround(std::clamp(st.isOpacity, 0.0, 1.0) * 255.0));
        items.push_back({"isdw", std::move(d)});
    }
    if (st.outerGlow) {
        std::vector<uint8_t> d(32, 0);
        auto fixed = [&d](size_t o, double v) {
            const uint32_t v32 =
                static_cast<uint32_t>(static_cast<int64_t>(std::llround(v * 65536.0)));
            d[o] = static_cast<uint8_t>(v32 >> 24);
            d[o + 1] = static_cast<uint8_t>(v32 >> 16);
            d[o + 2] = static_cast<uint8_t>(v32 >> 8);
            d[o + 3] = static_cast<uint8_t>(v32);
        };
        auto push16At = [&d](size_t o, uint16_t v) {
            d[o] = static_cast<uint8_t>(v >> 8);
            d[o + 1] = static_cast<uint8_t>(v & 0xFF);
        };
        fixed(4, st.ogSize);
        push16At(12, 0);
        for (int k = 0; k < 4; ++k) {
            const uint32_t comp =
                k < 3
                    ? static_cast<uint32_t>(std::lround(std::clamp(st.ogColor[k], 0.0f, 1.0f) * 255.0f))
                    : 0;
            push16At(14 + k * 2, static_cast<uint16_t>(comp << 8));
        }
        d[22] = 'N';
        d[23] = 'r';
        d[24] = 'm';
        d[25] = 'l';
        d[30] = 1;
        d[31] = static_cast<uint8_t>(std::lround(std::clamp(st.ogOpacity, 0.0, 1.0) * 255.0));
        items.push_back({"oglw", std::move(d)});
    }
    if (st.innerGlow) {
        std::vector<uint8_t> d(32, 0);
        auto fixed = [&d](size_t o, double v) {
            const uint32_t v32 =
                static_cast<uint32_t>(static_cast<int64_t>(std::llround(v * 65536.0)));
            d[o] = static_cast<uint8_t>(v32 >> 24);
            d[o + 1] = static_cast<uint8_t>(v32 >> 16);
            d[o + 2] = static_cast<uint8_t>(v32 >> 8);
            d[o + 3] = static_cast<uint8_t>(v32);
        };
        auto push16At = [&d](size_t o, uint16_t v) {
            d[o] = static_cast<uint8_t>(v >> 8);
            d[o + 1] = static_cast<uint8_t>(v & 0xFF);
        };
        fixed(4, st.igSize);
        push16At(12, 0);
        for (int k = 0; k < 4; ++k) {
            const uint32_t comp =
                k < 3
                    ? static_cast<uint32_t>(std::lround(std::clamp(st.igColor[k], 0.0f, 1.0f) * 255.0f))
                    : 0;
            push16At(14 + k * 2, static_cast<uint16_t>(comp << 8));
        }
        d[22] = 'N';
        d[23] = 'r';
        d[24] = 'm';
        d[25] = 'l';
        d[30] = 1;
        d[31] = static_cast<uint8_t>(std::lround(std::clamp(st.igOpacity, 0.0, 1.0) * 255.0));
        items.push_back({"iglw", std::move(d)});
    }
    if (st.colorOverlay) {
        std::vector<uint8_t> d(24, 0);
        auto push16At = [&d](size_t o, uint16_t v) {
            d[o] = static_cast<uint8_t>(v >> 8);
            d[o + 1] = static_cast<uint8_t>(v & 0xFF);
        };
        d[4] = 'N';
        d[5] = 'r';
        d[6] = 'm';
        d[7] = 'l';
        push16At(12, 0);
        for (int k = 0; k < 4; ++k) {
            const uint32_t comp =
                k < 3 ? static_cast<uint32_t>(std::lround(
                                  std::clamp(st.overlayColor[k], 0.0f, 1.0f) * 255.0f))
                      : 0;
            push16At(14 + k * 2, static_cast<uint16_t>(comp << 8));
        }
        d[22] = static_cast<uint8_t>(
            std::lround(std::clamp(st.overlayOpacity, 0.0, 1.0) * 255.0));
        d[23] = 1;
        items.push_back({"sofi", std::move(d)});
    }
    if (items.empty()) {
        return out;
    }
    push16(0);  // version
    push16(static_cast<uint16_t>(items.size()));
    for (const Item& it : items) {
        out.push_back('8');
        out.push_back('B');
        out.push_back('I');
        out.push_back('M');
        out.push_back(it.key[0]);
        out.push_back(it.key[1]);
        out.push_back(it.key[2]);
        out.push_back(it.key[3]);
        push32(static_cast<uint32_t>(it.data.size()));
        out.insert(out.end(), it.data.begin(), it.data.end());
    }
    return out;
}

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
    // M15c（PSD-2）：记录类型 0=普通 1=组（展开）2=组（折叠）3=组尾 divider；
    // 组/divider/调整层 = 空矩形 + 4 条零长通道
    int recordType = 0;
    std::string adjKey;             // "levl"/"curv"/"nvrt"；空 = 无
    std::vector<uint8_t> adjPayload;
    std::vector<uint8_t> lrfx;      // M17c：图层效果（非空 = 附加信息写 lrFX 块）
};

}  // namespace

PsdExportResult exportPsd(int fd, const PsdDocSnap& snap) {
    PsdExportResult res;
    if (snap.width == 0 || snap.height == 0) {
        res.error = "psd: empty document";
        close(fd);
        return res;
    }

    // ---- Pass 1：层编码（bottom→top 树序发射；写出时逆序 top-first） ----
    // PSD-2 组序（与 reader M14a 组装互逆）：文件 bottom→top = [子层..., 组尾 divider(3),
    // 组头(1/2)]——子层先于组记录、组尾 divider 在组头之前。
    std::vector<EncodedLayer> layers;
    res.notes.clear();
    std::unordered_map<uint64_t, std::vector<size_t>> kids;
    std::vector<size_t> roots;
    for (size_t i = 0; i < snap.layers.size(); ++i) {
        const PsdLayerSnap& l = snap.layers[i];
        if (l.parentId != 0 && l.parentId != l.id) {
            kids[l.parentId].push_back(i);
        } else {
            roots.push_back(i);
        }
    }
    const auto encodePixel = [&](const PsdLayerSnap& l, EncodedLayer* el) -> bool {
        if (l.pixels == nullptr || l.pixels->tiles.empty()) {
            return false;
        }
        uint32_t px = 0, py = 0, pw = 0, ph = 0;
        if (!gridBBox(*l.pixels, px, py, pw, ph)) {
            return false;
        }
        el->name = l.name;
        el->left = static_cast<int>(l.originX) + static_cast<int>(px);
        el->top = static_cast<int>(l.originY) + static_cast<int>(py);
        el->right = el->left + static_cast<int>(pw);
        el->bottom = el->top + static_cast<int>(ph);
        el->key = blendToKey(l.blendMode);
        el->opacity = static_cast<uint8_t>(
            std::lround(std::min(1.0, std::max(0.0, l.opacity)) * 255.0));
        el->clipping = l.clipping ? 1 : 0;
        el->flags = l.visible ? 0 : 2;
        el->red = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 0);
        el->green = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 1);
        el->blue = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 2);
        el->alpha = encodePlaneFromGrid(*l.pixels, px, py, pw, ph, 3);
        if (l.maskGrid != nullptr && l.maskW > 0 && l.maskH > 0) {
            el->hasMask = true;
            el->mLeft = static_cast<int>(l.originX) + l.maskOffsetX;
            el->mTop = static_cast<int>(l.originY) + l.maskOffsetY;
            el->mRight = el->mLeft + static_cast<int>(l.maskW);
            el->mBottom = el->mTop + static_cast<int>(l.maskH);
            el->mDefault = static_cast<uint8_t>(l.maskOutside);
            el->mFlags = l.maskEnabled ? 0 : 2;
            el->maskPlane = encodePlaneFromMask(*l.maskGrid, l.maskW, l.maskH);
        }
        return true;
    };
    const auto encodeEmpty = [&](EncodedLayer* el) -> void {
        // 组/divider/调整层记录：空矩形 + 4 条零长通道（reader 按长度跳过）
        el->top = el->left = el->bottom = el->right = 0;
        el->key = "norm";
        el->opacity = 255;
        el->clipping = 0;
        el->flags = 0;
    };
    std::function<void(const std::vector<size_t>&, uint64_t)> emitRange =
        [&](const std::vector<size_t>& idxs, uint64_t /*parent*/) -> void {
        for (size_t i : idxs) {
            const PsdLayerSnap& l = snap.layers[i];
            if (l.isGroup) {
                // PSD 规范组块（bottom→top 文件序）= [组底 divider(3)，成员…，组头(1/2)]：
                // divider 在成员之前（reader M14a 装配契约同构；调色板顶部组头 = 文件最后）
                EncodedLayer d;
                encodeEmpty(&d);
                d.recordType = 3;
                d.name = "</Layer group>";
                layers.push_back(std::move(d));
                auto it = kids.find(l.id);
                if (it != kids.end()) {
                    emitRange(it->second, l.id);
                }
                EncodedLayer g;
                encodeEmpty(&g);
                g.recordType = l.collapsed ? 2 : 1;
                g.name = l.name;
                // 穿透（引擎 Normal+不透明 1）写 pass 键；否则写实际混合键
                g.key = (l.blendMode == BlendMode::Normal && l.opacity >= 0.999)
                            ? "pass" : blendToKey(l.blendMode);
                g.opacity = static_cast<uint8_t>(
                    std::lround(std::min(1.0, std::max(0.0, l.opacity)) * 255.0));
                g.clipping = l.clipping ? 1 : 0;
                g.flags = l.visible ? 0 : 2;
                layers.push_back(std::move(g));
                continue;
            }
            if (l.isAdjustment) {
                const bool supported = l.adjKind == 0 || l.adjKind == 1 || l.adjKind == 3;
                if (!supported) {
                    res.notes.push_back("调整层 \"" + l.name +
                                        "\" 的类型不支持 PSD 导出，已跳过。");
                    continue;
                }
                EncodedLayer a;
                encodeEmpty(&a);
                a.name = l.name;
                a.key = blendToKey(l.blendMode);
                a.opacity = static_cast<uint8_t>(
                    std::lround(std::min(1.0, std::max(0.0, l.opacity)) * 255.0));
                a.clipping = l.clipping ? 1 : 0;
                a.flags = l.visible ? 0 : 2;
                if (l.adjKind == 0) {
                    // levl：u32 ver + inB/inW/gamma(×100)/outB/outW i16（reader M9d 同构）
                    a.adjKey = "levl";
                    const auto push16 = [&a](int v) -> void {
                        a.adjPayload.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
                        a.adjPayload.push_back(static_cast<uint8_t>(v & 0xFF));
                    };
                    const auto clamp255 = [](double v) -> int {
                        return static_cast<int>(std::lround(
                            std::min(1.0, std::max(0.0, v)) * 255.0));
                    };
                    a.adjPayload.insert(a.adjPayload.end(), 4, 0);  // ver u32
                    push16(clamp255(l.adjInBlack));
                    push16(clamp255(l.adjInWhite));
                    push16(static_cast<int>(std::lround(
                        std::min(10.0f, std::max(0.1f, l.adjGamma)) * 100.0f)));
                    push16(0);     // outB
                    push16(255);   // outW
                } else if (l.adjKind == 1) {
                    // curv：u32 ver=1 + u16 count + count×(x,y) 字节（0..255）
                    a.adjKey = "curv";
                    const size_t n = std::min(l.adjCurveX.size(), l.adjCurveY.size());
                    a.adjPayload.insert(a.adjPayload.end(), {0, 0, 0, 1});
                    a.adjPayload.push_back(static_cast<uint8_t>((n >> 8) & 0xFF));
                    a.adjPayload.push_back(static_cast<uint8_t>(n & 0xFF));
                    for (size_t p2 = 0; p2 < n; ++p2) {
                        const auto bx = [](float v) -> uint8_t {
                            return static_cast<uint8_t>(
                                std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f));
                        };
                        a.adjPayload.push_back(bx(l.adjCurveX[p2]));
                        a.adjPayload.push_back(bx(l.adjCurveY[p2]));
                    }
                } else {
                    a.adjKey = "nvrt";  // 空 payload
                }
                layers.push_back(std::move(a));
                continue;
            }
            // 普通层（文本/填充/样式 = 栅格化像素导出 + note）
            if (l.isText) {
                res.notes.push_back("文字层 \"" + l.name + "\" 已栅格化为像素导出。");
            }
            if (l.isFill) {
                res.notes.push_back("填充图层 \"" + l.name + "\" 已栅格化为像素导出。");
            }
            if (l.hasStyle) {
                res.notes.push_back("图层 \"" + l.name +
                                    "\" 的样式效果已合成进像素导出（非破坏样式不保留）。");
            }
            EncodedLayer el;
            // M17c：可 lrFX 表示的样式 → 原始像素 + lrFX 块（像素已由快照分流为原始）
            if (l.style != nullptr) {
                el.lrfx = buildLrfx(*l.style);
            }
            if (encodePixel(l, &el)) {
                layers.push_back(std::move(el));
            }
        }
    };
    emitRange(roots, 0);
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

    // 层记录（PSD 规范 = bottom-first 存储；组装配与 PS 堆叠语义均依赖此序）
    for (const EncodedLayer& el : layers) {
        w.i32(el.top);
        w.i32(el.left);
        w.i32(el.bottom);
        w.i32(el.right);
        if (el.recordType != 0) {
            // M15c：组/divider/调整层 = 4 条零长通道（reader 按长度跳过）
            w.u16(4);
            w.i16(0);
            w.u32(0);
            w.i16(1);
            w.u32(0);
            w.i16(2);
            w.u32(0);
            w.i16(-1);
            w.u32(0);
        } else {
            // 通道表 = [nch u16][每通道 id i16 + len u32]（reader/PSD 规范同构）。
            // 396d006 起此处多写了一个 i16(0) 且 len/id 逆序——单层往返侥幸对齐、
            // 多记录（M15c 组/divider）起整体 2 字节/记录漂移，readPsd truncated。
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
        // mask section（组/divider/调整层无 mask 段）
        if (el.recordType == 0 && el.hasMask) {
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
        // M15c：组/divider → lsct（dividerType u32 + filler u32）；调整层 → levl/curv/nvrt
        if (el.recordType != 0) {
            w.ascii("8BIM", 4);
            w.ascii("lsct", 4);
            w.u32(8);
            w.u32(static_cast<uint32_t>(el.recordType));
            w.u32(0);
        }
        if (!el.adjKey.empty()) {
            w.ascii("8BIM", 4);
            w.ascii(el.adjKey.c_str(), 4);
            w.u32(static_cast<uint32_t>(el.adjPayload.size()));
            w.raw(el.adjPayload);
            w.pad2In(w.size() - el.adjPayload.size());
        }
        // M17c：图层效果（lrFX；PS 读取兼容——其自身产物同时含 lfx2+lrFX）
        if (!el.lrfx.empty()) {
            w.ascii("8BIM", 4);
            w.ascii("lrFX", 4);
            w.u32(static_cast<uint32_t>(el.lrfx.size()));
            w.raw(el.lrfx);
            w.pad2In(w.size() - el.lrfx.size());
        }
        w.patchU32(extraAt, static_cast<uint32_t>(w.size() - extraAt - 4));
    }

    // 通道数据（与记录同序 bottom-first）。空记录（组/divider/调整层）跳过——
    // 其通道长度写 0，若仍 writeTo 会多出 2B/平面的压缩头使数据段整体漂移。
    for (const EncodedLayer& el : layers) {
        if (el.recordType != 0) {
            continue;
        }
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
