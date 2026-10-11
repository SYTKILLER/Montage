#ifndef MONTAGE_ENGINE_TEXT_RASTER_H
#define MONTAGE_ENGINE_TEXT_RASTER_H

// M12 文字栅格化接缝：排版→像素的独立函数面（text_ops 只依赖本头文件）。
// 设备实现 = text_raster_oh.cpp（OH_Drawing_Typography 全栈）；宿主机单测以同名
// 桩实现替换（.debug/m12/test_text.cpp），测试排版外围逻辑（指纹/网格发布/栅格化转层）。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {

// 一次栅格化输出：整幅直 alpha RGBA8888（行距 = width*4），空内容时 width=height=0。
struct TextRasterResult {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

// 排版并栅格化文本（maxWidth = 换行约束文档 px；point 文本传大值）。字体缺失时回退
// 系统默认并置 fontMissing=true（C-3 语义：不崩溃不空层）。
bool rasterizeText(const TextLayerState& t, double maxWidth, TextRasterResult& out,
                   bool& fontMissing, std::string& err);

// 系统字体族名列表（去重排序；FontParser 系统字体列表）
bool systemFontFamilies(std::vector<std::string>& out, std::string& err);

// 字体族是否可解析（FontMgr match；空族名恒 true = 默认字体）
bool fontFamilyAvailable(const std::string& family);

// 注册用户字体文件（familyAlias = 工程内引用名；path = 沙箱内 ttf/otf 绝对路径）。
// 注册后 rasterizeText 按该别名解析（FontCollection 全局单例）。
bool registerUserFont(const std::string& familyAlias, const std::string& path, std::string& err);

// 首帧预热：触发系统排版栈/全局 FontCollection 惰性初始化（渲染线程调用一次；
// 模拟器实证编辑会话中首触会撞 ArkUI 排版并发 → 6S 锁等待甚至竞态崩溃）。
void warmUpTextRaster();

}  // namespace montage

#endif  // MONTAGE_ENGINE_TEXT_RASTER_H
