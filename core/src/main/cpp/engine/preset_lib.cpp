#include "engine/preset_lib.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage {

namespace {
constexpr uint32_t kTile = 256;
constexpr uint32_t kThumbEdge = 128;
}  // namespace

PresetLib& PresetLib::get() {
    static PresetLib inst;
    return inst;
}

bool PresetLib::addPattern(const std::string& name, uint32_t w, uint32_t h, const uint8_t* rgba,
                           std::string& idOut, std::string& err) {
    if (rgba == nullptr || w == 0 || h == 0 || w > kMaxEdge || h > kMaxEdge) {
        err = "pattern size out of range (1.." + std::to_string(kMaxEdge) + ")";
        return false;
    }
    if (patterns.size() >= kMaxPatterns) {
        err = "pattern library full (" + std::to_string(kMaxPatterns) + "); delete one first";
        return false;
    }
    TileGridBuilder b((w + kTile - 1) / kTile, (h + kTile - 1) / kTile);
    for (uint32_t ty = 0; ty < b.rows(); ++ty) {
        for (uint32_t tx = 0; tx < b.cols(); ++tx) {
            EngineBuffer& t = b.ensureTile(tx, ty);
            uint8_t* dst = t.mapCpuWrite();
            const uint32_t x0 = tx * kTile;
            const uint32_t y0 = ty * kTile;
            for (uint32_t y = 0; y < kTile; ++y) {
                const uint32_t sy = y0 + y;
                if (sy >= h) {
                    std::memset(dst + static_cast<size_t>(y) * t.rowBytes(), 0,
                                static_cast<size_t>(kTile) * 4);
                    continue;
                }
                const uint32_t copyW = std::min<uint32_t>(kTile, w - x0);
                for (uint32_t x = 0; x < kTile; ++x) {
                    uint8_t* row = dst + static_cast<size_t>(y) * t.rowBytes();
                    if (x < copyW) {
                        const size_t si = (static_cast<size_t>(sy) * w + x0 + x) * 4;
                        row[x * 4 + 0] = rgba[si + 0];
                        row[x * 4 + 1] = rgba[si + 1];
                        row[x * 4 + 2] = rgba[si + 2];
                        row[x * 4 + 3] = rgba[si + 3];
                    } else {
                        row[x * 4 + 0] = row[x * 4 + 1] = row[x * 4 + 2] = row[x * 4 + 3] = 0;
                    }
                }
            }
            t.unmap();
        }
    }
    LibPattern p;
    p.id = "libpat_" + std::to_string(++seq_);
    p.name = name;
    p.w = w;
    p.h = h;
    p.pixels = b.publish(1);
    idOut = p.id;
    patterns.push_back(std::move(p));
    return true;
}

bool PresetLib::addTip(const std::string& name, uint32_t w, uint32_t h, const uint8_t* gray,
                       std::string& idOut, std::string& err) {
    (void)name;
    if (gray == nullptr || w == 0 || h == 0 || w > kMaxEdge || h > kMaxEdge) {
        err = "tip size out of range (1.." + std::to_string(kMaxEdge) + ")";
        return false;
    }
    if (tips.size() >= kMaxTips) {
        err = "brush tip library full (" + std::to_string(kMaxTips) + "); delete one first";
        return false;
    }
    LibTip t;
    t.id = "libtip_" + std::to_string(++seq_);
    t.w = w;
    t.h = h;
    t.gray.assign(gray, gray + static_cast<size_t>(w) * h);
    idOut = t.id;
    tips.push_back(std::move(t));
    return true;
}

bool PresetLib::dropPattern(const std::string& id) {
    for (size_t i = 0; i < patterns.size(); ++i) {
        if (patterns[i].id == id) {
            patterns.erase(patterns.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

bool PresetLib::dropTip(const std::string& id) {
    for (size_t i = 0; i < tips.size(); ++i) {
        if (tips[i].id == id) {
            tips.erase(tips.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

const PresetLib::LibPattern* PresetLib::findPattern(const std::string& id) const {
    for (const LibPattern& p : patterns) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const PresetLib::LibTip* PresetLib::findTip(const std::string& id) const {
    for (const LibTip& t : tips) {
        if (t.id == id) return &t;
    }
    return nullptr;
}

bool PresetLib::thumb(const std::string& id, bool isTip, uint32_t& w, uint32_t& h,
                      std::vector<uint8_t>& rgba) const {
    uint32_t sw = 0, sh = 0;
    std::vector<uint8_t> src;  // 统一 RGBA 源（tip 灰度 → 白底 RGBA）
    if (isTip) {
        const LibTip* t = findTip(id);
        if (t == nullptr) return false;
        sw = t->w;
        sh = t->h;
        src.resize(static_cast<size_t>(sw) * sh * 4);
        for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i) {
            src[i * 4 + 0] = src[i * 4 + 1] = src[i * 4 + 2] = t->gray[i];
            src[i * 4 + 3] = 255;
        }
    } else {
        const LibPattern* p = findPattern(id);
        if (p == nullptr || p->pixels == nullptr) return false;
        sw = p->w;
        sh = p->h;
        src.resize(static_cast<size_t>(sw) * sh * 4);
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x) {
                auto it = p->pixels->tiles.find(y / kTile * p->pixels->cols + x / kTile);
                uint8_t* d = src.data() + (static_cast<size_t>(y) * sw + x) * 4;
                if (it == p->pixels->tiles.end() || it->second == nullptr ||
                    it->second->pixels == nullptr) {
                    d[0] = d[1] = d[2] = d[3] = 0;
                    continue;
                }
                const uint8_t* s = it->second->pixels->mapCpu();
                const size_t off = (static_cast<size_t>(y % kTile) * it->second->pixels->rowBytes() +
                                    (x % kTile)) * 4;
                d[0] = s[off];
                d[1] = s[off + 1];
                d[2] = s[off + 2];
                d[3] = s[off + 3];
            }
        }
    }
    // box 下采样到 ≤128 边（photocraft abr_map fit 同式）
    if (sw <= kThumbEdge && sh <= kThumbEdge) {
        w = sw;
        h = sh;
        rgba = std::move(src);
        return true;
    }
    const float k = std::max(sw, sh) / static_cast<float>(kThumbEdge);
    const uint32_t nw = std::clamp(static_cast<uint32_t>(std::lround(sw / k)), 1u, kThumbEdge);
    const uint32_t nh = std::clamp(static_cast<uint32_t>(std::lround(sh / k)), 1u, kThumbEdge);
    rgba.assign(static_cast<size_t>(nw) * nh * 4, 0);
    for (uint32_t y = 0; y < nh; ++y) {
        const uint32_t y0 = y * sh / nh;
        const uint32_t y1 = std::min(sh, std::max((y + 1) * sh / nh, y0 + 1));
        for (uint32_t x = 0; x < nw; ++x) {
            const uint32_t x0 = x * sw / nw;
            const uint32_t x1 = std::min(sw, std::max((x + 1) * sw / nw, x0 + 1));
            uint32_t sum[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (uint32_t yy = y0; yy < y1; ++yy) {
                for (uint32_t xx = x0; xx < x1; ++xx) {
                    const uint8_t* s = src.data() + (static_cast<size_t>(yy) * sw + xx) * 4;
                    for (int c = 0; c < 4; ++c) sum[c] += s[c];
                    n++;
                }
            }
            uint8_t* d = rgba.data() + (static_cast<size_t>(y) * nw + x) * 4;
            for (int c = 0; c < 4; ++c) d[c] = static_cast<uint8_t>(sum[c] / n);
        }
    }
    w = nw;
    h = nh;
    return true;
}

}  // namespace montage
