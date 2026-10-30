#ifndef MONTAGE_IO_CODECS_CODEC_IMPORT_H
#define MONTAGE_IO_CODECS_CODEC_IMPORT_H

// M25 编解码导入的文档级入口（引擎侧；decodeLayers/decode 的字节域入口见 codec_table.h）。
// openTiffFile 桥接命令走这里（分层→PSD 通路整档替换；扁平→合成图单层）。

#include <memory>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace io {

// RGBA8 直 alpha 缓冲 → TileGrid（行优先铺瓦；宽高零/超限返回 null）
std::shared_ptr<const TileGrid> gridFromRgba(uint32_t w, uint32_t h,
                                             const std::vector<uint8_t>& rgba);

// TIFF 文档打开（PSD 同语义：resetDocument + 整档替换）。fd 所有权移交（完成即关闭）。
// 分层 ImageSourceData → psd_reader 通路（组/蒙版/混合键/不透明度全保留）；
// 扁平/分层解析失败 → 合成图单层「Background」。
// 成功返回 true；失败 err 带原因（fd 同样已关闭）。
bool importTiffDocument(int fd, int* outWidth, int* outHeight, std::vector<std::string>* notes,
                        std::string& err);

// P7：GIF 文档打开。单帧 → 单层（旧语义）；多帧 → 帧序列图层化
//（bottom→top = 帧 1..N，PS 打开动画 GIF 同构；delay 不入 v1 档，登记）。
bool importGifDocument(int fd, int* outWidth, int* outHeight, std::vector<std::string>* notes,
                       std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_CODECS_CODEC_IMPORT_H
