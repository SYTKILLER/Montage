#ifndef MONTAGE_IO_PROJECT_SAVE_H
#define MONTAGE_IO_PROJECT_SAVE_H

// M4b 工程保存（04 §1.2 定案）：.montage = zip 包（manifest.json + images/<layerId>.png），
// 格式标识 com.sytkiller.montage.project，版本线自持从 v1 起算。
// fd 所有权移交 native（含失败路径一律关闭）；直接向 fd 流式写出。
// 替换语义：TRUNC 后整包重写（非原子，中断会损坏旧包；原子 rename 归 O4-7 实测后处理）。

#include <cstddef>
#include <cstdint>
#include <string>

namespace montage {
namespace io {

// outLayers/outBytes 可为 nullptr（成功时回填图层数与包体字节数）
bool saveProject(int fd, size_t* outLayers, uint64_t* outBytes, std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_PROJECT_SAVE_H
