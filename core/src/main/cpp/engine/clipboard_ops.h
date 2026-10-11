#ifndef MONTAGE_ENGINE_CLIPBOARD_OPS_H
#define MONTAGE_ENGINE_CLIPBOARD_OPS_H

// M10c 剪贴板核（06 规划 §4 M10）：内部剪贴板走引擎内存（规避系统 pasteboard 像素格式坑，
// 登记 v1 不做跨应用）。拷贝 = 选区包围盒域 × 掩码（直 alpha 域 alpha×gray/255）；
// 合并拷贝复用 io/image_export 的 CPU flatten 区域版；粘贴 = 新图层落原偏移；清除 = 选区 × 0。
// 约定：调用方持 docMutex；history 事务由 bridge 层包裹。

#include <cstdint>
#include <vector>

#include "engine/document.h"

namespace montage {

struct ClipBuffer {
    uint32_t width = 0;
    uint32_t height = 0;
    int docX = 0;  // 拷贝时选区包围盒在文档域的落点（粘贴恢复位置）
    int docY = 0;
    std::vector<uint8_t> pixels;  // RGBA 直 alpha，w*h*4
};

// 选区包围盒（文档坐标；无选区/空选区返回 false）
bool selectionBBox(const Document& doc, int& x, int& y, int& w, int& h);

// 拷贝当前层选区内容（merged=true = 全部可见层合成后按掩码裁剪，复用 CPU flatten 区域版）。
// 掩码经羽化等有灰度时 alpha × gray/255（直 alpha 域）。
bool copySelectionToClipboard(const Document& doc, bool merged, ClipBuffer& out);

// 清除：活动层选区内像素 × 0（COW 重写触碰瓦片）。返回是否变更。
bool eraseSelectionPixels(Document& doc);

// 粘贴：剪贴板 → 新图层（insert 在 activeId 之上；origin = clip 落点；空剪贴板返回 0）。
LayerId pasteClipboardAsLayer(Document& doc, const ClipBuffer& clip, LayerId newId);

}  // namespace montage

#endif  // MONTAGE_ENGINE_CLIPBOARD_OPS_H
