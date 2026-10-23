#ifndef MONTAGE_IO_SMART_SOURCE_H
#define MONTAGE_IO_SMART_SOURCE_H

// M21 智能对象栅格源解码入口（轻量头：宿主测试可包含——设备实现在 image_import.cpp，
// 走系统解码器；宿主桩 .debug/m21/sys_stub.cpp）。

#include <cstdint>
#include <string>
#include <vector>

namespace montage {
namespace io {

// 内存栅格源解码（智能对象源；系统解码器整图单趟 + mimeType 驱动 swapRB + 预乘还原）。
// PNG 不走此路径（smart_ops 分发自研确定性解码）。
bool decodeRasterSource(const std::string& fileName, const std::vector<uint8_t>& bytes,
                        uint32_t& w, uint32_t& h, std::vector<uint8_t>& rgba, std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_SMART_SOURCE_H
