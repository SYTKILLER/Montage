#include "engine/transform_ops.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "engine/style_ops.h"
#include "engine/vector_render.h"

namespace montage {

namespace {

constexpr uint32_t kTile = kTileSize;

// ---- 网格虚拟大图行读写（跨瓦片拼接；缺瓦片零填充/跳过写） ----

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

void gridWriteRow(const TileGrid& g, uint32_t rowY, const uint8_t* src) {
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
        uint8_t* dst = it->second->pixels->mapCpuWrite();
        if (dst == nullptr) {
            continue;
        }
        const uint32_t rb = it->second->pixels->rowBytes();
        std::memcpy(dst + static_cast<size_t>(inY) * rb,
                    src + static_cast<size_t>(tx) * kTile * 4u, kTile * 4u);
    }
}

// 深拷贝网格（保持不可变 COW 纪律；EngineBuffer 构造为零填充）
std::shared_ptr<TileGrid> copyGrid(const TileGrid& g) {
    auto copy = std::make_shared<TileGrid>();
    copy->cols = g.cols;
    copy->rows = g.rows;
    copy->revision = g.revision + 1;
    for (const auto& [idx, tile] : g.tiles) {
        auto t = std::make_shared<Tile>();
        auto* buf = new EngineBuffer(kTile, kTile);
        std::memcpy(buf->mapCpuWrite(), tile->pixels->mapCpu(),
                    static_cast<size_t>(tile->pixels->rowBytes()) * kTile);
        t->pixels.reset(buf);
        copy->tiles.emplace(idx, std::move(t));
    }
    return copy;
}

// 行数据写入 builder（按瓦片段 ensureTile；全零段跳过保持稀疏）
void builderWriteRow(TileGridBuilder& b, uint32_t rowY, const uint8_t* src, uint32_t cols) {
    for (uint32_t tx = 0; tx < cols; ++tx) {
        bool any = false;
        for (uint32_t k = 0; k < kTile; ++k) {
            if (src[(tx * kTile + k) * 4 + 3] != 0) {
                any = true;
                break;
            }
        }
        if (!any) {
            continue;
        }
        uint8_t* dst = b.ensureTile(tx, rowY / kTile).mapCpuWrite() + (rowY % kTile) * kTile * 4;
        std::memcpy(dst, src + tx * kTile * 4, kTile * 4);
    }
}

// 网格水平镜像（builder 重建：稀疏网格镜像目标瓦片可能不存在，就地写会丢数据——M8a 实证修复）
std::shared_ptr<const TileGrid> flipGridH(const TileGrid& g) {
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    TileGridBuilder builder(g.cols, g.rows);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        gridReadRow(g, y, row.data());
        std::reverse(reinterpret_cast<uint32_t*>(row.data()),
                     reinterpret_cast<uint32_t*>(row.data() + w * 4));
        builderWriteRow(builder, y, row.data(), g.cols);
    }
    return builder.publish(g.revision + 1);
}

// 网格垂直镜像（builder 重建）
std::shared_ptr<const TileGrid> flipGridV(const TileGrid& g) {
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    TileGridBuilder builder(g.cols, g.rows);
    std::vector<uint8_t> row(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        gridReadRow(g, h - 1 - y, row.data());
        builderWriteRow(builder, y, row.data(), g.cols);
    }
    return builder.publish(g.revision + 1);
}

// 网格 90° 旋转（CW: (x,y)→(H-1-y,x)；输出 cols/rows 互换；透明像素保持稀疏）
std::shared_ptr<const TileGrid> rotateGrid90(const TileGrid& g, bool clockwise) {
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    const uint32_t nw = clockwise ? h : w;
    const uint32_t nh = clockwise ? w : h;
    TileGridBuilder builder(nw / kTile, nh / kTile);
    std::vector<uint8_t> rowIn(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        gridReadRow(g, y, rowIn.data());
        for (uint32_t x = 0; x < w; ++x) {
            if (rowIn[x * 4 + 3] == 0) {
                continue;
            }
            uint32_t nx = 0;
            uint32_t ny = 0;
            if (clockwise) {
                nx = h - 1 - y;
                ny = x;
            } else {
                nx = y;
                ny = w - 1 - x;
            }
            uint8_t* dst = builder.ensureTile(nx / kTile, ny / kTile).mapCpuWrite() +
                           (ny % kTile) * kTile * 4 + (nx % kTile) * 4;
            std::memcpy(dst, rowIn.data() + x * 4, 4);
        }
    }
    return builder.publish(g.revision + 1);
}

// 蒙版深变换：像素核 + offset 重映射（patch 在图层局部坐标系）
void transformMask(Layer& l, bool horizontal, bool rotate, bool clockwise,
                   uint32_t oldW, uint32_t oldH) {
    if (l.mask == nullptr) {
        return;
    }
    auto m = std::make_shared<LayerMask>(*l.mask);
    if (m->pixels != nullptr) {
        if (rotate) {
            m->pixels = rotateGrid90(*m->pixels, clockwise);
        } else if (horizontal) {
            m->pixels = flipGridH(*m->pixels);
        } else {
            m->pixels = flipGridV(*m->pixels);
        }
    }
    // offset 重映射：patch 覆盖图层局部区间 [off, off+patch)
    //   翻转 H：局部 (x)→(LW-1-x) → off' = LW - off - patchW
    //   翻转 V：off' = LH - off - patchH
    //   旋转 CW：(x,y)→(LH-1-y,x) → offX' = LH - offY - patchH；offY' = offX
    //   旋转 CCW：(x,y)→(y,LW-1-x) → offX' = offY；offY' = LW - offX - patchW
    //   旋转 180 由两次 90 合成，无需单列
    // 注意：LW/LH 用旋转前的图层网格尺寸（oldW/oldH 方向的网格维）
    const auto gridDim = [&](bool width) -> int {
        const TileGrid* g = l.pixels.get();  // 已变换后的新网格
        if (g == nullptr) {
            return 0;
        }
        if (!rotate) {
            return static_cast<int>((width ? g->cols : g->rows) * kTile);
        }
        // 旋转后网格维互换：新 cols*256 = 旧 rows*256（= 旧方向的"高"）
        return static_cast<int>((width ? g->rows : g->cols) * kTile);
    };
    const int patchW = static_cast<int>(m->width);
    const int patchH = static_cast<int>(m->height);
    if (rotate) {
        // 旋转前图层网格：宽 = oldW 方向网格（= 变换后新网格的 rows 维）
        const int preW = gridDim(true);
        const int preH = gridDim(false);
        const int ox = m->offsetX;
        const int oy = m->offsetY;
        if (clockwise) {
            m->offsetX = preH - oy - patchH;
            m->offsetY = ox;
        } else {
            m->offsetX = oy;
            m->offsetY = preW - ox - patchW;
        }
    } else if (horizontal) {
        const int lw = gridDim(true);
        m->offsetX = lw - m->offsetX - patchW;
    } else {
        const int lh = gridDim(false);
        m->offsetY = lh - m->offsetY - patchH;
    }
    l.mask = std::move(m);
}

}  // namespace

bool visibleContentBBox(const Document& doc, int& x, int& y, int& w, int& h) {
    int bx0 = INT32_MAX;
    int by0 = INT32_MAX;
    int bx1 = INT32_MIN;
    int by1 = INT32_MIN;
    for (const Layer& l : doc.layers) {
        if (!l.visible || l.adjustment != nullptr) {
            continue;
        }
        const std::shared_ptr<const TileGrid> g = l.effectivePixels();
        if (g == nullptr || g->tiles.empty()) {
            continue;
        }
        // 墨迹精确 bbox（审查修复 P1-5：旧实现取瓦片索引 min/max——trim 后画布永远
        // 是 256 倍数，四周多出大片透明边；PS 语义 = 裁到墨迹）
        uint32_t ux = 0, uy = 0, uw = 0, uh = 0;
        if (!gridInkBBox(*g, ux, uy, uw, uh)) {
            continue;
        }
        const int lx = static_cast<int>(l.transform.originX) + static_cast<int>(ux);
        const int ly = static_cast<int>(l.transform.originY) + static_cast<int>(uy);
        bx0 = std::min(bx0, lx);
        by0 = std::min(by0, ly);
        bx1 = std::max(bx1, lx + static_cast<int>(uw));
        by1 = std::max(by1, ly + static_cast<int>(uh));
    }
    if (bx0 == INT32_MAX) {
        return false;
    }
    x = bx0;
    y = by0;
    w = bx1 - bx0;
    h = by1 - by0;
    return true;
}

bool resizeCanvas(Document& doc, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (x == 0 && y == 0 && static_cast<uint32_t>(w) == doc.width &&
        static_cast<uint32_t>(h) == doc.height) {
        return false;
    }
    for (Layer& l : doc.layers) {
        // origin 平移（新文档原点 = 旧 (x,y)）；蒙版 offset 是图层局部坐标，随层走不动
        shiftLayerOrigins(l, -static_cast<double>(x), -static_cast<double>(y));
    }
    doc.width = static_cast<uint32_t>(w);
    doc.height = static_cast<uint32_t>(h);
    // 会话态语义：画布重设即清除（M8a 简化登记）。审查修复 P1-4：quickMask 网格与
    // 文档尺寸/位置强绑定，尺寸变了不复位会在退出 Q 键时把错位通道转成选区；
    // 通道掩码与文档尺寸强绑定（读档侧同规格校验）→ 尺寸变化即弃（v1 登记）
    doc.selection = nullptr;
    doc.quickMask = nullptr;
    doc.channels.clear();
    return true;
}

bool flipDocument(Document& doc, bool horizontal) {
    if (doc.width == 0 || doc.height == 0) {
        return false;
    }
    for (Layer& l : doc.layers) {
        // M18：形状层路径同仿射（几何真源 = 路径点；hash 变化 → 收尾 restyleDocument 重派生）
        if (l.shape != nullptr) {
            if (horizontal) {
                affineShapeLayer(l, -1.0, 0.0, 0.0, 1.0, 0.0, static_cast<double>(doc.width));
            } else {
                affineShapeLayer(l, 1.0, 0.0, 0.0, -1.0, 0.0, static_cast<double>(doc.height));
            }
        }
        if (l.pixels != nullptr) {
            if (horizontal) {
                l.pixels = flipGridH(*l.pixels);
                const double gw = static_cast<double>(l.pixels->cols) * kTile;
                l.transform.originX =
                    static_cast<double>(doc.width) - l.transform.originX - gw;
            } else {
                l.pixels = flipGridV(*l.pixels);
                const double gh = static_cast<double>(l.pixels->rows) * kTile;
                l.transform.originY =
                    static_cast<double>(doc.height) - l.transform.originY - gh;
            }
        }
        transformMask(l, horizontal, false, false, doc.width, doc.height);
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.selection = nullptr;
    doc.quickMask = nullptr;  // P1-4：像素重排后通道错位（尺寸不变，通道掩码保留——内容不随镜像，v1 登记）
    for (Layer& l : doc.layers) {
        invalidateStyled(l);  // M14c：像素重排后样式派生失效（bridge 收尾重算）
    }
    return true;
}

bool rotateDocument90(Document& doc, bool clockwise) {
    if (doc.width == 0 || doc.height == 0) {
        return false;
    }
    const uint32_t oldW = doc.width;
    const uint32_t oldH = doc.height;
    for (Layer& l : doc.layers) {
        const double ox = l.transform.originX;
        const double oy = l.transform.originY;
        // M18：形状层路径同仿射（CW: x'=−y+oldH, y'=x；CCW: x'=y, y'=−x+oldW）
        if (l.shape != nullptr) {
            if (clockwise) {
                affineShapeLayer(l, 0.0, 1.0, -1.0, 0.0, static_cast<double>(oldH), 0.0);
            } else {
                affineShapeLayer(l, 0.0, -1.0, 1.0, 0.0, 0.0, static_cast<double>(oldW));
            }
        }
        if (l.pixels != nullptr) {
            l.pixels = rotateGrid90(*l.pixels, clockwise);
            const double gh = static_cast<double>(l.pixels->rows) * kTile;
            const double gw = static_cast<double>(l.pixels->cols) * kTile;
            if (clockwise) {
                l.transform.originX = static_cast<double>(oldH) - oy - gh;
                l.transform.originY = ox;
            } else {
                l.transform.originX = oy;
                l.transform.originY = static_cast<double>(oldW) - ox - gw;
            }
        }
        transformMask(l, false, true, clockwise, oldW, oldH);
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.width = oldH;
    doc.height = oldW;
    doc.selection = nullptr;
    doc.quickMask = nullptr;  // P1-4：像素重排后通道错位
    doc.channels.clear();     // 通道掩码与文档尺寸强绑定 → 维度互换即弃（v1 登记）
    for (Layer& l : doc.layers) {
        invalidateStyled(l);  // M14c：像素重排后样式派生失效（bridge 收尾重算）
    }
    return true;
}

bool translateSelection(Document& doc, int dx, int dy) {
    if (doc.selection == nullptr || (dx == 0 && dy == 0)) {
        return false;
    }
    const TileGrid& g = *doc.selection;
    const uint32_t w = g.cols * kTile;
    const uint32_t h = g.rows * kTile;
    TileGridBuilder builder(g.cols, g.rows);
    std::vector<uint8_t> row4(w * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const int srcY = static_cast<int>(y) - dy;
        if (srcY < 0 || srcY >= static_cast<int>(h)) {
            continue;
        }
        gridReadRow(g, static_cast<uint32_t>(srcY), row4.data());
        for (uint32_t x = 0; x < w; ++x) {
            const int sx = static_cast<int>(x) - dx;  // dest[x]=src[x-dx]：内容 +dx 平移（M8a 单测修正）
            if (sx < 0 || sx >= static_cast<int>(w)) {
                continue;
            }
            if (row4[sx * 4] == 0) {
                continue;  // 灰度 0 保持稀疏
            }
            const uint8_t gray = row4[sx * 4];
            uint8_t* dst = builder.ensureTile(x / kTile, y / kTile).mapCpuWrite() +
                           (y % kTile) * kTile * 4 + (x % kTile) * 4;
            dst[0] = dst[1] = dst[2] = gray;
            dst[3] = 255;
        }
    }
    doc.selection = builder.publish(g.revision + 1);
    return true;
}

// ---------- M14b 对齐/分布（多选层；纯 origin 移动，像素零改动） ----------

namespace {

// 可用层 doc 域内容包围盒（组/调整层/空层/无内容层返回 false 由调用方跳过）
bool alignBBoxOf(const Layer& l, int& x, int& y, int& w, int& h) {
    if (l.isGroup || l.adjustment != nullptr) {
        return false;
    }
    const std::shared_ptr<const TileGrid> px = l.effectivePixels();
    if (px == nullptr) {
        return false;
    }
    // 墨迹精确 bbox（P1-5 连带：旧 layerContentBBox 瓦片粒度——同瓦片内两个小图形
    // bbox 相同，对齐完全失效）
    uint32_t ux = 0, uy = 0, uw = 0, uh = 0;
    if (!gridInkBBox(*px, ux, uy, uw, uh)) {
        return false;
    }
    x = static_cast<int>(std::lround(l.transform.originX)) + static_cast<int>(ux);
    y = static_cast<int>(std::lround(l.transform.originY)) + static_cast<int>(uy);
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
    return true;
}

Layer* findLayerForAlign(Document& doc, LayerId id) {
    for (Layer& e : doc.layers) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

}  // namespace

bool alignLayers(Document& doc, const std::vector<LayerId>& ids, int mode, std::string& err) {
    if (mode < 0 || mode > 5) {
        err = "align: bad mode";
        return false;
    }
    // 收集可用层及其包围盒（对齐目标 = 选中集包围盒，PS 多选同语义）
    struct Box {
        Layer* l;
        int x, y, w, h;
    };
    std::vector<Box> boxes;
    int ux0 = 0, uy0 = 0, ux1 = 0, uy1 = 0;
    for (LayerId id : ids) {
        Layer* l = findLayerForAlign(doc, id);
        if (l == nullptr) {
            continue;
        }
        int x = 0, y = 0, w = 0, h = 0;
        if (!alignBBoxOf(*l, x, y, w, h)) {
            continue;
        }
        if (boxes.empty()) {
            ux0 = x;
            uy0 = y;
            ux1 = x + w;
            uy1 = y + h;
        } else {
            ux0 = std::min(ux0, x);
            uy0 = std::min(uy0, y);
            ux1 = std::max(ux1, x + w);
            uy1 = std::max(uy1, y + h);
        }
        boxes.push_back({l, x, y, w, h});
    }
    if (boxes.size() < 2) {
        err = "align: needs at least 2 layers with content";
        return false;
    }
    const bool horizontal = mode <= 2;
    for (const Box& b : boxes) {
        if (horizontal) {
            double dx = 0.0;
            if (mode == 0) {
                dx = ux0 - b.x;
            } else if (mode == 2) {
                dx = ux1 - (b.x + b.w);
            } else {
                dx = (ux0 + ux1) / 2.0 - (b.x + b.w / 2.0);
            }
            shiftLayerOrigins(*b.l, dx, 0.0);
        } else {
            double dy = 0.0;
            if (mode == 3) {
                dy = uy0 - b.y;
            } else if (mode == 5) {
                dy = uy1 - (b.y + b.h);
            } else {
                dy = (uy0 + uy1) / 2.0 - (b.y + b.h / 2.0);
            }
            shiftLayerOrigins(*b.l, 0.0, dy);
        }
    }
    return true;
}

bool distributeLayers(Document& doc, const std::vector<LayerId>& ids, int mode, std::string& err) {
    if (mode < 0 || mode > 5) {
        err = "distribute: bad mode";
        return false;
    }
    struct Item {
        Layer* l;
        double metric;
    };
    std::vector<Item> items;
    for (LayerId id : ids) {
        Layer* l = findLayerForAlign(doc, id);
        if (l == nullptr) {
            continue;
        }
        int x = 0, y = 0, w = 0, h = 0;
        if (!alignBBoxOf(*l, x, y, w, h)) {
            continue;
        }
        double m = 0.0;
        switch (mode) {
            case 0: m = x; break;
            case 1: m = x + w / 2.0; break;
            case 2: m = x + w; break;
            case 3: m = y; break;
            case 4: m = y + h / 2.0; break;
            default: m = y + h; break;
        }
        items.push_back({l, m});
    }
    if (items.size() < 3) {
        err = "distribute: needs at least 3 layers with content";
        return false;
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.metric < b.metric; });
    // 选定边/中心线性等距插值；首尾锚定不动（PS 分布语义）
    const double first = items.front().metric;
    const double span = items.back().metric - first;
    const size_t n = items.size();
    for (size_t k = 1; k + 1 < n; ++k) {
        const double target = first + span * static_cast<double>(k) / static_cast<double>(n - 1);
        const double delta = target - items[k].metric;
        if (mode <= 2) {
            shiftLayerOrigins(*items[k].l, delta, 0.0);
        } else {
            shiftLayerOrigins(*items[k].l, 0.0, delta);
        }
    }
    return true;
}

}  // namespace montage
