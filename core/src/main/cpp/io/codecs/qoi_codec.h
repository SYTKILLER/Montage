#ifndef MONTAGE_IO_CODECS_QOI_CODEC_H
#define MONTAGE_IO_CODECS_QOI_CODEC_H

// QOI（Quite OK Image）编解码——Dominic Szablewski 规范（qoif 容器，公共域）。
// 引擎约定 RGBA8 直 alpha（QOI alpha 本身直，零转换）。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

// 解码（bytes 以 "qoif" 开头，调用方嗅探后进入）。失败 err 带原因。
bool qoiDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);

// 编码（colorspace 恒 0=sRGB；数据域语义与引擎一致）。
bool qoiEncode(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_QOI_CODEC_H
