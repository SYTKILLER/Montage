#include "io/cms/builtin.h"

#include <array>
#include <cctype>
#include <cstring>

#include "io/cms/synth.h"

namespace montage {
namespace cms {

// 合成 CMYK 内置 profile：内嵌 photocraft 程序生成的字节（CC0；生成模型见 synth.cpp）。
extern const uint8_t kCoatedCmykIcc[236844];

namespace {

// 内置 RGB 空间的 ICC D65（色度 0.3127, 0.3290）。
std::array<double, 3> builtinD65() {
    return xyToXyz(kD65Xy);
}

// D50 适配并 s15Fixed16 量化的色度矩阵，每行微调使 RGB (1,1,1) 恰映射到存储的 D50 白
//（中性保持中性）。
std::pair<Mat3, std::optional<Mat3>> colorants(const std::array<double, 2>& r, const std::array<double, 2>& g,
                                               const std::array<double, 2>& b,
                                               const std::array<double, 3>& white) {
    auto mOpt = rgbToXyzMatrix(r, g, b, white);
    // 只用内置空间的常量基色调用（测试构建全部内置 profile，基色必独立）。
    const Mat3 m = mOpt ? *mOpt : kIdentityMat3;
    const auto d50 = d50Quantized();
    const bool nearD50 = std::abs(white[0] - kD50[0]) < 1e-3 && std::abs(white[2] - kD50[2]) < 1e-3;
    std::optional<Mat3> adapt;
    Mat3 m50 = m;
    if (!nearD50) {
        adapt = bradford(white, kD50);
        m50 = mul(*adapt, m);
    } else {
        // 白已（近）D50：按行缩放到精确 PCS 白。
        const auto s = apply(m, {1.0, 1.0, 1.0});
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                m50[i][j] *= kD50[i] / s[i];
            }
        }
    }
    Mat3 q{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            q[i][j] = s15f16Round(m50[i][j]);
        }
        // 行和与量化 D50 的误差补到本行绝对值最大的元素。
        const double err = d50[i] - (q[i][0] + q[i][1] + q[i][2]);
        int k = 0;
        for (int j = 1; j < 3; ++j) {
            if (std::abs(q[i][j]) > std::abs(q[i][k])) {
                k = j;
            }
        }
        q[i][k] = s15f16Round(q[i][k] + err);
    }
    if (adapt) {
        Mat3 qa{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                qa[i][j] = s15f16Round((*adapt)[i][j]);
            }
        }
        adapt = qa;
    }
    return std::make_pair(q, adapt);
}

Profile rgbBuiltin(const char* desc, const std::array<std::array<double, 2>, 3>& prim,
                   const std::array<double, 3>& white, const Curve& trc) {
    auto mc = colorants(prim[0], prim[1], prim[2], white);
    Profile p;
    p.versionMajor = 4;
    p.versionMinor = 0x30;
    p.klass = ProfileClass::Display;
    p.colorSpace = ColorSpace::Rgb;
    p.colorSpaceSig = colorSpaceSig(ColorSpace::Rgb);
    p.pcs = Pcs::Xyz;
    p.renderingIntent = Intent::Perceptual;
    p.description = desc;
    p.copyright = kBuiltinCopyright;
    const auto d50 = d50Quantized();
    p.whitePoint = d50;
    p.chad = mc.second;
    p.matrix = mc.first;
    p.trc = std::array<Curve, 3>{trc, trc, trc};
    p.withEncodedBytes();
    return p;
}

Profile grayBuiltin(const char* desc, const Curve& trc) {
    Profile p;
    p.versionMajor = 4;
    p.versionMinor = 0x30;
    p.klass = ProfileClass::Display;
    p.colorSpace = ColorSpace::Gray;
    p.colorSpaceSig = colorSpaceSig(ColorSpace::Gray);
    p.pcs = Pcs::Xyz;
    p.renderingIntent = Intent::Perceptual;
    p.description = desc;
    p.copyright = kBuiltinCopyright;
    const auto d50 = d50Quantized();
    p.whitePoint = d50;
    p.grayTrc = trc;
    p.withEncodedBytes();
    return p;
}

Profile labBuiltin() {
    Lut ident;
    ident.kind = LutKind::Ab;
    ident.inputs = 3;
    ident.outputs = 3;
    ident.stages.push_back(Stage::curvesStage(std::vector<Curve>(3, Curve::identity())));
    Profile p;
    p.versionMajor = 4;
    p.versionMinor = 0x30;
    p.klass = ProfileClass::ColorSpace;
    p.colorSpace = ColorSpace::Lab;
    p.colorSpaceSig = colorSpaceSig(ColorSpace::Lab);
    p.pcs = Pcs::Lab;
    p.renderingIntent = Intent::Perceptual;
    p.description = builtinDescription(Builtin::LabD50);
    p.copyright = kBuiltinCopyright;
    p.whitePoint = d50Quantized();
    p.a2b[0] = ident;
    p.b2a[0] = ident;
    p.withEncodedBytes();
    return p;
}

Profile coatedCmykBuiltin() {
    // 内嵌字节即 synth::coatedCmyk() 的产物（photocraft regen 测试锁定）；解析失败则
    // 现场重建（synth.cpp；仅理论兜底路径）。
    std::string err;
    auto p = Profile::parse(kCoatedCmykIcc, sizeof(kCoatedCmykIcc), err);
    if (p) {
        return *p;
    }
    return synthCoatedCmyk();
}

}  // namespace

const char* builtinId(Builtin b) {
    switch (b) {
        case Builtin::Srgb: return "srgb";
        case Builtin::DisplayP3: return "display-p3";
        case Builtin::AdobeRgbCompat: return "adobe-rgb-compat";
        case Builtin::ProPhotoCompat: return "prophoto-compat";
        case Builtin::LinearSrgb: return "linear-srgb";
        case Builtin::Rec2020: return "rec2020";
        case Builtin::GrayGamma22: return "gray-gamma-2.2";
        case Builtin::SGray: return "sgray";
        case Builtin::LabD50: return "lab-d50";
        case Builtin::CoatedCmyk: return "coated-cmyk";
    }
    return "srgb";
}

const char* builtinDescription(Builtin b) {
    switch (b) {
        case Builtin::Srgb: return "sRGB IEC61966-2.1";
        case Builtin::DisplayP3: return "Display P3";
        case Builtin::AdobeRgbCompat: return "Adobe RGB (1998)-compatible (Montage)";
        case Builtin::ProPhotoCompat: return "ProPhoto RGB-compatible (ROMM primaries, Montage)";
        case Builtin::LinearSrgb: return "Linear sRGB (Montage)";
        case Builtin::Rec2020: return "Rec. ITU-R BT.2020 (Montage)";
        case Builtin::GrayGamma22: return "Gray Gamma 2.2 (Montage)";
        case Builtin::SGray: return "sGray (sRGB tone curve, Montage)";
        case Builtin::LabD50: return "Lab D50 identity (Montage)";
        case Builtin::CoatedCmyk: return "Photocraft Coated CMYK (synthetic, 300% TAC, medium GCR)";
    }
    return "";
}

std::optional<Builtin> builtinFromId(const std::string& s) {
    // 只保留字母数字的小写化。
    std::string n;
    n.reserve(s.size());
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    std::optional<Builtin> alias;
    if (n == "srgb" || n == "srgbiec6196621" || n == "srgbiec61966") {
        alias = Builtin::Srgb;
    } else if (n == "p3" || n == "displayp3") {
        alias = Builtin::DisplayP3;
    } else if (n == "adobergb" || n == "adobergb1998" || n == "adobergbcompat" || n == "clayrgb") {
        alias = Builtin::AdobeRgbCompat;
    } else if (n == "prophoto" || n == "prophotorgb" || n == "prophotocompat" || n == "romm" || n == "rommrgb") {
        alias = Builtin::ProPhotoCompat;
    } else if (n == "linearsrgb" || n == "srgblinear" || n == "linear") {
        alias = Builtin::LinearSrgb;
    } else if (n == "rec2020" || n == "bt2020") {
        alias = Builtin::Rec2020;
    } else if (n == "gray" || n == "graygamma22" || n == "gamma22") {
        alias = Builtin::GrayGamma22;
    } else if (n == "sgray" || n == "graysrgb") {
        alias = Builtin::SGray;
    } else if (n == "lab" || n == "labd50" || n == "cielab") {
        alias = Builtin::LabD50;
    } else if (n == "cmyk" || n == "coatedcmyk" || n == "defaultcmyk" || n == "photocraftcoatedcmyk") {
        alias = Builtin::CoatedCmyk;
    }
    if (alias) {
        return alias;
    }
    for (int i = 0; i < 10; ++i) {
        const Builtin b = static_cast<Builtin>(i);
        std::string desc = builtinDescription(b);
        std::string dn;
        for (char c : desc) {
            if (std::isalnum(static_cast<unsigned char>(c))) {
                dn.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        if (dn == n) {
            return b;
        }
    }
    return std::nullopt;
}

const Profile& builtinProfile(Builtin b) {
    switch (b) {
        case Builtin::Srgb: {
            static const Profile p = rgbBuiltin(
                builtinDescription(Builtin::Srgb),
                {{{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}}}, builtinD65(), Curve::srgbTrc());
            return p;
        }
        case Builtin::DisplayP3: {
            static const Profile p = rgbBuiltin(
                builtinDescription(Builtin::DisplayP3),
                {{{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}}}, builtinD65(), Curve::srgbTrc());
            return p;
        }
        case Builtin::AdobeRgbCompat: {
            static const Profile p = rgbBuiltin(
                builtinDescription(Builtin::AdobeRgbCompat),
                {{{0.64, 0.33}, {0.21, 0.71}, {0.15, 0.06}}}, builtinD65(),
                Curve::gammaValue(563.0 / 256.0));
            return p;
        }
        case Builtin::ProPhotoCompat: {
            static const Profile p = rgbBuiltin(
                builtinDescription(Builtin::ProPhotoCompat),
                {{{0.7347, 0.2653}, {0.1596, 0.8404}, {0.0366, 0.0001}}}, kD50, Curve::gammaValue(1.8));
            return p;
        }
        case Builtin::LinearSrgb: {
            static const Profile p = rgbBuiltin(
                builtinDescription(Builtin::LinearSrgb),
                {{{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}}}, builtinD65(), Curve::gammaValue(1.0));
            return p;
        }
        case Builtin::Rec2020: {
            // Rec. 709 OETF 逆：V = 1.099 L^0.45 − 0.099（L ≥ 0.018），以下 4.5 L。
            const double p7[7] = {1.0 / 0.45, 1.0 / 1.099, (1.099 - 1.0) / 1.099, 1.0 / 4.5, 0.081, 0.0, 0.0};
            static const Profile p = rgbBuiltin(
                builtinDescription(Builtin::Rec2020),
                {{{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}}}, builtinD65(),
                Curve::parametric(3, p7));
            return p;
        }
        case Builtin::GrayGamma22: {
            static const Profile p = grayBuiltin(builtinDescription(Builtin::GrayGamma22), Curve::gammaValue(2.2));
            return p;
        }
        case Builtin::SGray: {
            static const Profile p = grayBuiltin(builtinDescription(Builtin::SGray), Curve::srgbTrc());
            return p;
        }
        case Builtin::LabD50: {
            static const Profile p = labBuiltin();
            return p;
        }
        case Builtin::CoatedCmyk: {
            static const Profile p = coatedCmykBuiltin();
            return p;
        }
    }
    static const Profile fallback = builtinProfile(Builtin::Srgb);
    return fallback;
}

const Profile* defaultProfileFor(ColorSpace cs) {
    switch (cs) {
        case ColorSpace::Rgb: return &builtinProfile(Builtin::Srgb);
        case ColorSpace::Gray: return &builtinProfile(Builtin::SGray);
        case ColorSpace::Cmyk: return &builtinProfile(Builtin::CoatedCmyk);
        case ColorSpace::Lab: return &builtinProfile(Builtin::LabD50);
        default: return nullptr;
    }
}

const Profile& builtinLinearGray() {
    static const Profile p = grayBuiltin("Linear Gray (Montage)", Curve::gammaValue(1.0));
    return p;
}

}  // namespace cms
}  // namespace montage
