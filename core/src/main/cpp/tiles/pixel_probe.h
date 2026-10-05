#ifndef MONTAGE_TILES_PIXEL_PROBE_H
#define MONTAGE_TILES_PIXEL_PROBE_H

#include <cstdint>
#include <string>

namespace montage {
namespace tiles {

// 单个分配器探针结果（M0：CreateEmptyPixelmapUsingAllocator + AccessPixels 读写 + stride 实测）。
struct ProbeResult {
    std::string allocator;  // "dma" | "shared_memory" | "auto"
    bool createOk = false;
    int32_t createRc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowStride = 0;
    int32_t pixelFormat = 0;
    bool accessOk = false;
    int32_t accessRc = 0;
    bool writeReadOk = false;
    std::string error;
};

// 创建后保留对象的探针句柄（供 NAPI 侧 ConvertPixelmapNativeToNapi 采样）。
struct ProbeHandle {
    bool ok = false;
    int32_t rc = 0;
    void* pixelmap = nullptr;  // OH_PixelmapNative*
    std::string error;
};

ProbeResult ProbeAllocator(int allocatorMode, uint32_t width, uint32_t height);
ProbeHandle ProbeCreateKeep(int allocatorMode, uint32_t width, uint32_t height);
void ReleaseProbeHandle(ProbeHandle& handle);

}  // namespace tiles
}  // namespace montage

#endif  // MONTAGE_TILES_PIXEL_PROBE_H
