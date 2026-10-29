#ifndef MONTAGE_IO_PSD_PSD_DEEP_H
#define MONTAGE_IO_PSD_PSD_DEEP_H

// M22 深色/多模式 PSD 读入内核（「存储与往返先行」——渲染仍 8-bit，不动 TileGrid）。
// 平面深度扩展（1/16/32 位）、大端样本、模式交错（Gray/Indexed/Bitmap/Duotone/CMYK/Lab）
// 与导出侧重编码。平面转换对拍 photocraft psd/src/pixels.rs（MIT OR Apache-2.0）；
// CMYK/Lab 的颜色转换走 io/cms 色彩管理核（photocraft pixels.rs 的 naive 路径升级为
// 受管——相对色度 + BPC，与合成器同语义）。

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace montage {
namespace psd {

// 每行字节数（depth 1 = 位行）。
size_t deepRowBytes(size_t width, uint16_t depth);

// 位平面展开（MSB first，行补齐到字节）；map = 位值 → 输出字节。
std::vector<uint8_t> unpackBitsPlane(const uint8_t* data, size_t len, size_t width, size_t height,
                                     uint8_t (*map)(uint8_t));

// 平面样本 → 8-bit（photocraft planeToU8 对拍）：
// depth 1：bit=1 → 0（黑）、bit=0 → 255（白）；8：原样；16：大端 (v*255+32767)/65535；
// 32：大端 f32，NaN → 0，clamp 0..1 后四舍五入。不支持深度返回空。
std::optional<std::vector<uint8_t>> planeToU8(const uint8_t* data, size_t len, uint16_t depth, size_t width,
                                              size_t height, std::string& err);
// P6 D1-b：深度平面 → f32 归一化平面（16 位大端 u16 /65535；32 位大端 IEEE f32，NaN→0；
// 值域不钳——HDR 负值/超 1 由消费端语义决定；仅 16/32，1/8 位走 planeToU8）。
std::optional<std::vector<float>> planeToF32(const uint8_t* data, size_t len, uint16_t depth, size_t width,
                                             size_t height, std::string& err);

// 通道压缩段 → 原始字节平面（宽 × 高 × bps/8）。compression 0 = raw、1 = RLE
//（行计数 u16，PSB u32；行距 = deepRowBytes）。crop 为空 = 整面；
// 否则 {x, y, w, h}（采样粒度）——RLE 仍整面解后裁剪列带。
std::optional<std::vector<uint8_t>> decodeDeepChannel(int compression, size_t width, size_t height,
                                                      uint16_t depth, bool largeDoc, const uint8_t* payload,
                                                      size_t len, const int* crop, std::string& err);

// 8-bit 平面裁剪（采样粒度 {x,y,w,h}）。
std::vector<uint8_t> cropPlane8(const std::vector<uint8_t>& plane, size_t width, size_t height, int x, int y,
                                int w, int h);

// 模式交错 → RGBA8 直 alpha（photocraft interleave 的 M22 扩展）。
// colorPlanes = 已转 8-bit 的色彩平面（按模式语义序：RGB=R,G,B；Gray/Bitmap/Duotone/Indexed
// = 单平面；CMYK = C,M,Y,K 存储域（反相值，经 io/cms 受管转 sRGB）；Lab = L,a,b）。
// alpha = 透明通道（可空）；palette = indexed 调色板（256×3 平面序 R,G,B；可空）。
// 多通道（mode 7）与未知模式返回空。
std::optional<std::vector<uint8_t>> interleaveToRgba(uint16_t mode,
                                                     const std::vector<std::vector<uint8_t>>& colorPlanes,
                                                     const std::vector<uint8_t>* alpha, const uint8_t* palette,
                                                     size_t n, std::string& err);

// 模式的色彩通道数（不含 alpha）。
size_t deepColorChannels(uint16_t mode);

// 导出方向：RGBA8 直 alpha → 模式的 8-bit 色彩平面（含 alpha 平面在尾）。
// CMYK 经 io/cms 受管反变换；Lab 经 srgbToLab；Gray = Rec.601 亮度；Indexed =
// 最近调色板项（欧氏 RGB）。mode 0/7/8 或未知返回空（导出降级 8-bit RGB + note）。
std::optional<std::vector<std::vector<uint8_t>>> rgbaToModePlanes(uint16_t mode, const uint8_t* rgba, size_t n,
                                                                  const uint8_t* palette);

// 8-bit 平面 → 目标深度大端样本：16 = (v*65535+127)/255；32 = v/255 float；1 = 阈值 128
// 位打包（MSB first，行补齐）。8 = 原样。
std::vector<uint8_t> planeToDepth(const std::vector<uint8_t>& plane8, uint16_t depth, size_t width,
                                  size_t height);

// RGBA8 直 alpha → 单通道 8-bit 灰度蒙版式平面（导出 Gray/Lab/CMYK 的公共中间步不适用，
// 供 1-bit Bitmap 导出的阈值前平面用）。
std::vector<uint8_t> rgbaToGrayPlane(const uint8_t* rgba, size_t n);

}  // namespace psd
}  // namespace montage

#endif  // MONTAGE_IO_PSD_PSD_DEEP_H
