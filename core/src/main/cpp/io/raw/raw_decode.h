#ifndef MONTAGE_IO_RAW_RAW_DECODE_H
#define MONTAGE_IO_RAW_RAW_DECODE_H

// M23a RAW 解码内核公共入口（photocraft crates/raw/src/lib.rs 翻译）。
// identify/isRaw 从字节识别 raw 文件（廉价：头与 IFD）；decode 读未显影的
// Sensor 数据与元数据；develop 解码并显影。

#include <optional>
#include <vector>

#include "io/raw/raw_develop.h"
#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

// 从字节识别相机 raw 文件。
std::optional<RawFormat> identify(const uint8_t* bytes, size_t len);
inline std::optional<RawFormat> identify(const std::vector<uint8_t>& bytes) { return identify(bytes.data(), bytes.size()); }

// 字节像不像相机 raw 文件。
inline bool isRaw(const uint8_t* bytes, size_t len) { return identify(bytes, len).has_value(); }
inline bool isRaw(const std::vector<uint8_t>& bytes) { return isRaw(bytes.data(), bytes.size()); }

// 读未显影的传感器数据与元数据。
Sensor decodeRaw(const uint8_t* bytes, size_t len, const Limits& limits);
inline Sensor decodeRaw(const std::vector<uint8_t>& bytes, const Limits& limits) { return decodeRaw(bytes.data(), bytes.size(), limits); }

// 应用匹配的测量相机 profile（Nikon D4 等）/实测图像区（cameras.rs）；decodeTiffEp
// 已内联调用——公开给测试与显影前的再应用。
void applyCameraProfile(Sensor& s);

// 解码并显影。
inline Developed develop(const uint8_t* bytes, size_t len, const DevelopOptions& opts) {
    Sensor s = decodeRaw(bytes, len, opts.limits);
    return developSensor(s, opts);
}
inline Developed develop(const std::vector<uint8_t>& bytes, const DevelopOptions& opts) { return develop(bytes.data(), bytes.size(), opts); }

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_DECODE_H
