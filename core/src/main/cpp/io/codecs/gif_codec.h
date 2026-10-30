#ifndef MONTAGE_IO_CODECS_GIF_CODEC_H
#define MONTAGE_IO_CODECS_GIF_CODEC_H

// GIF 编解码（GIF87a/89a：LZW、GCT/LCT、interlace、GCE 透明）。读首帧/写单帧 +
// P7 多帧：gifDecodeFrames 全画布合成帧序列（disposal 0/1 保留、2 恢复透明、3 v1 按 2）、
// gifEncodeFrames 多帧动画（NETSCAPE 循环扩展 + 逐帧 GCE/LCT）。
// 量化 = codec_util medianCut + Floyd–Steinberg（可关）。

#include <cstdint>
#include <string>
#include <vector>

#include "io/codecs/codec_util.h"

namespace montage {
namespace io {
namespace codecs {

bool gifDecode(const uint8_t* data, size_t len, CodecImage& out, std::string& err);

// dither=false 关闭 FS 抖动；maxColors ≤ 256（透明占用一个索引）
bool gifEncode(const uint8_t* rgba, uint32_t w, uint32_t h, int maxColors, bool dither,
               std::vector<uint8_t>& out, std::string& err);

// P7：动画帧（全画布合成 RGBA + GCE delay 百分秒）
struct GifFrameRec {
    CodecImage img;
    uint16_t delayCs = 0;
};

// 多帧解码：帧序列按屏幕尺寸全画布合成（screen 为 0 时取首帧尺寸）。
bool gifDecodeFrames(const uint8_t* data, size_t len, std::vector<GifFrameRec>& out,
                     std::string& err);

// 多帧编码：frames 为全画布 RGBA（w×h×4）且与 delaysCs 等长；NETSCAPE2.0 循环扩展 +
// 逐帧 GCE（delay/透明）+ 逐帧 LCT 量化。
bool gifEncodeFrames(const std::vector<const uint8_t*>& frames, uint32_t w, uint32_t h,
                     const std::vector<uint16_t>& delaysCs, int maxColors, bool dither,
                     std::vector<uint8_t>& out, std::string& err);

}  // namespace codecs
}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_GIF_CODEC_H
