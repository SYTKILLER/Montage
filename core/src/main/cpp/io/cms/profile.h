#ifndef MONTAGE_IO_CMS_PROFILE_H
#define MONTAGE_IO_CMS_PROFILE_H

// M22 色彩管理内核 · ICC profile：解析模型、读取器（ICC.1:2001-04 v2 / ICC.1:2010 v4）
// 与 v4.3 写出，以及 profile ↔ PCS 的求值阶段连接。
// 翻译自 photocraft crates/cms/src/profile.rs + write.rs（MIT OR Apache-2.0）。

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "io/cms/pipeline.h"

namespace montage {
namespace cms {

enum class Intent : int { Perceptual = 0, RelativeColorimetric = 1, Saturation = 2, AbsoluteColorimetric = 3 };

inline constexpr Intent kAllIntents[4] = {Intent::Perceptual, Intent::RelativeColorimetric,
                                          Intent::Saturation, Intent::AbsoluteColorimetric};

inline std::optional<Intent> intentFromU32(uint32_t v) {
    switch (v) {
        case 0: return Intent::Perceptual;
        case 1: return Intent::RelativeColorimetric;
        case 2: return Intent::Saturation;
        case 3: return Intent::AbsoluteColorimetric;
        default: return std::nullopt;
    }
}

inline const char* intentId(Intent i) {
    switch (i) {
        case Intent::Perceptual: return "perceptual";
        case Intent::RelativeColorimetric: return "relative";
        case Intent::Saturation: return "saturation";
        case Intent::AbsoluteColorimetric: return "absolute";
    }
    return "relative";
}

// profile/设备类（头部字节 12..16）。
enum class ProfileClass : uint8_t { Input, Display, Output, DeviceLink, ColorSpace, Abstract, NamedColor, Unknown };

ProfileClass profileClassFromSig(uint32_t sig);
uint32_t profileClassSig(ProfileClass c);
const char* profileClassName(ProfileClass c);

// 数据颜色空间（头部字节 16..20）。
enum class ColorSpace : uint8_t {
    Xyz, Lab, Luv, YCbCr, Yxy, Rgb, Gray, Hsv, Hls, Cmyk, Cmy, NColor, Unknown,
};

ColorSpace colorSpaceFromSig(uint32_t sig);
uint32_t colorSpaceSig(ColorSpace cs);
const char* colorSpaceName(ColorSpace cs);

// 颜色空间通道数。
size_t colorSpaceChannels(ColorSpace cs);

// 阶段列表结束所在的 PCS（float 值：XYZ 的白 Y=1，或 Lab 的 L 0–100）。
enum class Pcs : uint8_t { Xyz, Lab };

// LUT 标签编码族——决定其 PCS 侧的编码方式。
enum class LutKind : uint8_t { Lut8, Lut16, Ab };

// 解析出的 AToB/BToA 标签：设备侧与 PCS 侧值均归一化到 [0,1]。
struct Lut {
    LutKind kind = LutKind::Ab;
    size_t inputs = 0;
    size_t outputs = 0;
    std::vector<Stage> stages;

    bool operator==(const Lut& o) const {
        return kind == o.kind && inputs == o.inputs && outputs == o.outputs && stages == o.stages;
    }
    bool operator!=(const Lut& o) const { return !(*this == o); }
};

// [`Profile::sameColors`] 把两个 profile 视为同一色彩空间的最大色差（CIE ΔE*ab）：
// 不可见，但为 profile 重编码开销的 20 倍。把 sRGB 红基色移 2% 的代价是 1.0。
inline constexpr double kSameColorsMaxDeltaE = 0.5;

// 解析出的 ICC profile。
struct Profile {
    // 主版本与 minor.bugfix 字节（如 (4,0x30)=4.3、(2,0x10)=2.1）。
    uint8_t versionMajor = 4;
    uint8_t versionMinor = 0;
    ProfileClass klass = ProfileClass::Unknown;
    uint32_t classSig = 0;        // Unknown 类的原始签名
    ColorSpace colorSpace = ColorSpace::Unknown;
    uint32_t colorSpaceSig = 0;
    Pcs pcs = Pcs::Xyz;
    Intent renderingIntent = Intent::Perceptual;
    std::string description;
    std::string copyright;
    std::array<double, 3> whitePoint = {0.9642, 1.0, 0.8249};  // wtpt 原样（缺省 D50）
    std::optional<std::array<double, 3>> blackPoint;           // bkpt（存在时）
    std::optional<Mat3> chad;                                  // chad 色适应矩阵
    std::optional<Mat3> matrix;                                // 色度矩阵（列 rXYZ/gXYZ/bXYZ），D50 适配
    std::optional<std::array<Curve, 3>> trc;
    std::optional<Curve> grayTrc;
    std::optional<Lut> a2b[3];  // A2B0/A2B1/A2B2（感知/色度/饱和）
    std::optional<Lut> b2a[3];  // B2A0/B2A1/B2A2

    // 本 profile 解析（或编码）自的字节——字节级嵌入保真用。
    std::shared_ptr<const std::vector<uint8_t>> bytes;
    uint64_t hash = 0;  // 字节 fnv1a（0 = 未算）

    // 解析 ICC profile 字节；失败置 err 返回空。
    static std::optional<Profile> parse(const uint8_t* data, size_t len, std::string& err);
    static std::optional<Profile> parse(const std::vector<uint8_t>& bytes, std::string& err) {
        return parse(bytes.data(), bytes.size(), err);
    }

    // 本 profile 的 ICC 字节：解析过的用原字节，否则做一次全新 v4 编码。
    std::shared_ptr<const std::vector<uint8_t>> toBytes() const;

    // 把模型重编码为 ICC v4 字节并缓存（内置 profile 用）。
    Profile& withEncodedBytes();

    size_t channels() const { return colorSpaceChannels(colorSpace); }

    // profile ICC 字节的哈希（进程内稳定；缓存键用）。
    uint64_t contentHash() const;

    // self 与 other 是否描述同样的颜色：同色彩空间，且设备值网格的每个采样经两者
    // （相对色度 intent）颜色一致（±kSameColorsMaxDeltaE）。同一空间的不同编码视作同色：
    // Photoshop 嵌入的 sRGB IEC61966-2.1（v2、1024 项表）与内置 sRGB（v4、参数曲线）
    // 字节全不同但颜色相同。比较颜色而非往返，因为 CMYK→PCS→CMYK 即使经同一 profile
    // 也不是恒等。
    bool sameColors(const Profile& other) const;

    // ICC 绝对色度用的介质白点：wtpt，但 v2 display profile 的 wtpt 记录的是（未适配的）
    // 显示白——那些按 D50 处理。
    std::array<double, 3> mediaWhite() const;

    // 灰 TRC profile 的 RGB 视图：R/G/B 各带灰曲线并映射到白的三分之一，
    // 使 (v,v,v) 恰为灰 v 的颜色。灰文档的合成图（存为复制 RGB）用。
    std::optional<Profile> grayAsRgb() const;

    // 无 LUT 的矩阵/TRC RGB 或灰 TRC profile。
    bool isMatrixShaper() const;

    // profile 是否有 intent 专用表（矩阵 profile 色度地支持所有 intent）。
    bool supportsIntent(Intent intent, bool input) const;

    // 归一化设备值 → float PCS 的阶段。
    std::optional<std::pair<std::vector<Stage>, Pcs>> deviceToPcs(Intent intent, std::string& err) const;
    // float PCS（返回的 Pcs 种类）→ 归一化设备值的阶段。
    std::optional<std::pair<std::vector<Stage>, Pcs>> pcsToDevice(Intent intent, std::string& err) const;

    bool operator==(const Profile& o) const;
    bool operator!=(const Profile& o) const { return !(*this == o); }
};

// 编码为 ICC v4.3 字节（相同标签数据在标签间共享一份）。
// LUT 阶段布局其标签类型存不下的（只可能出现在解析 profile 用了反方向标签类型时）跳过；
// 矩阵/TRC 标签仍完整描述它。
std::vector<uint8_t> encodeProfile(const Profile& p);

}  // namespace cms
}  // namespace montage

#endif  // MONTAGE_IO_CMS_PROFILE_H
