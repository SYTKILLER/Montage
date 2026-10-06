#ifndef MONTAGE_IO_PROJECT_OPEN_H
#define MONTAGE_IO_PROJECT_OPEN_H

// M4c 工程打开（04 §1.2）：.montage zip 包 → manifest 校验（04 §1.3 规则自持）→ 逐层 PNG
// 解码切瓦 → 整档替换 + History 清空（撤销不跨文档）。fd 所有权移交 native（全路径关闭）。

#include <cstddef>
#include <cstdint>
#include <string>

namespace montage {
namespace io {

struct ProjectOpenResult {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t layers = 0;
};

bool openProject(int fd, ProjectOpenResult* out, std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_PROJECT_OPEN_H
