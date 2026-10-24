#ifndef MONTAGE_ENGINE_CAMERA_RAW_H
#define MONTAGE_ENGINE_CAMERA_RAW_H

// M23b：Camera Raw 滤镜/显影链——浮点 RGB 像素上的显影管线。
// 翻译自 photocraft crates/algo/src/camera_raw.rs（MIT OR Apache-2.0）。
// 9 级（各控制中性即跳过，默认恒等）：
//  1 线性域白平衡+曝光（sRGB 曲线作感知工作编码，结束精确还原）
//  2 去雾（He 暗通道先验，大气光取暗通道最亮 0.1%，导向滤波精化透射率；负值加雾）
//  3 色调（亮度域，色比保持：高光/阴影=导向滤波边缘保持底层的曝光式加权；
//    白/黑移动端点；对比度中灰 S 曲线；清晰度大半径中间调带通、纹理小半径）
//  4 存在感：自然饱和度（偏向低饱和）+饱和度
//  5 色调曲线：参数化区域（高光/亮调/暗调/阴影，分割可动）+点曲线（主/RGB 通道，
//    Fritsch–Carlson 单调三次插值）
//  6 HSL 混色器：八色相带（红橙黄绿青蓝紫洋红）色相圆帽函数权重
//  7 颜色分级：阴影/中间调/高光/全局四轮（色相/饱和/亮度）+混合+平衡
//  8 细节：锐化（USM，Detail 控光晕、Masking 边缘蒙版）+降噪（亮度导向滤波、彩色色度模糊）
//  9 效果：裁后暗角（数量/中点/圆度/羽化/高光/样式）+颗粒（数量/大小/粗糙度；位置哈希确定性）
// par 并行单线程化（photocraft 测试已证线程数无关；设备性能基线挂 M16）。

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace montage {

// 一个颜色分级轮。
struct CameraWheel {
    float hue = 0.0f;  // 角度 0..360
    float sat = 0.0f;  // 0..=100
    float lum = 0.0f;  // -100..=100

    bool neutral() const { return sat == 0.0f && lum == 0.0f; }
};

// Camera Raw 设置（Adobe Camera Raw 滑杆单位）。
struct CameraRaw {
    // Basic
    float temperature = 0.0f;
    float tint = 0.0f;
    float exposure = 0.0f;
    float contrast = 0.0f;
    float highlights = 0.0f;
    float shadows = 0.0f;
    float whites = 0.0f;
    float blacks = 0.0f;
    float texture = 0.0f;
    float clarity = 0.0f;
    float dehaze = 0.0f;
    float vibrance = 0.0f;
    float saturation = 0.0f;
    // 色调曲线
    float curveHighlights = 0.0f;
    float curveLights = 0.0f;
    float curveDarks = 0.0f;
    float curveShadows = 0.0f;
    // 区域分割（百分比）。
    float curveSplits[3] = {25.0f, 50.0f, 75.0f};
    // 点曲线 [入,出] 0..=255（空 = 线性）。
    std::vector<std::array<float, 2>> pointCurve;
    std::vector<std::array<float, 2>> pointCurveRed;
    std::vector<std::array<float, 2>> pointCurveGreen;
    std::vector<std::array<float, 2>> pointCurveBlue;
    // 混色器：红、橙、黄、绿、青、蓝、紫、洋红。
    float hslHue[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float hslSat[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float hslLum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    // 颜色分级
    CameraWheel gradeShadows;
    CameraWheel gradeMidtones;
    CameraWheel gradeHighlights;
    CameraWheel gradeGlobal;
    float gradeBlending = 50.0f;
    float gradeBalance = 0.0f;
    // 细节
    float sharpenAmount = 0.0f;
    float sharpenRadius = 1.0f;
    float sharpenDetail = 25.0f;
    float sharpenMasking = 0.0f;
    float noiseLuminance = 0.0f;
    float noiseLuminanceDetail = 50.0f;
    float noiseColor = 0.0f;
    float noiseColorDetail = 50.0f;
    // 效果
    float grainAmount = 0.0f;
    float grainSize = 25.0f;
    float grainRoughness = 50.0f;
    float vignetteAmount = 0.0f;
    float vignetteMidpoint = 50.0f;
    float vignetteRoundness = 0.0f;
    float vignetteFeather = 50.0f;
    float vignetteHighlights = 0.0f;
    // highlightPriority | colorPriority | paintOverlay
    std::string vignetteStyle = "highlightPriority";
    uint32_t seed = 0;
    // 降采样代理预览的像素尺寸倍数（半径按它缩放）。
    float pixelScale = 1.0f;

    bool wbNeutral() const { return temperature == 0.0f && tint == 0.0f && exposure == 0.0f; }
    bool toneNeutral() const;
    bool hslNeutral() const;
    bool gradeNeutral() const { return gradeShadows.neutral() && gradeMidtones.neutral() && gradeHighlights.neutral() && gradeGlobal.neutral(); }
    bool curvesNeutral() const;
    // 所有控制都中性（滤镜恒等）时为真。
    bool isIdentity() const;
};

// CameraRaw JSON（camelCase）→ CameraRaw（M23b 起 dng_decode 与 M23c 滤镜路径共用；
// 未识别字段忽略；畸形点曲线容错回退线性）。
bool parseCameraJson(const std::string& json, CameraRaw& out);

// 点曲线校验（非受信输入）：空（线性）或 2..=16 个 0..=255 有限点、输入至少逐级递增。
// 失败返回错误文案。
std::string validateCurve(const std::vector<std::array<float, 2>>& points);  // 空 = 通过

// Camera Raw 的 Temperature/Tint 通道增益（-100…100），相对 as-shot 平衡：
// 暖抬 R 压 B（端点 ±35%）；负色调抬绿、正色调压向品红（±25%）。
// 对 Adobe 滑杆的观察近似，非色温模型；滤镜（线性 RGB）与 raw 打开（相机 WB 倍率）
// 共享，保证两边一致。
std::array<float, 3> whiteBalanceGains(float temperature, float tint);

// 色相带中心（度）。
extern const float kHslBands[8];

// 八个 HSL 带在色相 h 处的权重（相邻中心之间的帽函数）。
std::array<float, 8> bandWeights(float h);

// 单调三次（Fritsch–Carlson）过 `pts`（x 升序，0..=255 域），求值进 n 项 LUT（0..1 域）。
std::vector<float> cameraCurveLut(const std::vector<std::array<float, 2>>& pts, size_t n);

// 在直 RGBA 浮点像素（w×h，显示编码 RGB）上运行 Camera Raw 管线。
// `floatDoc` 保留 >1 的值（32 位文档）；false 时结尾钳制 0..1。
// px = 交错 RGBA 浮点（stride 4），RGB 0..1 显示编码。
void cameraRawDevelop(float* px, size_t w, size_t h, const CameraRaw& p, bool floatDoc);

}  // namespace montage

#endif  // MONTAGE_ENGINE_CAMERA_RAW_H
