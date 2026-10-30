#ifndef MONTAGE_BRIDGE_PSD_SNAP_H
#define MONTAGE_BRIDGE_PSD_SNAP_H

// P7：PsdDocSnap 快照构建（engine_bridge 持有实现；codec_bridge 分层 TIFF 导出复用）。
// 调用方必须持有 Engine::docMutex（*_Locked 约定）。

#include "io/psd_export.h"

namespace montage {
namespace bridge {

void BuildPsdDocSnapLocked(class Engine& e, io::PsdDocSnap& snap);

}  // namespace bridge
}  // namespace montage

#endif  // MONTAGE_BRIDGE_PSD_SNAP_H
