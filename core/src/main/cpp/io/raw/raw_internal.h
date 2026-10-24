#ifndef MONTAGE_IO_RAW_RAW_INTERNAL_H
#define MONTAGE_IO_RAW_RAW_INTERNAL_H

// M23a 内部入口声明：各容器解码器（.cpp 单件实现，翻译自 photocraft 同名 .rs）。

#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

// dng.rs
Sensor decodeDng(const Tiff& t, const Limits& limits);
// cr2.rs（同时提供 clipLevel/maskedBlack 等供 tiffep/raf 复用）
Sensor decodeCr2(const Tiff& t, const Limits& limits);
float clipLevel(const std::vector<uint16_t>& data, uint32_t precision);
// tiffep.rs
Sensor decodeTiffEp(const Tiff& t, RawFormat format, const Limits& limits);
// nefc.rs（compression 34713）——读 Huffman 压缩平面（maker note 解码表自取）
constexpr uint32_t kNikonCompression = 34713;
constexpr uint16_t kNikonLinearizationTable = 0x0096;
Plane readNefcCompressed(const Tiff& t, const Ifd& rawIfd, const Limits& limits);
// sony.rs（cRAW）
constexpr uint16_t kSonyRawFileType = 0x7000;
constexpr uint16_t kSonyToneCurve = 0x7010;
bool isCr(const Tiff& t, const Ifd& ifd);  // is_craw
Plane readSonyCr(const Tiff& t, const Ifd& rawIfd, const Limits& limits);
// rw2.rs
Sensor decodeRw2(const Tiff& t, const Limits& limits);
// orf.rs
Sensor decodeOrf(const Tiff& t, const Limits& limits);
// raf.rs（入口收整个文件字节）
Sensor decodeRaf(const uint8_t* data, size_t len, const Limits& limits);
constexpr uint8_t kRafMagic[] = "FUJIFILMCCD-RAW";
// cameras.rs
void applyCameraProfile(Sensor& s);

// tiffep.rs 辅助（orf 复用）
std::optional<Cfa> tiffEpCfa(const Tiff& t, const Ifd& raw, const std::vector<Ifd>& ifds);
std::optional<std::array<float, 4>> rggbByPosition(const Cfa& cfa, const std::vector<double>& v);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_INTERNAL_H
