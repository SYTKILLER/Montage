#include "tiles/pixel_probe.h"

#include <multimedia/image_framework/image/image_common.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

namespace montage {
namespace tiles {
namespace {

void fillOptions(OH_Pixelmap_InitializationOptions* opts, uint32_t width, uint32_t height) {
    OH_PixelmapInitializationOptions_SetWidth(opts, width);
    OH_PixelmapInitializationOptions_SetHeight(opts, height);
    OH_PixelmapInitializationOptions_SetPixelFormat(opts, PIXEL_FORMAT_RGBA_8888);
}

const char* allocatorName(int allocatorMode) {
    if (allocatorMode == IMAGE_ALLOCATOR_MODE_DMA) {
        return "dma";
    }
    if (allocatorMode == IMAGE_ALLOCATOR_MODE_SHARED_MEMORY) {
        return "shared_memory";
    }
    return "auto";
}

void readInfo(OH_PixelmapNative* pm, ProbeResult* r) {
    OH_Pixelmap_ImageInfo* info = nullptr;
    if (OH_PixelmapImageInfo_Create(&info) != IMAGE_SUCCESS || info == nullptr) {
        r->error = "ImageInfo create failed";
        return;
    }
    if (OH_PixelmapNative_GetImageInfo(pm, info) == IMAGE_SUCCESS) {
        OH_PixelmapImageInfo_GetWidth(info, &r->width);
        OH_PixelmapImageInfo_GetHeight(info, &r->height);
        OH_PixelmapImageInfo_GetRowStride(info, &r->rowStride);
        int32_t fmt = 0;
        OH_PixelmapImageInfo_GetPixelFormat(info, &fmt);
        r->pixelFormat = fmt;
    }
    OH_PixelmapImageInfo_Release(info);
}

}  // namespace

ProbeResult ProbeAllocator(int allocatorMode, uint32_t width, uint32_t height) {
    ProbeResult r;
    r.allocator = allocatorName(allocatorMode);

    OH_Pixelmap_InitializationOptions* opts = nullptr;
    OH_PixelmapInitializationOptions_Create(&opts);
    if (opts == nullptr) {
        r.error = "InitializationOptions create failed";
        return r;
    }
    fillOptions(opts, width, height);

    OH_PixelmapNative* pm = nullptr;
    r.createRc = OH_PixelmapNative_CreateEmptyPixelmapUsingAllocator(
        opts, static_cast<IMAGE_ALLOCATOR_MODE>(allocatorMode), &pm);
    OH_PixelmapInitializationOptions_Release(opts);
    if (r.createRc != IMAGE_SUCCESS || pm == nullptr) {
        r.error = "create rc=" + std::to_string(r.createRc);
        return r;
    }
    r.createOk = true;

    readInfo(pm, &r);

    void* addr = nullptr;
    r.accessRc = OH_PixelmapNative_AccessPixels(pm, &addr);
    if (r.accessRc != IMAGE_SUCCESS || addr == nullptr) {
        r.error = "access rc=" + std::to_string(r.accessRc);
        OH_PixelmapNative_Release(pm);
        return r;
    }
    r.accessOk = true;

    const uint32_t stride = r.rowStride > 0 ? r.rowStride : width * 4u;
    const size_t lastRow = static_cast<size_t>(stride) * (height - 1u);
    const size_t lastPx = lastRow + (static_cast<size_t>(width) - 1u) * 4u;

    uint8_t* p = static_cast<uint8_t*>(addr);
    p[0] = 0x10;
    p[1] = 0x20;
    p[2] = 0x30;
    p[3] = 0xFF;
    p[lastRow] = 0xAA;
    p[lastRow + 1] = 0xBB;
    p[lastRow + 2] = 0xCC;
    p[lastRow + 3] = 0x55;
    p[lastPx] = 0x7F;
    p[lastPx + 3] = 0x11;
    OH_PixelmapNative_UnaccessPixels(pm);

    void* addr2 = nullptr;
    if (OH_PixelmapNative_AccessPixels(pm, &addr2) == IMAGE_SUCCESS && addr2 != nullptr) {
        const uint8_t* q = static_cast<const uint8_t*>(addr2);
        r.writeReadOk = q[0] == 0x10 && q[1] == 0x20 && q[2] == 0x30 && q[3] == 0xFF &&
                        q[lastRow] == 0xAA && q[lastRow + 1] == 0xBB && q[lastRow + 2] == 0xCC &&
                        q[lastRow + 3] == 0x55 && q[lastPx] == 0x7F && q[lastPx + 3] == 0x11;
        OH_PixelmapNative_UnaccessPixels(pm);
    } else {
        r.error = "second access failed";
    }

    OH_PixelmapNative_Release(pm);
    return r;
}

ProbeHandle ProbeCreateKeep(int allocatorMode, uint32_t width, uint32_t height) {
    ProbeHandle h;
    h.ok = false;
    OH_Pixelmap_InitializationOptions* opts = nullptr;
    OH_PixelmapInitializationOptions_Create(&opts);
    if (opts == nullptr) {
        h.error = "InitializationOptions create failed";
        return h;
    }
    fillOptions(opts, width, height);
    OH_PixelmapNative* pm = nullptr;
    h.rc = OH_PixelmapNative_CreateEmptyPixelmapUsingAllocator(
        opts, static_cast<IMAGE_ALLOCATOR_MODE>(allocatorMode), &pm);
    OH_PixelmapInitializationOptions_Release(opts);
    if (h.rc != IMAGE_SUCCESS || pm == nullptr) {
        h.error = "create rc=" + std::to_string(h.rc);
        return h;
    }
    h.ok = true;
    h.pixelmap = pm;
    return h;
}

void ReleaseProbeHandle(ProbeHandle& handle) {
    if (handle.pixelmap != nullptr) {
        OH_PixelmapNative_Release(static_cast<OH_PixelmapNative*>(handle.pixelmap));
        handle.pixelmap = nullptr;
    }
}

}  // namespace tiles
}  // namespace montage
