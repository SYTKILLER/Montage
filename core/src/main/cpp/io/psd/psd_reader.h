#ifndef MONTAGE_IO_PSD_PSD_READER_H
#define MONTAGE_IO_PSD_PSD_READER_H

// PSD-1 解析器（04 §1.1：上游 PSDReader/PSDChannelCoder/PSDTypes 的 C++ 翻译蓝本，
// 逐段对拍 Adobe 2019 规格实现；PSDText/PSDVector 属 PSD-2 不翻译）。
// 范围：8-bit RGB；栅格图层/组/蒙版 patch/混合键/剪贴标记；文本/矢量/智能对象/效果层
// 按预合成像素导入 + 降级提示（PSDConversionSheet 语义）；调整层（levl/curv/hue 等）跳过+提示。

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace psd {

enum class LayerKind {
    Raster, Group, Adjustment, Text, SmartObject, Effects, Vector, Other,
};

struct Record {
    std::string name;
    bool isGroup = false;
    bool visible = true;
    double opacity = 1.0;
    std::string blendKey = "norm";
    BlendMode blendMode = BlendMode::Normal;
    bool blendExact = true;          // blendKey 精确映射到引擎 4 模式
    bool clipping = false;
    bool croppedToCanvas = false;
    int left = 0;
    int top = 0;
    int width = 0;                   // 0 = 无像素（组/空层）
    int height = 0;
    std::vector<uint8_t> rgba;       // 预乘 RGBA（PSDChannelCoder 语义）
    bool hasMask = false;
    int maskLeft = 0;
    int maskTop = 0;
    int maskW = 0;
    int maskH = 0;
    std::vector<uint8_t> mask;       // 灰度 patch
    uint8_t maskDefault = 255;
    bool maskEnabled = true;
    LayerKind kind = LayerKind::Raster;
    std::vector<std::string> notes;  // 降级提示（逐层）
};

struct PsdDocument {
    int width = 0;
    int height = 0;
    double resolution = 72.0;
    std::vector<Record> layers;      // bottom → top
    std::vector<std::string> notes;  // 全局提示
};

// 混合键映射（对拍 PSDTypes.fromPSD 25 项；Dissolve/Darker/Lighter 上游即落 Normal）
BlendMode blendFromKey(const std::string& key, bool* exact);

// 全量解析（data 内存持有至返回即完成拷贝）。失败返回 false + err。
bool readPsd(const uint8_t* data, size_t length, PsdDocument& out, std::string& err);

}  // namespace psd
}  // namespace montage

#endif  // MONTAGE_IO_PSD_PSD_READER_H
