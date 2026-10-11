#ifndef MONTAGE_ENGINE_TEXT_OPS_H
#define MONTAGE_ENGINE_TEXT_OPS_H

// M12 文字层操作（排版缓存/发布为瓦片网格/栅格化转普通层）。栅格化本体走
// text_raster.h 接缝（设备 = OH_Drawing；宿主单测 = 桩）。

#include <string>

#include "engine/document.h"

namespace montage {

// 文本参数指纹（relayout 缓存键：内容+全部样式字段；网格尺寸派生不单列）
uint64_t textStyleHash(const TextLayerState& t);

// 重排栅格化一个文本层（前置 l.text != nullptr）：指纹未变且有像素 → 直接 true（缓存）；
// 否则调 rasterizeText → 发布瓦片网格（行距 width*4 逐行拷入 256 瓦片）→ 更新
// l.pixels/l.text->styleHash → 蒙版启用时重合成 render。空内容 → pixels = null。
bool relayoutTextLayer(Layer& l, std::string& err);

// 文档内全部文本层重栅格（工程打开/字体注册后调用；返回处理层数）。
int relayoutDocumentText(Document& doc, std::string& firstErr);

// 文本层 → 普通像素层（栅格化命令核）：丢弃 text 状态，pixels 保留。前置 l.text != nullptr。
void rasterizeTextLayerToPixels(Layer& l);

// 图层名取内容首行（≤24 字符，PS 文本层自动命名语义）；空内容回退默认名。
std::string textLayerDisplayName(const TextLayerState& t);

}  // namespace montage

#endif  // MONTAGE_ENGINE_TEXT_OPS_H
