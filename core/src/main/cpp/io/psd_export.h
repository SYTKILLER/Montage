#ifndef MONTAGE_IO_PSD_EXPORT_H
#define MONTAGE_IO_PSD_EXPORT_H

// M8b PSD 导出（06 规划，用户裁决提前）：自有栈 → PSD 1.x（8-bit RGB）。
// 写端为 psd_reader 逐段结构之逆（记录布局/键表/luni 对拍同文件）。
// exportPsd 为纯函数（快照入参，无 Engine 依赖）——宿主机往返单测可编译（02 §185 路径）。
// v1 能力映射：像素层 RGBA(RLE)/蒙版(-2 通道)/剪贴 flag/24 混合键/不透明度/可见性/luni 中文名；
// M15c（PSD-2）：+ 图层组（lsct divider 1/2=折叠/3=组尾，穿透=pass 键）+ 调整层
// levl/curv/nvrt 参数直写（其余 kind 跳过 + note）+ 文本/填充/样式层栅格化像素导出 + note。
// 空像素层跳过。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace io {

// 锁内拍的导出快照（bridge 构建；字段语义同 Document/Layer/Mask）
struct PsdLayerSnap {
    std::string name;
    bool visible = true;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    bool clipping = false;
    std::shared_ptr<const TileGrid> pixels;   // null = 空层（写出时跳过 + note）
    double originX = 0.0;
    double originY = 0.0;
    std::shared_ptr<const TileGrid> maskGrid;  // patch 网格（R=G=B=gray）
    uint32_t maskW = 0;
    uint32_t maskH = 0;
    int maskOffsetX = 0;
    int maskOffsetY = 0;
    int maskOutside = 255;
    bool maskEnabled = true;
    bool isAdjustment = false;
    // ---- M15c（PSD-2）----
    uint64_t id = 0;          // 引擎 LayerId（组子层挂接用）
    uint64_t parentId = 0;    // 父组引擎 id（0 = 根）
    bool isGroup = false;
    bool collapsed = false;   // lsct type 1=展开 / 2=折叠
    bool isText = false;      // 降级 note（像素已栅格化导出）
    bool isFill = false;      // 降级 note（styledRender 像素导出）
    bool hasStyle = false;    // 降级 note（样式已合成进像素——不可 lrFX 表示的路径）
    std::shared_ptr<const LayerStyle> style;  // M17c 非空 = 效果全可 lrFX 表示（像素走原始+lrFX）
    // 调整层参数（isAdjustment 时；kind 对齐 AdjustmentKind 枚举）
    int32_t adjKind = 0;      // 0=Levels 1=Curves 3=Invert 可直写；其余跳过 + note
    float adjInBlack = 0.0f;
    float adjInWhite = 1.0f;
    float adjGamma = 1.0f;
    std::vector<float> adjCurveX;  // 0..1
    std::vector<float> adjCurveY;
};

struct PsdDocSnap {
    uint32_t width = 0;
    uint32_t height = 0;
    std::string name;
    std::vector<PsdLayerSnap> layers;  // bottom → top
    // 合成图（ImageData 段；null = 导出时从可见层现场扁平化）
    std::shared_ptr<std::vector<uint8_t>> flattenedRgba;
};

struct PsdExportResult {
    bool ok = false;
    std::string error;
    uint64_t bytes = 0;
    uint32_t layers = 0;
    std::vector<std::string> notes;
};

// fd 所有权移交（成功/失败都关闭）。快照由调用方在 docMutex 内构建。
PsdExportResult exportPsd(int fd, const PsdDocSnap& snap);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_PSD_EXPORT_H
