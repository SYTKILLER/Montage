#ifndef MONTAGE_IO_RAW_RAW_PREVIEW_H
#define MONTAGE_IO_RAW_RAW_PREVIEW_H

// M23c：raw 文件内嵌的相机渲染 JPEG 预览定位。翻译自 photocraft
// crates/raw/src/preview.rs（MIT OR Apache-2.0）。
// 用于两条路：①fit_look 的参考渲染（raw_look）；②不可解码 raw（CR3/PEF/
// 压缩变体）的导入回退（相机 JPEG 当栅格层导入）。JPEG 本体的解码走系统
// ImageSource（image_import），本层只做定位与 DCT 帧尺寸探测。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace montage {
namespace raw {

// 一个内嵌 baseline-JPEG 预览（buffer 内偏移 + 长度；不拷贝）。
struct EmbeddedPreview {
    size_t offset = 0;
    size_t len = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// raw 文件的每个 DCT-JPEG 预览（TIFF 系与 RAF），同一流只取一次
//（相机常存全尺寸与小渲染两份）。
std::vector<EmbeddedPreview> embeddedPreviews(const uint8_t* bytes, size_t len);
inline std::vector<EmbeddedPreview> embeddedPreviews(const std::vector<uint8_t>& bytes) {
    return embeddedPreviews(bytes.data(), bytes.size());
}

// 最大的内嵌预览（面积最大）。
std::optional<EmbeddedPreview> embeddedPreview(const uint8_t* bytes, size_t len);
inline std::optional<EmbeddedPreview> embeddedPreview(const std::vector<uint8_t>& bytes) {
    return embeddedPreview(bytes.data(), bytes.size());
}

// 预览声明的色彩空间（EXIF ColorSpace / InteropIndex；DNG 恒空——其预览是
// 转换器的渲染而非原始拍摄的描述）。
enum class PreviewColorSpace { Srgb, AdobeRgb };
std::optional<PreviewColorSpace> embeddedPreviewColorSpace(const uint8_t* bytes, size_t len);
inline std::optional<PreviewColorSpace> embeddedPreviewColorSpace(const std::vector<uint8_t>& bytes) {
    return embeddedPreviewColorSpace(bytes.data(), bytes.size());
}

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_PREVIEW_H
