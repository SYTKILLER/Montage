#include "engine/text_raster.h"

// M12 设备实现：OH_Drawing_Typography 全栈排版 → 位图 → 直 alpha 像素。
// API 均经本地 SDK 核验（07 §10 V-08 + 2026-10-11 复核）：CreateTypographyHandler/
// PushTextStyle/AddText/TypographyLayout(maxWidth)/TypographyPaint/BitmapBuild/BitmapGetPixels、
// FontParser 系统字体列表、FontMgr match、RegisterFont 用户字体。
// 颜色字节序：OH_Drawing 颜色 = 0xAARRGGBB；位图声明 PREMUL（Skia 绘制语义），
// 拷出时统一 unpremultiply 还原引擎直 alpha 约定（黑文本为恒等变换）。

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>

#include <hilog/log.h>
#include <native_drawing/drawing_bitmap.h>
#include <native_drawing/drawing_canvas.h>
#include <native_drawing/drawing_color.h>
#include <native_drawing/drawing_font_collection.h>
#include <native_drawing/drawing_font_mgr.h>
#include <native_drawing/drawing_register_font.h>
#include <native_drawing/drawing_text_declaration.h>
#include <native_drawing/drawing_text_typography.h>
#include <native_drawing/drawing_types.h>

namespace montage {

namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.TextRaster";

// 排版栈单例：SharedFontCollection（与 TypographyCreate 共享语义，官方推荐形态）。
// 模拟器实证：Rosen 全局 FontCollection 惰性初始化带内部锁，编辑会话中（ArkUI RichEditor
// 并发排版）触发 Get() 竞态 → appfreeze 6S（THREAD_BLOCK_6S in CreateSktFontCollection）
// 甚至 cppcrash（SEGV in FontCollection::Get）——修复：SharedFontCollection + 引擎侧排版
// 全程互斥 + 渲染线程首帧预热（把惰性初始化挪离编辑期，render_loop 调 warmUpTextRaster）。
std::mutex g_fcMtx;
OH_Drawing_FontCollection* g_fc = nullptr;

OH_Drawing_FontCollection* fontCollection() {
    std::lock_guard<std::mutex> lk(g_fcMtx);
    if (g_fc == nullptr) {
        g_fc = OH_Drawing_CreateSharedFontCollection();
    }
    return g_fc;
}

// 引擎侧排版串行锁（系统全局 FontCollection 状态在模拟器上不容并发触达）
std::mutex g_rasterMtx;

// 字体可用性缓存（族名列表 + 已注册用户字体别名；fontFamilyAvailable/registerUserFont 共用）
std::mutex g_fontListMtx;
std::vector<std::string> g_fontList;
std::set<std::string> g_userFonts;

// 上下各留 2px 抗锯齿出血（GL 采样直取，避免首末行字形被裁）
constexpr uint32_t kPad = 2;

void unpremultiplyRow(uint8_t* px, size_t n) {
    for (size_t i = 0; i < n; i += 4) {
        const unsigned a = px[i + 3];
        if (a == 0 || a == 255) {
            continue;
        }
        // 审查修复：RGB 通道须随行游标 i（原写死 px[0..2]——半透明彩色字边缘保持预乘态
        // 发暗，且首像素被后续所有像素的 alpha 反复相除损坏）
        px[i] = static_cast<uint8_t>((px[i] * 255u + a / 2u) / a);
        px[i + 1] = static_cast<uint8_t>((px[i + 1] * 255u + a / 2u) / a);
        px[i + 2] = static_cast<uint8_t>((px[i + 2] * 255u + a / 2u) / a);
    }
}

}  // namespace

// 首帧预热（渲染线程调用一次）：触发全局 FontCollection 惰性初始化，
// 把初始化开销与竞态窗口挪离编辑会话（ArkUI RichEditor 并发排版实证崩溃）。
void warmUpTextRaster() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    TextLayerState t;
    t.content = "Warm";
    t.fontSize = 24.0;
    TextRasterResult out;
    bool missing = false;
    std::string err;
    rasterizeText(t, 256.0, out, missing, err);
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "text raster warmup done: %{public}du x %{public}du err=%{public}s",
                 out.width, out.height, err.c_str());
}

bool rasterizeText(const TextLayerState& t, double maxWidth, TextRasterResult& out,
                   bool& fontMissing, std::string& err) {
    std::lock_guard<std::mutex> rasterLock(g_rasterMtx);  // 排版栈全局互斥（模拟器竞态实证）
    fontMissing = false;
    out.width = 0;
    out.height = 0;
    out.rgba.clear();
    if (t.content.empty()) {
        return true;  // 空内容：合法空栅格
    }
    // 字体族可用性预检（C-3：缺失回退默认 + 提示；默认族空串跳过）
    if (!t.fontFamily.empty() && !fontFamilyAvailable(t.fontFamily)) {
        fontMissing = true;
    }
    OH_Drawing_TypographyStyle* typoStyle = OH_Drawing_CreateTypographyStyle();
    OH_Drawing_TextStyle* style = OH_Drawing_CreateTextStyle();
    if (typoStyle == nullptr || style == nullptr) {
        err = "create style failed";
        OH_Drawing_DestroyTypographyStyle(typoStyle);
        OH_Drawing_DestroyTextStyle(style);
        return false;
    }
    OH_Drawing_SetTypographyTextDirection(typoStyle, TEXT_DIRECTION_LTR);
    const int aligns[3] = {TEXT_ALIGN_LEFT, TEXT_ALIGN_CENTER, TEXT_ALIGN_RIGHT};
    const int a = (t.align >= 0 && t.align <= 2) ? t.align : 0;
    OH_Drawing_SetTypographyTextAlign(typoStyle, aligns[a]);

    const double fs = std::min(800.0, std::max(4.0, t.fontSize));
    const uint8_t r8 = static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, t.color[0])) * 255.0f + 0.5f);
    const uint8_t g8 = static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, t.color[1])) * 255.0f + 0.5f);
    const uint8_t b8 = static_cast<uint8_t>(std::min(1.0f, std::max(0.0f, t.color[2])) * 255.0f + 0.5f);
    OH_Drawing_SetTextStyleColor(style, (0xFFu << 24) | (static_cast<uint32_t>(r8) << 16) |
                                             (static_cast<uint32_t>(g8) << 8) |
                                             static_cast<uint32_t>(b8));
    OH_Drawing_SetTextStyleFontSize(style, fs);
    // 行距：fontHeight 实测语义 = fontSize 的倍数（SDK 注释未写明；传 48、fs=40 实测行高
    // 1920 = 48×40，2026-10-11 模拟器校准）→ 直传 lineHeight 倍数；halfLeading 上下均分
    OH_Drawing_SetTextStyleFontHeight(style, std::min(4.0, std::max(0.5, t.lineHeight)));
    OH_Drawing_SetTextStyleHalfLeading(style, true);
    // 字距：PS 千分比 em → px
    OH_Drawing_SetTextStyleLetterSpacing(style, fs * t.tracking / 1000.0);
    OH_Drawing_SetTextStyleFontWeight(style, t.bold ? FONT_WEIGHT_700 : FONT_WEIGHT_400);
    OH_Drawing_SetTextStyleFontStyle(style, t.italic ? FONT_STYLE_ITALIC : FONT_STYLE_NORMAL);
    if (!fontMissing && !t.fontFamily.empty()) {
        const char* families[1] = {t.fontFamily.c_str()};
        OH_Drawing_SetTextStyleFontFamilies(style, 1, families);
    }

    OH_Drawing_TypographyCreate* handler =
        OH_Drawing_CreateTypographyHandler(typoStyle, fontCollection());
    OH_Drawing_Typography* typo = nullptr;
    OH_Drawing_Bitmap* bmp = nullptr;
    OH_Drawing_Canvas* canvas = nullptr;
    bool ok = false;
    if (handler != nullptr) {
        OH_Drawing_TypographyHandlerPushTextStyle(handler, style);
        OH_Drawing_TypographyHandlerAddText(handler, t.content.c_str());
        OH_Drawing_TypographyHandlerPopTextStyle(handler);
        typo = OH_Drawing_CreateTypography(handler);
        if (typo != nullptr) {
            OH_Drawing_TypographyLayout(typo, std::max(1.0, maxWidth));
            const double textW = OH_Drawing_TypographyGetLongestLine(typo);
            const double textH = OH_Drawing_TypographyGetHeight(typo);
            // 空排版（纯空白内容）GetLongestLine 可能返回最小 float
            if (std::isfinite(textW) && textW > 0.0 && textH > 0.0) {
                const uint32_t w = static_cast<uint32_t>(std::ceil(textW)) + 2 * kPad;
                const uint32_t h = static_cast<uint32_t>(std::ceil(textH)) + 2 * kPad;
                OH_Drawing_BitmapFormat fmt;
                fmt.colorFormat = COLOR_FORMAT_RGBA_8888;
                fmt.alphaFormat = ALPHA_FORMAT_PREMUL;
                bmp = OH_Drawing_BitmapCreate();
                if (bmp != nullptr) {
                    OH_Drawing_BitmapBuild(bmp, w, h, &fmt);
                    canvas = OH_Drawing_CanvasCreate();
                    if (canvas != nullptr) {
                        OH_Drawing_CanvasBind(canvas, bmp);
                        OH_Drawing_CanvasClear(canvas, 0x00000000);
                        OH_Drawing_TypographyPaint(typo, canvas,
                                                   static_cast<float>(kPad),
                                                   static_cast<float>(kPad));
                        const size_t stride = static_cast<size_t>(w) * 4u;
                        out.rgba.resize(static_cast<size_t>(w) * h * 4u);
                        std::memcpy(out.rgba.data(), OH_Drawing_BitmapGetPixels(bmp),
                                    out.rgba.size());
                        for (uint32_t row = 0; row < h; ++row) {
                            unpremultiplyRow(out.rgba.data() + static_cast<size_t>(row) * stride,
                                             stride);
                        }
                        out.width = w;
                        out.height = h;
                        ok = true;
                    }
                }
            } else {
                ok = true;  // 仅空白：空栅格
            }
        }
    }
    if (canvas != nullptr) {
        OH_Drawing_CanvasDestroy(canvas);
    }
    if (bmp != nullptr) {
        OH_Drawing_BitmapDestroy(bmp);
    }
    if (typo != nullptr) {
        OH_Drawing_DestroyTypography(typo);
    }
    if (handler != nullptr) {
        OH_Drawing_DestroyTypographyHandler(handler);
    }
    OH_Drawing_DestroyTextStyle(style);
    OH_Drawing_DestroyTypographyStyle(typoStyle);
    if (!ok && err.empty()) {
        err = "typography layout/paint failed";
    }
    return ok;
}

bool systemFontFamilies(std::vector<std::string>& out, std::string& err) {
    out.clear();
    // 首选 FontParser 系统字体列表；模拟器实测返回 0 族（配置不可达）→ FontMgr 枚举兜底
    OH_Drawing_FontParser* parser = OH_Drawing_CreateFontParser();
    if (parser != nullptr) {
        size_t num = 0;
        char** list = OH_Drawing_FontParserGetSystemFontList(parser, &num);
        if (list != nullptr) {
            std::set<std::string> uniq;
            for (size_t i = 0; i < num; ++i) {
                if (list[i] != nullptr && list[i][0] != '\0') {
                    uniq.insert(list[i]);
                }
            }
            out.assign(uniq.begin(), uniq.end());
            OH_Drawing_DestroySystemFontList(list, num);
        }
        OH_Drawing_DestroyFontParser(parser);
    }
    if (out.empty()) {
        OH_Drawing_FontMgr* mgr = OH_Drawing_FontMgrCreate();
        if (mgr == nullptr) {
            err = "font parser empty and font mgr create failed";
            return false;
        }
        const int count = OH_Drawing_FontMgrGetFamilyCount(mgr);
        std::set<std::string> uniq;
        for (int i = 0; i < count; ++i) {
            char* name = OH_Drawing_FontMgrGetFamilyName(mgr, i);
            if (name != nullptr && name[0] != '\0') {
                uniq.insert(name);
            }
            OH_Drawing_FontMgrDestroyFamilyName(name);
        }
        OH_Drawing_FontMgrDestroy(mgr);
        out.assign(uniq.begin(), uniq.end());
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "system font families: %{public}zu", out.size());
    return true;
}

bool fontFamilyAvailable(const std::string& family) {
    if (family.empty()) {
        return true;  // 默认字体
    }
    // FontMgrMatchFamily 实测对未知族名回退默认而非返回空（Skia 语义，模拟器实证
    // notes=0 漏报）→ 可用性 = 系统族名列表成员 ∪ 已注册用户字体别名
    std::lock_guard<std::mutex> lk(g_fontListMtx);
    if (g_userFonts.count(family) > 0) {
        return true;
    }
    if (g_fontList.empty()) {
        std::string err;
        systemFontFamilies(g_fontList, err);
    }
    for (const std::string& f : g_fontList) {
        if (f == family) {
            return true;
        }
    }
    return false;
}

bool registerUserFont(const std::string& familyAlias, const std::string& path, std::string& err) {
    if (familyAlias.empty() || path.empty()) {
        err = "registerUserFont: empty alias/path";
        return false;
    }
    const uint32_t rc = OH_Drawing_RegisterFont(fontCollection(), familyAlias.c_str(), path.c_str());
    if (rc == 0) {
        std::lock_guard<std::mutex> lk(g_fontListMtx);
        g_userFonts.insert(familyAlias);
    }
    if (rc != 0) {
        err = "OH_Drawing_RegisterFont rc=" + std::to_string(rc);
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "%{public}s (%{public}s)", err.c_str(),
                     path.c_str());
        return false;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "user font registered: %{public}s <- %{public}s",
                 familyAlias.c_str(), path.c_str());
    return true;
}

}  // namespace montage
