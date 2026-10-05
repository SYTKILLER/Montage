#ifndef MONTAGE_IO_PSD_PSD_IMPORT_H
#define MONTAGE_IO_PSD_PSD_IMPORT_H

// PSD → 引擎文档烘焙（PSD-1，M2 引擎能力边界内的降级策略）：
// - 蒙版：patch 按文档位置贴到图层网格（其余填 maskDefault）→ 乘进图层像素（烘焙）+ 提示
// - 剪贴：clip 层像素 × base 层 alpha（文档位置对齐，烘焙）+ 提示
// - 组：M2 无组层级 → 面板保留记录 + "组扁平化"提示（pass-through 渲染语义等价）
// - 文本/矢量/智能对象/效果：按预合成像素导入 + 提示；调整层跳过 + 提示
// - 混合键：引擎 4 模式精确映射，其余落 Normal + 提示（对齐上游落 Normal 并报告的策略）

#include <string>
#include <vector>

namespace montage {
namespace io {

// 同步执行（调用方负责放工作线程）。fd 所有权移交（完成即关闭）。
// 成功时引擎文档已被替换（多图层入栈），notes 返回降级提示清单。
bool importPsd(int fd, int* outWidth, int* outHeight, std::vector<std::string>* notes,
               std::string& err);

}  // namespace io
}  // namespace montage

#endif  // MONTAGE_IO_PSD_PSD_IMPORT_H
