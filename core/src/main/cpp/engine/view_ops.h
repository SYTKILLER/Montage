#ifndef MONTAGE_ENGINE_VIEW_OPS_H
#define MONTAGE_ENGINE_VIEW_OPS_H

// M15a 视图辅助命令：参考线（会话态，非历史事务）+ 图层内容精确包围盒（移动工具吸附用）。
// 参考线不进 History 快照（PS 语义：参考线移动无撤销），持久化经 .montage v9 guides 块。

#include <string>

#include "engine/document.h"

namespace montage {

// orient：0 = 水平参考线（pos = y）、1 = 垂直参考线（pos = x）。pos 须在 [0, 文档边] 内。
bool addGuide(Document& doc, int orient, double pos, std::string& err);
bool moveGuide(Document& doc, int orient, int index, double pos, std::string& err);
bool removeGuide(Document& doc, int orient, int index, std::string& err);
void clearGuides(Document& doc);
void setGuidesLocked(Document& doc, bool locked);
void setGuidesVisible(Document& doc, bool visible);

// 图层视觉包围盒（文档域，精确 alpha 扫描）：像素 = effectivePixels（含蒙版合成/样式派生
// 与其独立 origin）；文本层 = 栅格结果墨迹；调整层/空层/组 = false。吸附与信息面板用。
bool layerContentBBox(const Document& doc, LayerId id,
                      double& x, double& y, double& w, double& h, std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_VIEW_OPS_H
