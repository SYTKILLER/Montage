#ifndef MONTAGE_IO_PSDSRC_BLOB_H
#define MONTAGE_IO_PSDSRC_BLOB_H

// M22 .montage v14：深色 PSD 层 verbatim 元数据自包含 blob（images/psdsrc_<id>.bin）。
// 布局（大端）：
//   u32 magic 'PSRC' | u16 version=1 | u16 depth | u16 mode | u16 channels
//   u8 cropped | u8 reserved×3 | u64 importHash
//   i32 srcLeft/srcTop/srcRight/srcBottom | i32 srcMaskLeft/srcMaskTop/srcMaskRight/srcMaskBottom
//   u16 channelCount | per channel: i16 id + u64 len
//   section 字节（全部通道 [compression u16 + payload] 原样）

#include <cstdint>
#include <string>
#include <vector>

namespace montage {
namespace io {

struct PsdSrcMeta {
    uint16_t depth = 8;
    uint16_t mode = 3;
    uint16_t channels = 3;
    bool cropped = false;
    uint64_t importHash = 0;
    int srcLeft = 0, srcTop = 0, srcRight = 0, srcBottom = 0;
    int srcMaskLeft = 0, srcMaskTop = 0, srcMaskRight = 0, srcMaskBottom = 0;
    std::vector<std::pair<int16_t, int64_t>> channelTable;
    std::vector<uint8_t> section;
};

std::vector<uint8_t> encodePsdSrcBlob(const PsdSrcMeta& m);
bool decodePsdSrcBlob(const uint8_t* data, size_t len, PsdSrcMeta& out, std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_PSDSRC_BLOB_H
