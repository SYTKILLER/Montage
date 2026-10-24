// M23a：PhotoCraft 测量的相机 profile——供自身不带颜色描述的 raw 格式用
// （DNG 自带；Nikon NEF 不带）。净室：所有数字来自对相机输出的自有测量。
// profile 给出传感器数据内的图像区（相机 JPEG 裁剪）、饱和电平、ForwardMatrix
// （白平衡后相机 → XYZ D50，DNG 语义）与默认色调曲线（同 DNG ProfileToneCurve）。
// **Nikon D4**（单机身 148 张未压缩 NEF）：数据已黑减；绿饱和 15520。
// 翻译自 photocraft crates/raw/src/cameras.rs（MIT OR Apache-2.0）。

#include "io/raw/raw_internal.h"

#include <cstring>

namespace montage {
namespace raw {

namespace {

const Mat3 kD4Forward = {{
    {{0.644598, 0.0703598, 0.249262}},
    {{0.342249, 0.518929, 0.138822}},
    {{0.0568265, -0.269568, 1.03795}},
}};
const Mat3 kD4Color = {{
    {{0.895936, -0.218092, -0.185988}},
    {{-1.09736, 2.06898, -0.0131889}},
    {{-0.26977, 0.393884, 0.687829}},
}};

constexpr std::array<float, 2> kD4Tone[31] = {
    {0.005524272f, 0.00285028f}, {0.006569503f, 0.00357083f}, {0.0078125f, 0.00447912f}, {0.009290681f, 0.00605813f},
    {0.01104854f, 0.00841445f},  {0.01313901f, 0.011536f},   {0.015625f, 0.0165399f},   {0.01858136f, 0.0230234f},
    {0.02209709f, 0.0319205f},   {0.02627801f, 0.0434858f},  {0.03125f, 0.0582138f},    {0.03716272f, 0.0771963f},
    {0.04419417f, 0.0993894f},   {0.05255603f, 0.125058f},   {0.0625f, 0.157207f},      {0.07432544f, 0.194929f},
    {0.08838835f, 0.236109f},    {0.1051121f, 0.279155f},    {0.125f, 0.329365f},       {0.1486509f, 0.388405f},
    {0.1767767f, 0.457944f},     {0.2102241f, 0.52456f},     {0.25f, 0.598051f},        {0.2973018f, 0.668794f},
    {0.3535534f, 0.749954f},     {0.4204482f, 0.815443f},    {0.5f, 0.870167f},         {0.5946036f, 0.907869f},
    {0.7071068f, 0.939351f},     {0.8408964f, 0.970567f},    {1.0f, 1.0f},
};

struct Profile {
    const char* make;
    const char* model;
    size_t width, height;  // profile 测量时的传感器数据尺寸
    size_t x, y, w, h;     // 图像区
    float white;
    bool blackZero;  // 黑电平已知为 0（数据机内黑减）
    const Mat3* forward;
    const Mat3* color;
    const std::array<float, 2>(*tone)[31];
};

constexpr Profile kProfiles[1] = {
    {"NIKON CORPORATION", "NIKON D4", 4992, 3292, 8, 6, 4928, 3280, 15520.0f, true, &kD4Forward, &kD4Color, &kD4Tone},
};

// 尚未测全 profile 机身的实测图像区（方法同 profile：解码平面与相机 JPEG 对齐）。
struct ImageArea {
    const char* make;
    const char* model;
    size_t width, height;
    size_t x, y, w, h;
};

constexpr ImageArea kImageAreas[3] = {
    {"NIKON CORPORATION", "NIKON D5100", 4992, 3280, 8, 8, 4928, 3264},
    {"NIKON CORPORATION", "NIKON D800E", 7424, 4924, 8, 6, 7360, 4912},
    {"SONY", "ILCE-7", 6048, 4024, 12, 12, 6000, 4000},
};

bool eq(const std::optional<std::string>& s, const char* lit) {
    return s.has_value() && s->compare(lit) == 0;
}

}  // namespace

// 应用匹配的测量 profile；未测全的机身应用实测图像区。
void applyCameraProfile(Sensor& s) {
    if (!s.make || !s.model) return;
    for (const ImageArea& a : kImageAreas) {
        if (eq(s.make, a.make) && eq(s.model, a.model) && s.width == a.width && s.height == a.height && s.crop == s.active) {
            s.crop = Rect::make(a.x, a.y, a.w, a.h);
        }
    }
    for (const Profile& p : kProfiles) {
        if (!eq(s.make, p.make) || !eq(s.model, p.model) || s.width != p.width || s.height != p.height) continue;
        s.crop = Rect::make(p.x, p.y, p.w, p.h);
        s.white[0] = s.white[1] = s.white[2] = p.white;
        if (p.blackZero) {
            s.warnings.erase(std::remove_if(s.warnings.begin(), s.warnings.end(),
                                            [](const std::string& w) { return w.find("black level not recorded") != std::string::npos; }),
                             s.warnings.end());
        }
        ColorInfo info;
        Calibration cal;
        // D50 日光：矩阵在相机日光中性下一致。
        cal.temperature = 5003.0;
        cal.colorMatrix = *p.color;
        cal.forwardMatrix = *p.forward;
        cal.cameraCalibration = kIdentityMat3;
        info.calibrations.push_back(std::move(cal));
        s.color = std::move(info);
        s.toneCurve.assign(&(*p.tone)[0], &(*p.tone)[0] + 31);
        break;
    }
}

}  // namespace raw
}  // namespace montage
