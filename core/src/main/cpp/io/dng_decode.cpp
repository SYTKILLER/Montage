#include "io/dng_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <hilog/log.h>

#include "io/raw/raw_color.h"
#include "io/raw/raw_decode.h"
#include "io/raw/raw_types.h"

namespace montage {
namespace io {
namespace {

constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Dng";

// M23a RAW 解码内核（photocraft raw crate C++ 翻译）之上的门面。
// 显影链：内核显影 → 16-bit ProPhoto(gamma 1.8) → 线性化 → ProPhoto→sRGB 矩阵 →
// sRGB 编码 → 用户伽马 → 8-bit TileGrid。显影参数（temp/tint/ev/gamma）语义与
// M15e 面板一致：temp/tint 在解析白平衡上调制通道增益 ±30%、ev 线性 2^ev、
// gamma 作用在 sRGB 编码值上。M23b：cameraJson 非空时追加 Camera Raw 9 级链
// （photocraft camera_raw.rs 语义；在 sRGB 编码域上工作，与其滤镜/预览一致）。

inline float srgbEncode(float v) {
    v = std::min(1.0f, std::max(0.0f, v));
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// ---- 轻量 JSON 数值/串提取（CameraRaw 字段 camelCase；project_open 手写解析器同思路）----

struct JsonScan {
    const char* p;
    const char* end;

    // 找顶层（花括号深度 1）键 key 的值起点；未找到返回 nullptr。
    const char* find(const char* key) const {
        size_t klen = std::strlen(key);
        int depth = 0;
        bool inStr = false;
        for (const char* q = p; q < end; ++q) {
            char c = *q;
            if (inStr) {
                if (c == '\\') {
                    ++q;
                    continue;
                }
                if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') {
                inStr = true;
                // 可能是键名：检查 "key" 后跟 :
                if (depth == 1) {
                    const char* s = q + 1;
                    const char* e = s;
                    while (e < end && *e != '"' && *e != '\\') ++e;
                    if (e < end && *e == '"' && static_cast<size_t>(e - s) == klen && std::memcmp(s, key, klen) == 0) {
                        const char* r = e + 1;
                        while (r < end && *r == ' ') ++r;
                        if (r < end && *r == ':') return r + 1;
                    }
                }
                continue;
            }
            if (c == '{' || c == '[') ++depth;
            else if (c == '}' || c == ']') --depth;
        }
        return nullptr;
    }

    // 键存在（值任意）即真。
    bool has(const char* key) const { return find(key) != nullptr; }

    // 数值（标量）。
    bool num(const char* key, float& out) const {
        const char* v = find(key);
        if (!v) return false;
        char* e = nullptr;
        out = std::strtof(v, &e);
        return e != v;
    }

    // 串（vignetteStyle 等；最多 31 字节）。
    bool str(const char* key, char* out, size_t cap) const {
        const char* v = find(key);
        if (!v) return false;
        while (v < end && *v == ' ') ++v;
        if (v >= end || *v != '"') return false;
        ++v;
        size_t n = 0;
        while (v < end && *v != '"' && n + 1 < cap) out[n++] = *v++;
        out[n] = 0;
        return true;
    }

    // 数组（数值或 [x,y] 点对的扁平表）。
    std::vector<float> arr(const char* key) const {
        std::vector<float> out;
        const char* v = find(key);
        if (!v) return out;
        while (v < end && *v == ' ') ++v;
        if (v >= end || *v != '[') return out;
        ++v;
        while (v < end) {
            while (v < end && (*v == ' ' || *v == ',' || *v == '[' || *v == ']')) ++v;
            if (v >= end || *v == '}' || *v == ']') break;
            char* e = nullptr;
            float f = std::strtof(v, &e);
            if (e == v) break;
            out.push_back(f);
            v = e;
        }
        return out;
    }
};

// CameraRaw JSON（camelCase）→ CameraRaw。未识别字段忽略（向前兼容）。
bool parseCameraJson(const std::string& json, CameraRaw& out) {
    if (json.empty()) return true;
    JsonScan js{json.c_str(), json.c_str() + json.size()};
    auto f = [&](const char* k, float& dst) { float v; if (js.num(k, v)) dst = v; };
    f("temperature", out.temperature);
    f("tint", out.tint);
    f("exposure", out.exposure);
    f("contrast", out.contrast);
    f("highlights", out.highlights);
    f("shadows", out.shadows);
    f("whites", out.whites);
    f("blacks", out.blacks);
    f("texture", out.texture);
    f("clarity", out.clarity);
    f("dehaze", out.dehaze);
    f("vibrance", out.vibrance);
    f("saturation", out.saturation);
    f("curveHighlights", out.curveHighlights);
    f("curveLights", out.curveLights);
    f("curveDarks", out.curveDarks);
    f("curveShadows", out.curveShadows);
    {
        auto v = js.arr("curveSplits");
        for (size_t i = 0; i < v.size() && i < 3; ++i) out.curveSplits[i] = v[i];
    }
    auto readCurve = [&](const char* k, std::vector<std::array<float, 2>>& dst) {
        auto v = js.arr(k);
        if (v.empty()) return;
        dst.clear();
        for (size_t i = 0; i + 1 < v.size(); i += 2) dst.push_back({v[i], v[i + 1]});
    };
    readCurve("pointCurve", out.pointCurve);
    readCurve("pointCurveRed", out.pointCurveRed);
    readCurve("pointCurveGreen", out.pointCurveGreen);
    readCurve("pointCurveBlue", out.pointCurveBlue);
    auto read8 = [&](const char* k, float* dst) {
        auto v = js.arr(k);
        for (size_t i = 0; i < v.size() && i < 8; ++i) dst[i] = v[i];
    };
    read8("hslHue", out.hslHue);
    read8("hslSat", out.hslSat);
    read8("hslLum", out.hslLum);
    auto wheel = [&](const char* base, CameraWheel& w) {
        std::string hKey = std::string(base) + "Hue";
        std::string sKey = std::string(base) + "Sat";
        std::string lKey = std::string(base) + "Lum";
        float v;
        if (js.num(hKey.c_str(), v)) w.hue = v;
        if (js.num(sKey.c_str(), v)) w.sat = v;
        if (js.num(lKey.c_str(), v)) w.lum = v;
    };
    wheel("gradeShadows", out.gradeShadows);
    wheel("gradeMidtones", out.gradeMidtones);
    wheel("gradeHighlights", out.gradeHighlights);
    wheel("gradeGlobal", out.gradeGlobal);
    f("gradeBlending", out.gradeBlending);
    f("gradeBalance", out.gradeBalance);
    f("sharpenAmount", out.sharpenAmount);
    f("sharpenRadius", out.sharpenRadius);
    f("sharpenDetail", out.sharpenDetail);
    f("sharpenMasking", out.sharpenMasking);
    f("noiseLuminance", out.noiseLuminance);
    f("noiseLuminanceDetail", out.noiseLuminanceDetail);
    f("noiseColor", out.noiseColor);
    f("noiseColorDetail", out.noiseColorDetail);
    f("grainAmount", out.grainAmount);
    f("grainSize", out.grainSize);
    f("grainRoughness", out.grainRoughness);
    f("vignetteAmount", out.vignetteAmount);
    f("vignetteMidpoint", out.vignetteMidpoint);
    f("vignetteRoundness", out.vignetteRoundness);
    f("vignetteFeather", out.vignetteFeather);
    f("vignetteHighlights", out.vignetteHighlights);
    {
        char style[32] = {0};
        if (js.str("vignetteStyle", style, sizeof style)) out.vignetteStyle = style;
    }
    f("seed", *reinterpret_cast<float*>(&out.seed));  // 数值通道；seed 小值即够
    // seed 是整数语义，直接 strtof 再转；重读为 uint32：
    {
        const char* v = js.find("seed");
        if (v) {
            char* e = nullptr;
            long lv = std::strtol(v, &e, 10);
            if (e != v && lv >= 0) out.seed = static_cast<uint32_t>(lv);
        }
    }
    f("pixelScale", out.pixelScale);
    // 点曲线校验（新输入拒绝畸形；空 = 线性）。
    for (auto* curve : {&out.pointCurve, &out.pointCurveRed, &out.pointCurveGreen, &out.pointCurveBlue}) {
        if (!validateCurve(*curve).empty()) {
            curve->clear();  // 容错：畸形曲线回退线性，不拒整个显影（滤镜路径 validate 先行）
        }
    }
    return true;
}

}  // namespace

bool looksLikeCameraRaw(const std::vector<uint8_t>& bytes) {
    return raw::isRaw(bytes);
}

bool developDngToGrid(const std::vector<uint8_t>& bytes,
                      const DevelopParams& params,
                      std::shared_ptr<const TileGrid>& out, uint32_t& outW, uint32_t& outH,
                      std::string& err) {
    // M15e：参数 defensively 钳制（桥接侧已钳，核内再钳一遍——显影核不信任入参）
    const float temp = std::min(100.0f, std::max(-100.0f, params.temp));
    const float tint = std::min(100.0f, std::max(-100.0f, params.tint));
    const float ev = std::min(2.0f, std::max(-2.0f, params.ev));
    const float gamma = std::min(2.0f, std::max(0.5f, params.gamma));

    raw::DevelopOptions opts;
    opts.exposure = ev;
    opts.temp = temp;
    opts.tint = tint;
    // M23b：Camera Raw 链（cameraJson 非空时；temp/tint/exposure 由链自管——
    // 避免 v1 增益调制与链的 WB 级重复应用）。
    CameraRaw camera;
    bool cameraOn = false;
    if (!params.cameraJson.empty()) {
        parseCameraJson(params.cameraJson, camera);
        cameraOn = true;
        opts.temp = 0;
        opts.tint = 0;
        opts.exposure = 0;
    }
    raw::Developed dev;
    try {
        dev = raw::develop(bytes, opts);
    } catch (const raw::RawError& e) {
        err = std::string("raw: ") + e.name() + ": " + e.msg;
        return false;
    } catch (const std::exception& e) {
        err = std::string("raw: ") + e.what();
        return false;
    }

    // ProPhoto(gamma 1.8) → 线性 ProPhoto → 线性 sRGB(D65, Bradford) → sRGB 编码。
    const raw::Mat3 ppToSrgb = raw::prophotoD50ToSrgb();
    const uint32_t w = dev.width;
    const uint32_t h = dev.height;
    // M23b：cameraJson 非空时同时在浮点缓冲累积（Camera Raw 链在 sRGB 编码域工作）。
    std::vector<float> fpx;
    if (cameraOn) {
        fpx.assign(static_cast<size_t>(w) * h * 4, 0.0f);
    }
    auto builder = std::make_shared<TileGridBuilder>(
        (w + kTileSize - 1u) / kTileSize, (h + kTileSize - 1u) / kTileSize);
    for (uint32_t ty = 0; ty < (h + kTileSize - 1u) / kTileSize; ++ty) {
        for (uint32_t tx = 0; tx < (w + kTileSize - 1u) / kTileSize; ++tx) {
            EngineBuffer& buf = builder->ensureTile(tx, ty);
            uint8_t* dst = buf.mapCpuWrite();
            const uint32_t x0 = tx * kTileSize;
            const uint32_t y0 = ty * kTileSize;
            const uint32_t copyW = std::min(kTileSize, w - x0);
            const uint32_t copyH = std::min(kTileSize, h - y0);
            for (uint32_t rr = 0; rr < copyH; ++rr) {
                uint8_t* row = dst + static_cast<size_t>(buf.rowBytes()) * rr;
                for (uint32_t x = 0; x < copyW; ++x) {
                    const size_t idx = (static_cast<size_t>(y0 + rr) * w + (x0 + x)) * 3;
                    uint8_t* px = row + static_cast<size_t>(x) * 4;
                    // 线性 ProPhoto 域三通道（gamma 1.8 逆编码）。
                    float lin[3];
                    for (int ch = 0; ch < 3; ++ch) {
                        lin[ch] = std::pow(std::min(1.0f, std::max(0.0f, dev.rgb[idx + ch] / 65535.0f)), 1.8f);
                    }
                    float enc[3];
                    for (int ch = 0; ch < 3; ++ch) {
                        const float s = ppToSrgb[ch][0] * lin[0] + ppToSrgb[ch][1] * lin[1] + ppToSrgb[ch][2] * lin[2];
                        enc[ch] = srgbEncode(s);
                        if (gamma != 1.0f) {
                            // 用户伽马作用在 sRGB 编码值上（端点保持 0/1，γ>1 提亮中间调）
                            enc[ch] = std::pow(std::min(1.0f, std::max(0.0f, enc[ch])), 1.0f / gamma);
                        }
                        px[ch] = static_cast<uint8_t>(std::lround(enc[ch] * 255.0f));
                    }
                    px[3] = 255;
                    if (cameraOn) {
                        const size_t fidx = (static_cast<size_t>(y0 + rr) * w + (x0 + x)) * 4;
                        // cameraOn 时用户伽马不重复应用（链内 exposure/WB 已接管基础调节）。
                        for (int ch = 0; ch < 3; ++ch) {
                            float s = ppToSrgb[ch][0] * lin[0] + ppToSrgb[ch][1] * lin[1] + ppToSrgb[ch][2] * lin[2];
                            fpx[fidx + ch] = srgbEncode(s);
                        }
                        fpx[fidx + 3] = 1.0f;
                    }
                }
            }
        }
    }
    // M23b：Camera Raw 9 级链跑全图浮点缓冲，再写回瓦片。
    if (cameraOn) {
        cameraRawDevelop(fpx.data(), w, h, camera, false);
        for (uint32_t ty = 0; ty < (h + kTileSize - 1u) / kTileSize; ++ty) {
            for (uint32_t tx = 0; tx < (w + kTileSize - 1u) / kTileSize; ++tx) {
                EngineBuffer& buf = builder->ensureTile(tx, ty);
                uint8_t* dst = buf.mapCpuWrite();
                const uint32_t x0 = tx * kTileSize;
                const uint32_t y0 = ty * kTileSize;
                const uint32_t copyW = std::min(kTileSize, w - x0);
                const uint32_t copyH = std::min(kTileSize, h - y0);
                for (uint32_t rr = 0; rr < copyH; ++rr) {
                    uint8_t* row = dst + static_cast<size_t>(buf.rowBytes()) * rr;
                    for (uint32_t x = 0; x < copyW; ++x) {
                        const size_t fidx = (static_cast<size_t>(y0 + rr) * w + (x0 + x)) * 4;
                        uint8_t* px = row + static_cast<size_t>(x) * 4;
                        for (int ch = 0; ch < 3; ++ch) {
                            px[ch] = static_cast<uint8_t>(std::lround(std::clamp(fpx[fidx + ch], 0.0f, 1.0f) * 255.0f));
                        }
                        px[3] = 255;
                    }
                }
            }
        }
    }
    out = builder->publish(1);
    outW = w;
    outH = h;
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                 "developed %{public}s %ux%u wb=(%.2f,%.2f,%.2f) camera=%{public}d "
                 "user=(t=%{public}d ti=%{public}d ev=%{public}d g=%{public}d) warns=%{public}zu",
                 raw::rawFormatName(dev.info.format), w, h,
                 dev.info.wbMultipliers[0], dev.info.wbMultipliers[1], dev.info.wbMultipliers[2],
                 cameraOn ? 1 : 0,
                 static_cast<int>(std::lround(temp)), static_cast<int>(std::lround(tint)),
                 static_cast<int>(std::lround(ev * 100.0f)),
                 static_cast<int>(std::lround(gamma * 100.0f)), dev.warnings.size());
    return true;
}

}  // namespace io
}  // namespace montage
