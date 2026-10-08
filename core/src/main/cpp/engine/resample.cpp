#include "engine/resample.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace montage {

namespace {

constexpr uint32_t kTile = kTileSize;

// 网格行读（与 transform_ops 同构；resample 独立副本避免跨头暴露）
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

// 2×3 仿射逆矩阵（[a c e; b d f] 列主序存 m[6]={a,b,c,d,e,f} 表示 x'=a·x+c·y+e）
// 逆矩阵：m = {a,b,c,d,e,f} 表示 x'=a·x+b·y+e, y'=c·x+d·y+f（行主序平移在后）
bool invert2x3(const double in[6], double out[6]) {
    const double a = in[0];
    const double b = in[1];
    const double e = in[2];
    const double c = in[3];
    const double d = in[4];
    const double f = in[5];
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-12) {
        return false;
    }
    // src = M⁻¹·(dst - t)
    out[0] = d / det;
    out[1] = -b / det;
    out[2] = (b * f - d * e) / det;
    out[3] = -c / det;
    out[4] = a / det;
    out[5] = (c * e - a * f) / det;
    return true;
}

}  // namespace

std::shared_ptr<const TileGrid> warpGridAffine(const TileGrid& src, const double m[6],
                                               uint32_t dstCols, uint32_t dstRows) {
    double inv[6];
    if (!invert2x3(m, inv)) {
        return nullptr;
    }
    const uint32_t srcW = src.cols * kTile;
    const uint32_t srcH = src.rows * kTile;
    const uint32_t dstW = dstCols * kTile;
    const uint32_t dstH = dstRows * kTile;
    TileGridBuilder builder(dstCols, dstRows);
    std::vector<uint8_t> srcRow(srcW * 4);
    int32_t curRow = -1;  // 惰性行读（warp 常稀疏）
    for (uint32_t dy = 0; dy < dstH; ++dy) {
        bool rowHasContent = false;
        std::vector<uint8_t> dstRow(dstW * 4, 0);
        for (uint32_t dx = 0; dx < dstW; ++dx) {
            // 逆映射（dst 中心采样 +0.5 对齐：以像素中心连续坐标计算）
            const double sx = inv[0] * (dx + 0.5) + inv[1] * (dy + 0.5) + inv[2] - 0.5;
            const double sy = inv[3] * (dx + 0.5) + inv[4] * (dy + 0.5) + inv[5] - 0.5;
            if (sx < -0.5 || sy < -0.5 || sx > srcW - 0.5 || sy > srcH - 0.5) {
                continue;
            }
            const int32_t x0 = static_cast<int32_t>(std::floor(sx));
            const int32_t y0 = static_cast<int32_t>(std::floor(sy));
            const double fx = sx - x0;
            const double fy = sy - y0;
            if (curRow != y0 && y0 >= 0 && y0 < static_cast<int32_t>(srcH)) {
                gridReadRow(src, static_cast<uint32_t>(y0), srcRow.data());
                curRow = y0;
            }
            auto sample = [&](int32_t sxp, int32_t syp, int c) -> double {
                if (sxp < 0 || syp < 0 || sxp >= static_cast<int32_t>(srcW) ||
                    syp >= static_cast<int32_t>(srcH)) {
                    return 0.0;
                }
                if (syp != curRow) {
                    gridReadRow(src, static_cast<uint32_t>(syp), srcRow.data());
                    curRow = syp;
                }
                return srcRow[static_cast<size_t>(sxp) * 4u + static_cast<size_t>(c)];
            };
            uint8_t* out = dstRow.data() + static_cast<size_t>(dx) * 4u;
            double alpha = 0.0;
            for (int c = 0; c < 4; ++c) {
                const double v =
                    sample(x0, y0, c) * (1 - fx) * (1 - fy) +
                    sample(x0 + 1, y0, c) * fx * (1 - fy) +
                    sample(x0, y0 + 1, c) * (1 - fx) * fy +
                    sample(x0 + 1, y0 + 1, c) * fx * fy;
                out[c] = static_cast<uint8_t>(std::lround(std::min(255.0, std::max(0.0, v))));
                if (c == 3) {
                    alpha = out[c];
                }
            }
            if (alpha > 0) {
                rowHasContent = true;
            }
        }
        if (!rowHasContent) {
            continue;
        }
        for (uint32_t tx = 0; tx < dstCols; ++tx) {
            bool any = false;
            for (uint32_t k = 0; k < kTile; ++k) {
                if (dstRow[(tx * kTile + k) * 4 + 3] != 0) {
                    any = true;
                    break;
                }
            }
            if (!any) {
                continue;
            }
            uint8_t* dst = builder.ensureTile(tx, dy / kTile).mapCpuWrite() +
                           (dy % kTile) * kTile * 4;
            std::memcpy(dst, dstRow.data() + tx * kTile * 4, kTile * 4);
        }
    }
    return builder.publish(src.revision + 1);
}

bool resampleDocument(Document& doc, uint32_t newW, uint32_t newH) {
    if (doc.width == 0 || doc.height == 0 || newW == 0 || newH == 0) {
        return false;
    }
    if (newW == doc.width && newH == doc.height) {
        return false;
    }
    const double sx = static_cast<double>(newW) / doc.width;
    const double sy = static_cast<double>(newH) / doc.height;
    for (Layer& l : doc.layers) {
        const double m[6] = {sx, 0.0, 0.0, 0.0, sy, 0.0};  // x'=sx·x, y'=sy·y（行主序）
        const uint32_t newCols = std::max(1u, static_cast<uint32_t>(
            std::ceil(static_cast<double>(l.pixels ? l.pixels->cols : 1) * sx)));
        const uint32_t newRows = std::max(1u, static_cast<uint32_t>(
            std::ceil(static_cast<double>(l.pixels ? l.pixels->rows : 1) * sy)));
        if (l.pixels != nullptr) {
            l.pixels = warpGridAffine(*l.pixels, m, newCols, newRows);
        }
        l.transform.originX *= sx;
        l.transform.originY *= sy;
        if (l.mask != nullptr) {
            auto m2 = std::make_shared<LayerMask>(*l.mask);
            if (m2->pixels != nullptr) {
                const uint32_t mCols = std::max(1u, static_cast<uint32_t>(
                    std::ceil(m2->pixels->cols * sx)));
                const uint32_t mRows = std::max(1u, static_cast<uint32_t>(
                    std::ceil(m2->pixels->rows * sy)));
                m2->pixels = warpGridAffine(*m2->pixels, m, mCols, mRows);
            }
            m2->width = std::max(1u, static_cast<uint32_t>(std::lround(m2->width * sx)));
            m2->height = std::max(1u, static_cast<uint32_t>(std::lround(m2->height * sy)));
            m2->offsetX = static_cast<int>(std::lround(m2->offsetX * sx));
            m2->offsetY = static_cast<int>(std::lround(m2->offsetY * sy));
            l.mask = std::move(m2);
        }
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.width = newW;
    doc.height = newH;
    doc.selection = nullptr;
    return true;
}

bool rotateDocumentArbitrary(Document& doc, double degreesCw) {
    if (doc.width == 0 || doc.height == 0) {
        return false;
    }
    // 顺时针 θ：屏幕坐标 y 向下 → 旋转矩阵 x' = cosθ·x - sinθ·y, y' = sinθ·x + cosθ·y
    const double rad = degreesCw * 3.14159265358979323846 / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    // 四角变换求新包围盒
    const double cx[4] = {0.0, static_cast<double>(doc.width), 0.0, static_cast<double>(doc.width)};
    const double cy[4] = {0.0, 0.0, static_cast<double>(doc.height), static_cast<double>(doc.height)};
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (int i = 0; i < 4; ++i) {
        const double nx = c * cx[i] - s * cy[i];
        const double ny = s * cx[i] + c * cy[i];
        minX = std::min(minX, nx);
        minY = std::min(minY, ny);
        maxX = std::max(maxX, nx);
        maxY = std::max(maxY, ny);
    }
    const uint32_t newW = static_cast<uint32_t>(std::ceil(maxX - minX));
    const uint32_t newH = static_cast<uint32_t>(std::ceil(maxY - minY));
    // 平移使包围盒左上角对齐原点：t = (-minX, -minY)
    for (Layer& l : doc.layers) {
        const double ox = l.transform.originX;
        const double oy = l.transform.originY;
        // origin 映射（含包围盒平移）
        l.transform.originX = c * ox - s * oy - minX;
        l.transform.originY = s * ox + c * oy - minY;
        if (l.pixels != nullptr) {
            // 层网格内容同矩阵；新网格尺寸覆盖旋转后的层包围盒
            const double gw = static_cast<double>(l.pixels->cols * kTile);
            const double gh = static_cast<double>(l.pixels->rows * kTile);
            const double gx[4] = {0.0, gw, 0.0, gw};
            const double gy[4] = {0.0, 0.0, gh, gh};
            double gnx0 = 1e18, gny0 = 1e18, gnx1 = -1e18, gny1 = -1e18;
            for (int i = 0; i < 4; ++i) {
                const double nx = c * gx[i] - s * gy[i];
                const double ny = s * gx[i] + c * gy[i];
                gnx0 = std::min(gnx0, nx);
                gny0 = std::min(gny0, ny);
                gnx1 = std::max(gnx1, nx);
                gny1 = std::max(gny1, ny);
            }
            const uint32_t newCols = std::max(1u, static_cast<uint32_t>(std::ceil(gnx1 - gnx0)));
            const uint32_t newRows = std::max(1u, static_cast<uint32_t>(std::ceil(gny1 - gny0)));
            // 层局部矩阵 = 文档矩阵 + 层内平移 -gmin
            const double lm[6] = {c, -s, -gnx0, s, c, -gny0};
            l.pixels = warpGridAffine(*l.pixels, lm, newCols, newRows);
            // origin_new = Mdoc(origin_old) + gmin（q=Mlayer·p=Mrot·p-gmin 的补偿，M8c 推导）
            l.transform.originX += gnx0;
            l.transform.originY += gny0;
        }
        if (l.mask != nullptr) {
            auto m2 = std::make_shared<LayerMask>(*l.mask);
            if (m2->pixels != nullptr) {
                const double gw = static_cast<double>(m2->pixels->cols * kTile);
                const double gh = static_cast<double>(m2->pixels->rows * kTile);
                const double gx[4] = {0.0, gw, 0.0, gw};
                const double gy[4] = {0.0, 0.0, gh, gh};
                double gnx0 = 1e18, gny0 = 1e18, gnx1 = -1e18, gny1 = -1e18;
                for (int i = 0; i < 4; ++i) {
                    const double nx = c * gx[i] - s * gy[i];
                    const double ny = s * gx[i] + c * gy[i];
                    gnx0 = std::min(gnx0, nx);
                    gny0 = std::min(gny0, ny);
                    gnx1 = std::max(gnx1, nx);
                    gny1 = std::max(gny1, ny);
                }
                const uint32_t newCols = std::max(1u, static_cast<uint32_t>(std::ceil(gnx1 - gnx0)));
                const uint32_t newRows = std::max(1u, static_cast<uint32_t>(std::ceil(gny1 - gny0)));
                const double lm[6] = {c, -s, -gnx0, s, c, -gny0};
                m2->pixels = warpGridAffine(*m2->pixels, lm, newCols, newRows);
                // patch 逻辑尺寸/offset 随旋转包围盒更新（近似：包围盒左上角对齐）
                m2->width = newCols;
                m2->height = newRows;
                const double mox = static_cast<double>(m2->offsetX);
                const double moy = static_cast<double>(m2->offsetY);
                m2->offsetX = static_cast<int>(std::lround(c * mox - s * moy - gnx0));
                m2->offsetY = static_cast<int>(std::lround(s * mox + c * moy - gny0));
            }
            l.mask = std::move(m2);
        }
        if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
            l.render = composeMasked(l.pixels, *l.mask);
        }
    }
    doc.width = newW;
    doc.height = newH;
    doc.selection = nullptr;
    return true;
}


bool bakeLayerMatrix(Document& doc, LayerId id, const double m[6]) {
    if (doc.width == 0 || doc.height == 0) {
        return false;
    }
    Layer* target = nullptr;
    for (Layer& l : doc.layers) {
        if (l.id == id) {
            target = &l;
            break;
        }
    }
    if (target == nullptr || target->pixels == nullptr) {
        return false;
    }
    Layer& l = *target;
    const double gw = static_cast<double>(l.pixels->cols * kTile);
    const double gh = static_cast<double>(l.pixels->rows * kTile);
    const double gx[4] = {0.0, gw, 0.0, gw};
    const double gy[4] = {0.0, 0.0, gh, gh};
    double nx0 = 1e18, ny0 = 1e18, nx1 = -1e18, ny1 = -1e18;
    for (int i = 0; i < 4; ++i) {
        const double nx = m[0] * gx[i] + m[1] * gy[i] + m[2];
        const double ny = m[3] * gx[i] + m[4] * gy[i] + m[5];
        nx0 = std::min(nx0, nx);
        ny0 = std::min(ny0, ny);
        nx1 = std::max(nx1, nx);
        ny1 = std::max(ny1, ny);
    }
    const uint32_t newCols = std::max(1u, static_cast<uint32_t>(std::ceil(nx1 - nx0)));
    const uint32_t newRows = std::max(1u, static_cast<uint32_t>(std::ceil(ny1 - ny0)));
    const double lm[6] = {m[0], m[1], m[2] - nx0, m[3], m[4], m[5] - ny0};
    auto warped = warpGridAffine(*l.pixels, lm, newCols, newRows);
    if (warped == nullptr) {
        return false;  // 退化矩阵/分配失败：不改动（防御 T7 链路空指针）
    }
    l.pixels = warped;
    // origin_new = M(origin_old) + (nx0, ny0)（内容 doc 位置守恒，T7 推导）
    const double tox = m[0] * l.transform.originX + m[1] * l.transform.originY + m[2];
    const double toy = m[3] * l.transform.originX + m[4] * l.transform.originY + m[5];
    l.transform.originX = tox + nx0;
    l.transform.originY = toy + ny0;
    if (l.mask != nullptr) {
        auto m2m = std::make_shared<LayerMask>(*l.mask);
        if (m2m->pixels != nullptr) {
            const double mgw = static_cast<double>(m2m->pixels->cols * kTile);
            const double mgh = static_cast<double>(m2m->pixels->rows * kTile);
            const double mgx[4] = {0.0, mgw, 0.0, mgw};
            const double mgy[4] = {0.0, 0.0, mgh, mgh};
            double mnx0 = 1e18, mny0 = 1e18, mnx1 = -1e18, mny1 = -1e18;
            for (int i = 0; i < 4; ++i) {
                const double nx = m[0] * mgx[i] + m[1] * mgy[i] + m[2];
                const double ny = m[3] * mgx[i] + m[4] * mgy[i] + m[5];
                mnx0 = std::min(mnx0, nx);
                mny0 = std::min(mny0, ny);
                mnx1 = std::max(mnx1, nx);
                mny1 = std::max(mny1, ny);
            }
            const uint32_t mCols = std::max(1u, static_cast<uint32_t>(std::ceil(mnx1 - mnx0)));
            const uint32_t mRows = std::max(1u, static_cast<uint32_t>(std::ceil(mny1 - mny0)));
            const double mlm[6] = {m[0], m[1], m[2] - mnx0, m[3], m[4], m[5] - mny0};
            m2m->pixels = warpGridAffine(*m2m->pixels, mlm, mCols, mRows);
            m2m->width = mCols;
            m2m->height = mRows;
            const double mox = static_cast<double>(m2m->offsetX);
            const double moy = static_cast<double>(m2m->offsetY);
            m2m->offsetX = static_cast<int>(std::lround(m[0] * mox + m[1] * moy + m[2] + mnx0));
            m2m->offsetY = static_cast<int>(std::lround(m[3] * mox + m[4] * moy + m[5] + mny0));
        }
        l.mask = std::move(m2m);
    }
    if (l.mask != nullptr && l.mask->enabled && l.pixels != nullptr) {
        l.render = composeMasked(l.pixels, *l.mask);
    }
    return true;
}

bool bakeLayerTransform(Document& doc, LayerId id, double scaleX, double scaleY,
                        double rotDegCw, double skewDeg, double pivotX, double pivotY) {
    const double rad = rotDegCw * 3.14159265358979323846 / 180.0;
    const double cr = std::cos(rad);
    const double sr = std::sin(rad);
    const double k = std::tan(skewDeg * 3.14159265358979323846 / 180.0);
    // M = T(p) · R · SkewX · S · T(-p)（行主序）
    const double m0 = cr * scaleX + (-sr) * k * scaleY;
    const double m1 = -sr * scaleY;
    const double m2 = pivotX - m0 * pivotX - m1 * pivotY;
    const double m3 = sr * scaleX + cr * k * scaleY;
    const double m4 = cr * scaleY;
    const double m5 = pivotY - m3 * pivotX - m4 * pivotY;
    const double mm[6] = {m0, m1, m2, m3, m4, m5};
    return bakeLayerMatrix(doc, id, mm);
}

}  // namespace montage
