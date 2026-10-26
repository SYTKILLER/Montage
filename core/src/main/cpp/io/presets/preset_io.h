#ifndef MONTAGE_IO_PRESETS_PRESET_IO_H
#define MONTAGE_IO_PRESETS_PRESET_IO_H

// P1 预设库资源导入内核：Photoshop 资源格式解析（photocraft crates/psd C++ 翻译子集，MIT
// OR Apache-2.0 干净房对拍）：
// - .aco/.ase 色板（aco.rs/ase.rs）：v1+v2 双节 / ASEF 块结构；非 RGB 空间 naive 转 sRGB
//   （photocraft color/convert.rs 同式：HSB 圆柱、CMYK (1−c)(1−k)、Lab D50→sRGB Bradford、
//   Gray = 1 − level/10000（aco 存墨量）/ ase Gray 0..1 直用（1 = 白））。
// - .grd 渐变（grd.rs）：8BGR v5 版本化 ActionDescriptor，GrdL→Grdn→Grad（Clrs 色标 +
//   Trns 透明度标）；noise（ClNs）跳过计数不报错。
// - .pat 图案（patterns.rs parse_pat_file 路线）：8BPT v1；8/16/32 位、Gray/Indexed/RGB/CMYK
//   模式 → RGBA8（16 位取高字节、32 位 float 钳制）；PackBits/RAW 行压缩；VMA 通道矩形
//   偏移放置。v1 上限：边 ≤4096（预设库容量同源）、1 位深度明确报错。
// - .abr 笔刷（abr.rs + io/abr_map.rs 简化映射）：v1/v2 legacy（computed/sample tip）+ v6+
//   分节（samp 位图/desc 描述符预设）；phry 层级忽略（组 = 文件名，登记）；纹理/双笔刷/
//   动态等无对应参数的键进 warnings 不静默丢弃。
// 写出：aco/ase/grd/pat 四格式（round-trip oracle 与导出用；v1 grd 仅 RGB 停）。
// 全部入口 bounds-checked 永不 panic：截断/坏签名/超限 = 报错返回 false，err 带原因。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace montage {
namespace presets {

// 单文件字节上限（photocraft preset_import_cmds MAX_FILE_BYTES 同值）
constexpr size_t kPresetFileMaxBytes = static_cast<size_t>(512) << 20;
// 图案单边上限（与 engine/preset_lib 容量同源；photocraft MAX_EDGE=30000 收紧登记）
constexpr uint32_t kPatMaxEdge = 4096;
// .abr 采样位图单边上限（photocraft abr.rs MAX_EDGE 同值）
constexpr uint32_t kAbrMaxEdge = 8192;
// 色板单文件上限（.ase；.aco 计数是 u16 天然受限）
constexpr size_t kMaxSwatches = 100000;

// ---------- 色板（.aco/.ase） ----------

// 统一 sRGB 0..1（非 RGB 空间 naive 转换在解析内完成；转换函数导出供测试）
struct Swatch {
    std::string name;
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

// .ase 组结构；.aco / 无组 .ase = 单隐式组（name 空、hasGroups false）
struct SwatchGroup {
    std::string name;
    std::vector<Swatch> items;
};
struct SwatchFile {
    std::vector<SwatchGroup> groups;
    bool hasGroups = false;
    std::vector<std::string> warnings;
};

bool parseAco(const uint8_t* d, size_t n, SwatchFile& out, std::string& err);
bool parseAse(const uint8_t* d, size_t n, SwatchFile& out, std::string& err);
// 写出：aco = v1+v2 双节（v2 带名）；ase = 1.0（有组结构时写组块）。超 65535/块溢出报错。
bool writeAco(const std::vector<Swatch>& sw, std::vector<uint8_t>& out, std::string& err);
bool writeAse(const SwatchFile& f, std::vector<uint8_t>& out, std::string& err);

// ---------- 渐变（.grd v5） ----------

struct GrdStop {
    float loc = 0.0f;      // 0..1
    float mid = 0.5f;      // 中点 0..1（0.5 = 居中）
    int kind = 0;          // 0=RGB 1=前景 2=背景
    float r = 0.0f, g = 0.0f, b = 0.0f;  // kind==0 时有效（0..1）
};
struct GrdOpacityStop {
    float loc = 0.0f;
    float opacity = 1.0f;  // 0..1
    float mid = 0.5f;
};
struct GrdGradient {
    std::string name;
    bool noise = false;    // ClNs：无 stops（解析保留标记，导入端跳过计数）
    std::vector<GrdStop> stops;
    std::vector<GrdOpacityStop> opacity;
};
// skippedNoise = 跳过的 noise 渐变数；out 空 = 报错（对拍 grd.rs「文件无渐变」）
bool parseGrd(const uint8_t* d, size_t n, std::vector<GrdGradient>& out, int& skippedNoise,
              std::string& err);
// 写出 v5（仅 kind 0 RGB 停；kind 1/2 写成当前色黑——v1 测试用，导出面板登记不做）
bool writeGrd(const std::vector<GrdGradient>& gs, std::vector<uint8_t>& out, std::string& err);

// ---------- 图案（.pat） ----------

struct PatTile {
    std::string name;
    std::string id;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;  // 直 alpha，w×h×4
};
bool parsePat(const uint8_t* d, size_t n, std::vector<PatTile>& out, std::string& err);
// 写出（8 位 RGB+alpha，RLE 行压缩；round-trip 测试与「导出图案」用）
bool writePat(const std::vector<PatTile>& ts, std::vector<uint8_t>& out, std::string& err);

// ---------- 笔刷（.abr） ----------

// 采样笔尖：单灰度平面（行主序；8 位 1 字节/样本，16 位大端 2 字节/样本，最大值 = 全墨）
struct AbrSample {
    std::string id;  // v6+ uuid（desc sampledData 引用）；legacy 空
    uint32_t w = 0, h = 0;
    uint16_t depth = 8;
    std::vector<uint8_t> data;
};
// 映射后的笔刷预设（photocraft abr_map BrushPreset 的 Montage 参数面子集）
struct AbrBrush {
    std::string name;
    float diameter = 0.0f;   // px（sampled = 长边）
    float hardness = 1.0f;   // 0..1
    float spacing = 0.25f;   // 0..10（legacy 0 = 关 → 0.25 默认）
    float angle = 0.0f;      // 度
    float roundness = 1.0f;  // 0.01..1
    float opacity = -1.0f;   // -1 = 预设未携带（toolOptions 缺）
    float flow = -1.0f;
    bool sampled = false;
    size_t sampleIndex = static_cast<size_t>(-1);  // sampled → file.samples 下标
};
struct AbrFile {
    uint16_t version = 0;
    std::vector<AbrSample> samples;
    std::vector<AbrBrush> brushes;
    std::vector<std::string> warnings;
};
bool parseAbr(const uint8_t* d, size_t n, AbrFile& out, std::string& err);

// ---------- 颜色转换（photocraft color/convert.rs naive 同式；导出供测试对拍） ----------

void hsbToRgb(float h, float s, float v, float out[3]);       // h 度、s/v 0..1
void cmykToRgbNaive(const float c[4], float out[3]);          // (1−c)(1−k)
void labToSrgbNaive(float l, float a, float b, float out[3]); // Lab D50 → 编码 sRGB

}  // namespace presets
}  // namespace montage

#endif  // MONTAGE_IO_PRESETS_PRESET_IO_H
