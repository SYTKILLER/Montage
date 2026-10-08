#ifndef MONTAGE_ENGINE_RESAMPLE_H
#define MONTAGE_ENGINE_RESAMPLE_H

// M8c/M8d 重采样核（06 规划 §4）：仿射 warp（双线性，出界透明）为图像大小/任意角旋转/
// 自由变换烘焙共用。全部锁外执行（调用方持 docMutex 完成组装与回写）。

#include "engine/document.h"

namespace montage {

// 仿射变换（正向：dst = M·src，m = [a b c; d e f]，即 x'=a·x+b·y+c, y'=d·x+e·y+f）。
// dst 网格 dstCols×dstRows；dst 像素经逆矩阵映射回 src 采样（双线性；边缘 clamp；
// src 出界 → 透明）。透明跳过保持稀疏。revision = src.revision+1。
std::shared_ptr<const TileGrid> warpGridAffine(const TileGrid& src, const double m[6],
                                               uint32_t dstCols, uint32_t dstRows);

// 图像大小：整档等比/非等比缩放（层 origin、像素、蒙版 patch 及 offset 同步缩放；
// 选区清除）。返回是否变更。
bool resampleDocument(Document& doc, uint32_t newW, uint32_t newH);

// 任意角旋转（顺时针度数；文档域扩至旋转包围盒；层像素/蒙版经仿射核重采样）。
bool rotateDocumentArbitrary(Document& doc, double degreesCw);

// M8d 自由变换烘焙：围绕枢轴的 缩放/旋转/斜切 一次性仿射（提交式，PS 普通层语义）。
// 层网格 warp 到新包围盒；origin/蒙版同步；history 事务由 bridge 包裹。
bool bakeLayerTransform(Document& doc, LayerId id, double scaleX, double scaleY,
                        double rotDegCw, double skewDeg, double pivotX, double pivotY);

// M8.1 矩阵版烘焙（overlay 状态机的任意 2x3 仿射；行主序 x'=m0·x+m1·y+m2）。
bool bakeLayerMatrix(Document& doc, LayerId id, const double m[6]);

}  // namespace montage

#endif  // MONTAGE_ENGINE_RESAMPLE_H
