#ifndef MONTAGE_IO_RAW_RAW_OPCODES_H
#define MONTAGE_IO_RAW_RAW_OPCODES_H

// M23a：DNG opcode 列表解析（DNG 1.7 第 7 章）。
// 翻译自 photocraft crates/raw/src/opcodes.rs（MIT OR Apache-2.0）。
// opcode 列表无论文件字节序一律大端：计数，然后每 opcode 的 ID、DNG 版本、
// flags（bit 0 = 可选）、参数尺寸与参数。只应用 GainMap（ID 9，手机写在
// OpcodeList2 里的镜头阴影校正）；其余报告名字。

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

struct OpcodeListResult {
    std::vector<GainMap> maps;
    std::vector<std::string> skipped;  // 未应用的 opcode 名
};

OpcodeListResult parseOpcodeList(const uint8_t* list, size_t len);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_OPCODES_H
