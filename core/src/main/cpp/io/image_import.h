#ifndef MONTAGE_IO_IMAGE_IMPORT_H
#define MONTAGE_IO_IMAGE_IMPORT_H

// 导入管线（02 §6.1 定案）：fd → ImageSource → 分带区域解码（峰值=一个带）→ 切瓦片 →
// 带边界发布 TileGrid + 进度事件 → 完成后 bump docVersion。T3 引擎工作线程执行。

#include <cstdint>

#include <napi/native_api.h>

namespace montage {
namespace io {

// 导入完成 payload（堆分配，TSFN 送 JS 线程消费后由消费方 delete）
struct ImportCompletion {
    bool ok = false;
    uint32_t width = 0;
    uint32_t height = 0;
    char error[192] = {0};
};

// 异步启动导入（新建线程）。完成/失败经 completionTsfn 送 JS 线程 resolve Promise（payload=
// ImportCompletion*，消费方 delete）；fd 所有权一律移交本函数（结束时 close，含失败路径）。
// 返回 false = 未启动（已有导入在跑），此时 fd 同样被关闭。
bool startImageImport(int fd, napi_threadsafe_function completionTsfn);

// 请求取消（带边界生效）；不等待线程退出。
void cancelImageImport();

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_IMAGE_IMPORT_H
