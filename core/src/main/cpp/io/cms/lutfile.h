#ifndef MONTAGE_IO_CMS_LUTFILE_H
#define MONTAGE_IO_CMS_LUTFILE_H

// M22 色彩管理内核 · 创作 3D LUT 文件（图像 › 调整 › 颜色查找 的导入格式）：
// `.cube`（Adobe/Resolve）、`.3dl`（Autodesk Lustre/Flame）与 `.look`（SpeedGrade XML），
// 另含少量程序生成的（CC0）预设外观。
// 翻译自 photocraft crates/cms/src/lutfile.rs（MIT OR Apache-2.0）。
// 所有表归一化为 LutFile：size³ 个 RGB 三元组，值 0..=1，红变化最快
//（索引 = ((b·size + g)·size + r)·3）。格式遵循公开描述：Adobe "Cube LUT Specification 1.0"、
// OpenColorIO 文档化的 Lustre 3DL 布局、SpeedGrade .look XML（十六进制小端 float32 三元组）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace montage {
namespace cms {

// 解析出的 3D LUT。
struct LutFile {
    std::string title;
    size_t size = 0;
    std::vector<float> data;  // size³ × 3 值，红最快
    // 表期望的输入域（.cube 的 DOMAIN_MIN/MAX、LUT_3D_INPUT_RANGE）：输入 c 采样于
    // (c − min)/(max − min)，域外饱和。默认 [0,1]。
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};

    // 边长 size 的恒等表。
    static LutFile identity(size_t size);

    // 在 size³ 网格上采样 f。
    template <typename F>
    static LutFile fromFn(const std::string& title, size_t size, F f) {
        const float m = static_cast<float>(size < 2 ? 2 : size - 1);
        LutFile l;
        l.title = title;
        l.size = size;
        l.data.reserve(size * size * size * 3);
        for (size_t b = 0; b < size; ++b) {
            for (size_t g = 0; g < size; ++g) {
                for (size_t r = 0; r < size; ++r) {
                    const float in[3] = {static_cast<float>(r) / m, static_cast<float>(g) / m,
                                         static_cast<float>(b) / m};
                    const auto o = f(std::array<float, 3>{in[0], in[1], in[2]});
                    for (int k = 0; k < 3; ++k) {
                        l.data.push_back(std::min(std::max(o[k], 0.0f), 1.0f));
                    }
                }
            }
        }
        return l;
    }

    // 表是否期望普通 0..1 输入（常见情形；PS 默认）。
    bool domainIsDefault() const {
        for (int k = 0; k < 3; ++k) {
            if (domainMin[k] != 0.0f || domainMax[k] != 1.0f) {
                return false;
            }
        }
        return true;
    }
};

// 接受的最大边长（64³ 是 Photoshop 实际上限；129 留余量同时保持 GPU 表在纹理限制内）。
inline constexpr size_t kLutFileMaxSize = 129;

// 解析 LUT——按 file_name 扩展名选格式（无扩展名按内容嗅探）。失败置 err。
std::optional<LutFile> parseLutFileName(const std::string& fileName, const uint8_t* bytes, size_t len,
                                        std::string& err);
std::optional<LutFile> parseCube(const char* text, size_t len, std::string& err);
std::optional<LutFile> parse3dl(const char* text, size_t len, std::string& err);
std::optional<LutFile> parseLook(const char* text, size_t len, std::string& err);

// 写 .cube 文件（Photoshop 嵌入 Color Lookup 层的格式）。
std::string writeCube(const LutFile& l);

// 内置生成外观（程序化，CC0）：id 与标签。
const std::pair<const char*, const char*>* builtinLookTable(size_t& count);
// 按 id 取内置外观（33³），无则空。
std::optional<LutFile> builtinLook(const std::string& id);

// M22 收口：进程级用户 LUT 槽（导入的 .cube/.3dl/.look；Color Lookup 滤镜
// p0 ≥ 100 经此取表）。槽 0..15，返回空 = 未加载。
std::optional<LutFile> importedLookup(unsigned slot);
// 存入槽（覆盖），返回实际槽号（0..15；满则挤最老——LRU 简化为循环）。
unsigned storeImportedLookup(const LutFile& l);
// 槽占用数（UI 透出）。
unsigned importedLookupCount();

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_LUTFILE_H
