#ifndef MONTAGE_IO_RAW_RAW_DEVELOP_H
#define MONTAGE_IO_RAW_RAW_DEVELOP_H

// M23a：raw 显影——线性化、减黑、缩放到白、白平衡、剪裁高光、去马赛克、
// 相机 → XYZ(D50) → 线性 ProPhoto、曝光、gamma 1.8 编码到 16 位、orientation。
// 翻译自 photocraft crates/raw/src/develop.rs（MIT OR Apache-2.0）。
// 输出 16-bit ProPhoto（ROMM 基色，D50，gamma 1.8）；Montage 发布链
// （TileGrid sRGB）在 io/dng_decode 的门面层做 ProPhoto → sRGB 转换。

#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "io/raw/raw_demosaic.h"
#include "io/raw/raw_look.h"
#include "io/raw/raw_types.h"

namespace montage {
namespace raw {

// 白平衡选择。
struct WhiteBalance {
    enum Mode { AsShot, Auto, Multipliers };
    Mode mode = AsShot;
    std::array<double, 3> multipliers = {1.0, 1.0, 1.0};  // mode = Multipliers 时

    static WhiteBalance asShot() { return {}; }
    static WhiteBalance autoWb() { return {Auto, {}}; }
    static WhiteBalance fromMultipliers(std::array<double, 3> m) { return {Multipliers, m}; }
};

// 显影设置。
struct DevelopOptions {
    Demosaic demosaic = Demosaic::Ahd;
    WhiteBalance whiteBalance;
    // 曝光补偿 EV，加在文件基线曝光上。
    double exposure = 0.0;
    // 用户色温/色调偏移（-100..100，Montage M15e 面板语义）：在解析出的中性色上
    // 调制通道增益 ±30%（暖抬 R 压 B；品红压 G）。核内防御性钳制。
    float temp = 0.0f;
    float tint = 0.0f;
    // 应用文件 orientation（旋转/翻转像素）。
    bool orient = true;
    // M23c：per-file 相机 look（fit_look 产物）。文件无颜色校准时替代中性
    // 回退（不可用时忽略并告警，语义对齐 photocraft develop）。
    std::optional<CameraLook> cameraLook;
    Limits limits;
};

// 显影信息。
struct RawInfo {
    RawFormat format = RawFormat::TiffEp;
    std::optional<std::string> make;
    std::optional<std::string> model;
    // 传感器数据尺寸。
    size_t sensorWidth = 0, sensorHeight = 0;
    // 裁剪原点的 CFA 图案：Bayer 为 "RGGB"；其他图案逐行列出整个重复，行以 `/` 分
    // （X-Trans: "GGRGGB/GGBGGR/…"）；LinearRaw 为空。
    std::optional<std::string> cfa;
    // 使用的白平衡倍率（R, G, B，按最小值归一）。
    std::array<double, 3> wbMultipliers = {1.0, 1.0, 1.0};
    uint16_t orientation = 1;
    double baselineExposure = 0.0;
};

// 显影结果：16-bit RGB，ProPhoto RGB（ROMM 基色，D50，gamma 1.8）。
struct Developed {
    uint32_t width = 0, height = 0;
    // 交错 RGB，`width * height * 3` 样本。
    std::vector<uint16_t> rgb;
    RawInfo info;
    // 一切近似或未应用的说明。
    std::vector<std::string> warnings;
};

// 显影传感器数据。
Developed developSensor(const Sensor& s, const DevelopOptions& opts);

// 线性 0..1 → gamma-1.8 16-bit 码的查找表（tone 曲线折叠进表）。
class GammaLut {
public:
    explicit GammaLut(const std::vector<std::array<float, 2>>& curve);
    inline uint16_t encode(float v) const {
        v = std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f);
        float x = std::sqrt(v) * static_cast<float>(kSteps);
        size_t i = std::min(static_cast<size_t>(x), static_cast<size_t>(kSteps));
        float f = x - static_cast<float>(i);
        float a = table[i];
        return static_cast<uint16_t>(a + (table[i + 1] - a) * f + 0.5f);
    }

private:
    static constexpr size_t kSteps = 4096;
    std::vector<float> table;
};

// TIFF orientation（1–8）应用于交错 RGB。
std::tuple<std::vector<uint16_t>, size_t, size_t> orientRgb(std::vector<uint16_t> src, size_t w, size_t h, uint16_t o);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_DEVELOP_H
