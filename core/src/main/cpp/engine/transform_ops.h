#ifndef MONTAGE_ENGINE_TRANSFORM_OPS_H
#define MONTAGE_ENGINE_TRANSFORM_OPS_H

// M8a 无损文档操作（06 规划 §4）：全部为几何/元数据变换，零重采样，撤销 = 快照互换。
// 约定：所有操作要求调用方持 docMutex；history 事务由 bridge 层 beginEdit/endEdit 包裹。

#include "engine/document.h"

namespace montage {

// 可见层内容包围盒并集（doc 坐标，含 origin 偏移；无可见内容返回 false）
bool visibleContentBBox(const Document& doc, int& x, int& y, int& w, int& h);

// 裁剪/画布大小：文档域重设 + 全层 origin 平移（选区清除，会话态语义）。
// 坐标可为负（扩展画布）。返回是否变更。
bool resizeCanvas(Document& doc, int x, int y, int w, int h);

// 水平/垂直翻转（像素精确重排 + origin/蒙版 offset 重映射）
bool flipDocument(Document& doc, bool horizontal);

// 90° 旋转（顺时针/逆时针；文档域 w/h 交换）
bool rotateDocument90(Document& doc, bool clockwise);

// 选区掩码平移（出界裁剪；无选区返回 false）
bool translateSelection(Document& doc, int dx, int dy);

// M14b 对齐/分布（多选层；纯 origin 移动，像素零改动，一步事务由调用方包裹）。
// mode：0=左 1=水平居中 2=右 3=顶 4=垂直居中 5=底。组/调整层/空层跳过；
// 对齐需 ≥2 可用层（对齐到选中集包围盒），分布需 ≥3（选定边/中心等距插值，首尾锚定）。
bool alignLayers(Document& doc, const std::vector<LayerId>& ids, int mode, std::string& err);
bool distributeLayers(Document& doc, const std::vector<LayerId>& ids, int mode, std::string& err);

}  // namespace montage

#endif  // MONTAGE_ENGINE_TRANSFORM_OPS_H
