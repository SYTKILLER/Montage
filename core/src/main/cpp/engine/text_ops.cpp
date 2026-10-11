#include "engine/text_ops.h"

#include <algorithm>
#include <cstring>

#include <hilog/log.h>

#include "engine/text_raster.h"

namespace montage {

namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Text";

// FNV-1a 64 位（指纹：内容含长度，样式逐字段）
void hashMix(uint64_t& h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
}

// 点文字最大排版宽（不自动换行；65536 px 上限足够任何文档）
constexpr double kPointTextMaxWidth = 65536.0;
}  // namespace

uint64_t textStyleHash(const TextLayerState& t) {
    uint64_t h = 1469598103934665603ull;
    hashMix(h, t.content.data(), t.content.size());
    const size_t contentLen = t.content.size();
    hashMix(h, &contentLen, sizeof(contentLen));  // 分隔内容与样式（防空串歧义）
    hashMix(h, t.fontFamily.data(), t.fontFamily.size());
    hashMix(h, &t.fontSize, sizeof(t.fontSize));
    hashMix(h, &t.lineHeight, sizeof(t.lineHeight));
    hashMix(h, &t.tracking, sizeof(t.tracking));
    const uint8_t flags = static_cast<uint8_t>((t.bold ? 1 : 0) | (t.italic ? 2 : 0) |
                                               (t.align << 2));
    hashMix(h, &flags, 1);
    hashMix(h, t.color, sizeof(t.color));
    hashMix(h, &t.wrapWidth, sizeof(t.wrapWidth));
    return h;
}

bool relayoutTextLayer(Layer& l, std::string& err) {
    if (l.text == nullptr) {
        err = "not a text layer";
        return false;
    }
    TextLayerState& t = *l.text;
    const uint64_t hash = textStyleHash(t);
    // 空内容栅格 = 无像素；hash 相同即缓存命中（否则空层每次编辑事件都重排）
    if (hash == t.styleHash && (l.pixels != nullptr || t.content.empty())) {
        return true;  // 参数未变（缓存）
    }
    bool fontMissing = false;
    TextRasterResult ras;
    const double maxWidth = t.wrapWidth > 0.0 ? t.wrapWidth : kPointTextMaxWidth;
    if (!rasterizeText(t, maxWidth, ras, fontMissing, err)) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "rasterize failed: %{public}s", err.c_str());
        return false;
    }
    if (fontMissing) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag,
                     "font missing, fallback default: %{public}s", t.fontFamily.c_str());
    }
    if (ras.width == 0 || ras.height == 0) {
        // 空内容：无像素（编辑态由 overlay 承担视觉）
        l.pixels = nullptr;
        l.render = nullptr;
        t.rasterW = 0.0;
        t.rasterH = 0.0;
        t.styleHash = hash;
        return true;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "relayout: content=%{public}zu chars -> raster %{public}du x %{public}du",
                 t.content.size(), ras.width, ras.height);
    // 整幅直 alpha → 256 瓦片网格（行距 = ras.width*4 → 瓦片行距 = 256*4）
    TileGridBuilder builder((ras.width + kTileSize - 1) / kTileSize,
                            (ras.height + kTileSize - 1) / kTileSize);
    const size_t srcStride = static_cast<size_t>(ras.width) * 4u;
    for (uint32_t ty = 0; ty < builder.rows(); ++ty) {
        for (uint32_t tx = 0; tx < builder.cols(); ++tx) {
            EngineBuffer& buf = builder.ensureTile(tx, ty);
            uint8_t* dst = buf.mapCpuWrite();
            const uint32_t y0 = ty * kTileSize;
            const uint32_t x0 = tx * kTileSize;
            const uint32_t rows = std::min<uint32_t>(kTileSize, ras.height - y0);
            const uint32_t cols = std::min<uint32_t>(kTileSize, ras.width - x0);
            for (uint32_t r = 0; r < rows; ++r) {
                std::memcpy(dst + static_cast<size_t>(r) * kTileSize * 4u,
                            ras.rgba.data() + static_cast<size_t>(y0 + r) * srcStride +
                                static_cast<size_t>(x0) * 4u,
                            static_cast<size_t>(cols) * 4u);
            }
        }
    }
    l.pixels = builder.publish(l.pixels != nullptr ? l.pixels->revision + 1 : 1);
    t.rasterW = static_cast<double>(ras.width);
    t.rasterH = static_cast<double>(ras.height);
    if (l.mask != nullptr && l.mask->enabled) {
        l.render = composeMasked(l.pixels, *l.mask);
    } else {
        l.render = nullptr;
    }
    t.styleHash = hash;
    return true;
}

int relayoutDocumentText(Document& doc, std::string& firstErr) {
    int n = 0;
    for (Layer& l : doc.layers) {
        if (l.text == nullptr) {
            continue;
        }
        std::string err;
        if (relayoutTextLayer(l, err)) {
            ++n;
        } else if (firstErr.empty()) {
            firstErr = err;
        }
    }
    return n;
}

void rasterizeTextLayerToPixels(Layer& l) {
    l.text = nullptr;  // pixels 保留（此后为普通像素层）
    // render 派生缓存与蒙版语义无关 text，无需重算
}

std::string textLayerDisplayName(const TextLayerState& t) {
    // 首行、按字符截断（UTF-8 感知：不切多字节序列中间）
    const std::string& s = t.content;
    size_t cut = 0;
    size_t chars = 0;
    while (cut < s.size() && chars < 24) {
        const unsigned char c = static_cast<unsigned char>(s[cut]);
        const size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (c == '\n') {
            break;
        }
        cut += len;
        ++chars;
    }
    std::string first = s.substr(0, cut);
    const size_t nl = first.find('\n');
    if (nl != std::string::npos) {
        first = first.substr(0, nl);
    }
    if (first.empty()) {
        return "文本";
    }
    return first;
}

}  // namespace montage
