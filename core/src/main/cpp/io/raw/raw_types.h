#ifndef MONTAGE_IO_RAW_RAW_TYPES_H
#define MONTAGE_IO_RAW_RAW_TYPES_H

// M23a RAW 解码内核：公共类型（sensor.rs + error.rs + opcodes.rs 的类型面）。
// 翻译自 photocraft crates/raw（MIT OR Apache-2.0）。
// 错误模型：photocraft 的 RawError 枚举 → C++ 异常 RawError（只在出错路径抛出，
// 公共入口捕获——敌意文件不得崩溃，只能得到错误）。

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "io/raw/raw_tiff.h"

namespace montage {
namespace raw {

// 为什么一个 raw 文件解不开。
struct RawError {
    enum Kind { NotRaw, Unsupported, Malformed, LimitExceeded };
    Kind kind;
    std::string msg;

    static RawError notRaw() { return {NotRaw, "not a camera raw file"}; }
    static RawError unsupported(std::string m) { return {Unsupported, std::move(m)}; }
    static RawError malformed(std::string m) { return {Malformed, std::move(m)}; }
    static RawError limitExceeded(std::string m) { return {LimitExceeded, std::move(m)}; }
    const char* name() const {
        switch (kind) {
            case NotRaw: return "not raw";
            case Unsupported: return "unsupported";
            case Malformed: return "malformed";
            default: return "limit exceeded";
        }
    }
};

// 相机 raw 容器族。
enum class RawFormat { Dng, Cr2, Cr3, Nef, Arw, Pef, Orf, Rw2, Raf, TiffEp };

inline const char* rawFormatName(RawFormat f) {
    switch (f) {
        case RawFormat::Dng: return "DNG";
        case RawFormat::Cr2: return "CR2";
        case RawFormat::Cr3: return "CR3";
        case RawFormat::Nef: return "NEF";
        case RawFormat::Arw: return "ARW";
        case RawFormat::Pef: return "PEF";
        case RawFormat::Orf: return "ORF";
        case RawFormat::Rw2: return "RW2";
        case RawFormat::Raf: return "RAF";
        default: return "TIFF/EP raw";
    }
}

// 解压炸弹护栏：头字段在分配前检查（与 photocraft-codecs 同形同默认）。
struct Limits {
    uint32_t maxWidth = 1u << 18;
    uint32_t maxHeight = 1u << 18;
    uint64_t maxPixels = 1ull << 28;
    uint64_t maxAlloc = 2ull << 30;

    // 检查 `width × height` 的 `bytesPerPixel` 字节缓冲。
    void check(uint64_t width, uint64_t height, uint64_t bytesPerPixel) const {
        if (width == 0 || height == 0) throw RawError::malformed("zero-sized image " + std::to_string(width) + "x" + std::to_string(height));
        if (width > maxWidth || height > maxHeight) {
            throw RawError::limitExceeded("dimensions " + std::to_string(width) + "x" + std::to_string(height) + " exceed " +
                                          std::to_string(maxWidth) + "x" + std::to_string(maxHeight));
        }
        uint64_t pixels = width * height;
        if (pixels > maxPixels) throw RawError::limitExceeded("pixels exceed " + std::to_string(maxPixels));
        uint64_t bytes = pixels * bytesPerPixel;
        if (bytes > maxAlloc) throw RawError::limitExceeded("bytes exceed " + std::to_string(maxAlloc));
    }
};

// 传感器数据像素域的矩形。
struct Rect {
    size_t x = 0, y = 0, width = 0, height = 0;

    static Rect make(size_t px, size_t py, size_t w, size_t h) { return {px, py, w, h}; }
    Rect intersect(const Rect& o) const {
        size_t x0 = std::max<>(x, o.x);
        size_t y0 = std::max<>(y, o.y);
        size_t x1 = std::min<>(x + width, o.x + o.width);
        size_t y1 = std::min<>(y + height, o.y + o.height);
        return {x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0};
    }
    bool empty() const { return width == 0 || height == 0; }
    bool operator==(const Rect& o) const { return x == o.x && y == o.y && width == o.width && height == o.height; }
};

// 一个 CFA 格的颜色：0 = 红，1 = 绿，2 = 蓝。
struct Cfa {
    size_t width = 0, height = 0;
    // 行主序颜色，`width * height` 项。
    std::vector<uint8_t> colors;
    // 图案左上格的数据坐标。
    size_t originX = 0, originY = 0;

    static std::optional<Cfa> bayer(const char* pattern) {
        Cfa c;
        c.width = 2;
        c.height = 2;
        c.originX = 0;
        c.originY = 0;
        for (const char* p = pattern; *p; ++p) {
            switch (*p) {
                case 'R': c.colors.push_back(0); break;
                case 'G': c.colors.push_back(1); break;
                case 'B': c.colors.push_back(2); break;
                default: return std::nullopt;
            }
        }
        if (c.colors.size() != 4) return std::nullopt;
        return c;
    }

    // 数据坐标处的颜色。
    uint8_t color(size_t x, size_t y) const {
        if (width == 0 || height == 0 || colors.empty()) return 1;
        size_t cx = (x + width - originX % width) % width;
        size_t cy = (y + height - originY % height) % height;
        size_t idx = cy * width + cx;
        return idx < colors.size() ? colors[idx] : 1;
    }

    // 数据坐标 (x0, y0) 处看到的 2×2 颜色，行主序。
    std::array<uint8_t, 4> phase(size_t x0, size_t y0) const {
        return {color(x0, y0), color(x0 + 1, y0), color(x0, y0 + 1), color(x0 + 1, y0 + 1)};
    }

    // Bayer 图案判定：2×2 周期含一红一蓝两条对角绿。
    bool isBayer() const {
        if (width == 0 || height == 0 || width > 16 || height > 16) return false;
        auto p = phase(originX, originY);
        bool periodic = true;
        for (size_t y = 0; y < height && periodic; ++y)
            for (size_t x = 0; x < width; ++x)
                if (color(originX + x, originY + y) != p[(y % 2) * 2 + x % 2]) { periodic = false; break; }
        bool greensOnDiagonal =
            (p[0] == 1 && p[3] == 1 && p[1] != 1 && p[2] != 1 && p[1] + p[2] == 2) ||
            (p[1] == 1 && p[2] == 1 && p[0] != 1 && p[3] != 1 && p[0] + p[3] == 2);
        return periodic && greensOnDiagonal;
    }
};

// 黑电平（DNG BlackLevel / BlackLevelRepeatDim / BlackLevelDeltaH/V）。
struct BlackLevels {
    size_t rows = 1, cols = 1;
    // `rows * cols * samples` 个值，sample 最快。
    std::vector<float> values;
    // 活动区每列的增量。
    std::vector<float> deltaH;
    // 活动区每行的增量。
    std::vector<float> deltaV;

    static BlackLevels uniform(float v) { return {1, 1, {v}, {}, {}}; }

    // 活动区坐标 (ax, ay) 处 sample `s`（共 spp）的黑电平。
    float at(size_t ax, size_t ay, size_t s, size_t spp) const {
        size_t r = rows > 0 ? ay % rows : 0;
        size_t c = cols > 0 ? ax % cols : 0;
        float base = 0.0f;
        size_t idx = (r * (cols > 0 ? cols : 1) + c) * spp + s;
        if (idx < values.size()) {
            base = values[idx];
        } else if (!values.empty()) {
            base = values[0];
        }
        float dh = ax < deltaH.size() ? deltaH[ax] : 0.0f;
        float dv = ay < deltaV.size() ? deltaV[ay] : 0.0f;
        return base + dh + dv;
    }

    bool isUniform() const {
        for (float v : deltaH)
            if (v != 0.0f) return false;
        for (float v : deltaV)
            if (v != 0.0f) return false;
        for (size_t i = 1; i < values.size(); ++i)
            if (values[i - 1] != values[i]) return false;
        return true;
    }
};

// 颜色科学的数据结构（数学函数见 raw_color.h）。Mat3 = 3×3 行主序。
using Mat3 = std::array<std::array<double, 3>, 3>;
constexpr Mat3 kIdentityMat3 = {{{{1.0, 0.0, 0.0}}, {{0.0, 1.0, 0.0}}, {{0.0, 0.0, 1.0}}}};

// 一个 DNG 校准（ColorMatrixN 及其同伴）。
struct Calibration {
    // 光源开尔文温度。
    double temperature = 0;
    // XYZ → 参考相机（ColorMatrixN）。
    Mat3 colorMatrix = kIdentityMat3;
    // 白平衡后相机 → XYZ D50（ForwardMatrixN）。
    std::optional<Mat3> forwardMatrix;
    // 参考相机 → 单机（CameraCalibrationN）。
    Mat3 cameraCalibration = kIdentityMat3;
};

// raw 文件的颜色描述。
struct ColorInfo {
    // DNG 校准（0、1 或 2 个），按温度排序。
    std::vector<Calibration> calibrations;
    // AnalogBalance。
    std::optional<std::array<double, 3>> analogBalance;
    // AsShotNeutral：中性色的相机坐标。
    std::optional<std::array<double, 3>> asShotNeutral;
    // AsShotWhiteXY。
    std::optional<std::array<double, 2>> asShotWhiteXY;
};

// DNG OpcodeList2 增益图（镜头阴影校正），应用于归一化后的数据。
struct GainMap {
    size_t top = 0, left = 0, bottom = 0, right = 0;
    size_t plane = 0, planes = 0;
    size_t rowPitch = 0, colPitch = 0;
    size_t pointsV = 0, pointsH = 0;
    // 图点间距与原点，图像相对坐标 (0..1)。
    double spacingV = 0, spacingH = 0, originV = 0, originH = 0;
    size_t mapPlanes = 0;
    // `pointsV × pointsH × mapPlanes` 个增益。
    std::vector<float> gains;

    // 图像相对行 `rv`、图平面 `mp` 沿图水平点的增益（竖直插值已做）。
    std::vector<float> rowOf(double rv, size_t mp) const {
        size_t mpC = std::min(mp, mapPlanes > 0 ? mapPlanes - 1 : 0);
        double fv = std::clamp((rv - originV) / spacingV, 0.0, static_cast<double>(pointsV > 0 ? pointsV - 1 : 0));
        size_t i0 = std::min(static_cast<size_t>(std::floor(fv)), pointsV > 0 ? pointsV - 1 : 0);
        size_t i1 = std::min(i0 + 1, pointsV > 0 ? pointsV - 1 : 0);
        float t = static_cast<float>(fv - static_cast<double>(i0));
        std::vector<float> out(pointsH, 1.0f);
        for (size_t j = 0; j < pointsH; ++j) {
            size_t ia = (i0 * pointsH + j) * mapPlanes + mpC;
            size_t ib = (i1 * pointsH + j) * mapPlanes + mpC;
            float a = ia < gains.size() ? gains[ia] : 1.0f;
            float b = ib < gains.size() ? gains[ib] : 1.0f;
            out[j] = a + (b - a) * t;
        }
        return out;
    }

    float at(const std::vector<float>& row, double rh) const {
        double fh = std::clamp((rh - originH) / spacingH, 0.0, static_cast<double>(pointsH > 0 ? pointsH - 1 : 0));
        size_t j0 = std::min(static_cast<size_t>(std::floor(fh)), pointsH > 0 ? pointsH - 1 : 0);
        size_t j1 = std::min(j0 + 1, pointsH > 0 ? pointsH - 1 : 0);
        float t = static_cast<float>(fh - static_cast<double>(j0));
        float a = j0 < row.size() ? row[j0] : 1.0f;
        float b = j1 < row.size() ? row[j1] : 1.0f;
        return a + (b - a) * t;
    }

    bool covers(size_t ax, size_t ay) const {
        return ay >= top && ay < bottom && ax >= left && ax < right &&
               rowPitch > 0 && (ay - top) % rowPitch == 0 && colPitch > 0 && (ax - left) % colPitch == 0;
    }
};

// 未显影的传感器数据与显影所需的全部元数据。
struct Sensor {
    RawFormat format = RawFormat::TiffEp;
    std::optional<std::string> make;
    std::optional<std::string> model;
    // 数据宽高（像素）。
    size_t width = 0, height = 0;
    // 每像素样本数：CFA 为 1，LinearRaw 为 3（单色 LinearRaw 为 1）。
    size_t samples = 0;
    // 行主序交错样本，`width * height * samples`。
    std::vector<uint16_t> data;
    // CFA 图案（仅 CFA 数据）。
    std::optional<Cfa> cfa;
    // 存储值 → 线性值映射（DNG LinearizationTable）。
    std::optional<std::vector<uint16_t>> linearization;
    BlackLevels black;
    // 每样本白（饱和）电平。
    float white[3] = {65535.0f, 65535.0f, 65535.0f};
    // 持有图像数据的传感器区（DNG ActiveArea）；黑电平增量以它为索引起点。
    Rect active;
    // 输出区（ActiveArea ∩ DefaultCrop）。
    Rect crop;
    ColorInfo color;

    // 白平衡倍率（相机自己的元数据，非 DNG），R G B。
    std::optional<std::array<double, 3>> cameraWb;
    // TIFF orientation（1–8）。
    uint16_t orientation = 1;
    // 默认渲染烘焙的曝光补偿（DNG BaselineExposure），EV。
    double baselineExposure = 0.0;
    // DNG OpcodeList2 增益图。
    std::vector<GainMap> gainMaps;
    // 相机 profile 默认色调曲线：线性 ProPhoto 域递增 [入,出] 点对，颜色转换与
    // 曝光之后逐通道应用。首点之下为过 0 的直线；空表 = 无曲线。
    std::vector<std::array<float, 2>> toneCurve;
    std::vector<std::string> warnings;

    // 单色数据（单样本 LinearRaw）判定：显影为中灰，无白平衡无颜色校准。
    bool isMonochrome() const { return samples == 1 && !cfa.has_value(); }
};

// 从一个 IFD 读出的原始样本。
struct Plane {
    size_t width = 0, height = 0, samples = 0;
    uint32_t bits = 0;
    std::vector<uint16_t> data;
};

struct Segment {
    size_t x = 0, y = 0;
    // 编码宽高（瓦片尺寸，或图像宽 × 每 strip 行数）。
    size_t w = 0, h = 0;
    size_t offset = 0, len = 0;
};

// 无损 JPEG 瓦片样本到瓦片的映射方式。
enum class JpegLayout { Flat, Quads };

// 读 raw IFD 的（未压缩/无损 JPEG）图像。实现见 raw_plane.cpp。
Plane readPlane(const Tiff& t, const Ifd& ifd, const Limits& limits, JpegLayout layout);
std::vector<Segment> segmentsOf(const Tiff& t, const Ifd& ifd, size_t width, size_t height);

}  // namespace raw
}  // namespace montage

#endif  // MONTAGE_IO_RAW_RAW_TYPES_H
