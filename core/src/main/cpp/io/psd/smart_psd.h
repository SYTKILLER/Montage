#ifndef MONTAGE_IO_PSD_SMART_PSD_H
#define MONTAGE_IO_PSD_SMART_PSD_H

// PSD 智能对象双向（photocraft io/src/smart_map.rs + linked.rs C++ 翻译子集，MIT OR Apache-2.0）：
// - lnk2/lnk3/lnkD 全局块：内嵌链接文件项（liFD uuid+文件名+原样字节）解析/编码
// - SoLd/SoLE/PlLd 块：soLD descriptor（Idnt uuid / placed 实例 id / Trnf 四角 → 仿射 /
//   Sz 源尺寸 / filterFX 滤镜栈）解析与写出
// - 滤镜映射：PS class ↔ montage FilterKind（13 类，GsnB/boxblur/MtnB/UnsM/AdNs/Mdn/HghP/
//   Mxm/Mnm/Msc/Embs）；未实现滤镜 descriptor 原样 hex 保真（可显隐/删除，导出原样回写）
// - FEid 滤镜缓存 v1 不写（PS 自重算，登记）

#include <cstdint>
#include <string>
#include <vector>

#include "engine/document.h"

namespace montage {
namespace psd {

// lnk2 内嵌链接文件
struct LinkedFile {
    std::string uuid;
    std::string fileName;
    std::vector<uint8_t> bytes;
};

// 解析一个 linked 块数据内的全部 liFD 项（liFE/liFA 外部/别名项跳过）
std::vector<LinkedFile> parseLinkedFiles(const uint8_t* data, size_t n);

// 编码一项 liFD（version 7：无 open descriptor；长度前缀 + 4 字节对齐）
std::vector<uint8_t> encodeLinkedFile(const LinkedFile& f);

// 由字节内容推断 4 字节文件类型（8BPS/PNGf/JPEG/TIFF/GIFf）
std::string linkedFileType(const std::vector<uint8_t>& bytes);

// SoLd/SoLE/PlLd 解析产物（best effort；仿射 = Trnf 四角 × Sz 源框）
struct PlacedSmart {
    std::string idnt;       // 'Idnt'：lnk2 uuid
    std::string placedId;   // 'placed'：实例 id（FEid 键）
    double m[6] = {1, 0, 0, 0, 1, 0};  // 源 → 文档仿射（行主序）
    double srcW = 0.0;      // 'Sz  ' 源尺寸
    double srcH = 0.0;
    // filterFX 栈（hasStack = 块内带 filterFX）
    bool hasStack = false;
    bool stackEnabled = true;
    bool maskEnabled = true;
    bool maskLinked = false;
    std::vector<SmartFilter> filters;
};

// 解析 placed 块（key = "SoLd"/"SoLE"/"PlLd"；data = 块 payload = 'soLD'/'plcL' 起始）。
// 失败返回 false（调用侧降级栅格化）。
bool parsePlaced(const std::string& key, const uint8_t* data, size_t n, PlacedSmart& out);

// 新生成 SoLd 块 payload（'soLD' + version 4 + versioned descriptor + pad4）。
// uuid/placedId 沿用；m/源尺寸出四角；滤镜栈写出（不可表示滤镜返回 false = 调用侧烘焙像素）。
bool soldBytes(const SmartLayerState& sm, double dpi, std::vector<uint8_t>& out);

// 确定性 uuid（contentsId → fnv1a → v4 形态 8-4-4-4-16 hex；Montage 自建 SO 用）
std::string uuidFromContents(uint64_t contentsId);

// 滤镜表示性判定（导出预检：全部滤镜可由 KNOWN 表/psdHex 表示才走 live SoLd）
bool filtersRepresentable(const std::vector<SmartFilter>& filters);

}  // namespace psd
}  // namespace montage

#endif  // MONTAGE_IO_PSD_SMART_PSD_H
