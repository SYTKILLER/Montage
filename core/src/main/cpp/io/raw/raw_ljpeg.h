#ifndef MONTAGE_IO_RAW_RAW_LJPEG_H
#define MONTAGE_IO_RAW_RAW_LJPEG_H

// M23a：无损 JPEG 解码器（ITU-T T.81 / ISO 10918-1，process 14 "LJ92"：
// Huffman 编码、预测、顺序、2–16 位样本）。
// 翻译自 photocraft crates/raw/src/ljpeg.rs（MIT OR Apache-2.0），
// 该实现按 T.81 规范净室编写（Annex C Huffman 表、F.2.2.1-3 差分类别解码、
// Annex H 无损模式：预测器 1–7、点变换、首行/首列规则、重启间隔）。
// 输出为光栅序样本序列：`height` 行 × `width × components` 交错样本。
// 只支持单个交错 scan 持全部组件、采样因子 1×1——DNG/CR2/TIFF 系 raw 均如此使用。

#include <cstdint>
#include <vector>

#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

// 帧头（SOF3）。
struct LjpegFrame {
    uint8_t precision = 0;
    size_t width = 0, height = 0, components = 0;

    std::optional<size_t> samples() const {
        if (width > SIZE_MAX / height) return std::nullopt;
        size_t wh = width * height;
        if (wh > SIZE_MAX / components) return std::nullopt;
        return wh * components;
    }
};

// 只读帧头不解码。
LjpegFrame ljpegFrame(const uint8_t* data, size_t len);

// 解码无损 JPEG 流，返回 (帧, 样本)。`maxSamples` 限制输出尺寸，分配前检查。
std::pair<LjpegFrame, std::vector<uint16_t>> ljpegDecode(const uint8_t* data, size_t len, size_t maxSamples);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_LJPEG_H
