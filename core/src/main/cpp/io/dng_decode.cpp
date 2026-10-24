#include "io/dng_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <hilog/log.h>

#include "io/raw/raw_color.h"
#include "io/raw/raw_decode.h"
#include "io/raw/raw_internal.h"
#include "io/raw/raw_look.h"
#include "io/raw/raw_preview.h"
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

}  // namespace

bool looksLikeCameraRaw(const std::vector<uint8_t>& bytes) {
    return raw::isRaw(bytes);
}

namespace {
PreviewDecodeFn g_previewDecode = nullptr;
}  // namespace

void setPreviewDecodeHook(PreviewDecodeFn fn) {
    g_previewDecode = fn;
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
        raw::Sensor sensor = raw::decodeRaw(bytes, opts.limits);
        // M23c fit_look：无文件颜色校准的非 DNG 彩色 raw，用内嵌预览 JPEG 拟合
        // per-file 相机 look（矩阵+色调曲线）；拟合失败静默保持中性回退。
        if (g_previewDecode != nullptr && !sensor.isMonochrome() &&
            sensor.format != raw::RawFormat::Dng) {
            std::array<double, 3> neutral{};
            std::vector<std::string> wbWarn;
            if (raw::lookWhiteBalance(sensor, opts, neutral, wbWarn) &&
                !raw::balancedToXyzD50(sensor.color, neutral)) {
                auto prev = raw::embeddedPreview(bytes);
                if (prev.has_value()) {
                    uint32_t rw = 0;
                    uint32_t rh = 0;
                    std::vector<uint8_t> rgb;
                    if (g_previewDecode(bytes.data() + prev->offset, prev->len, rw, rh, rgb)) {
                        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                     "fit_look reference decoded %{public}dx%{public}d",
                                     static_cast<int>(rw), static_cast<int>(rh));
                        raw::LookReference ref;
                        ref.width = rw;
                        ref.height = rh;
                        ref.rgb = std::move(rgb);
                        std::string why;
                        auto look = raw::fitLook(sensor, opts, ref, why);
                        if (look.has_value()) {
                            opts.cameraLook = *look;
                            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                         "fit_look accepted: %{public}dx%{public}d dE=%{public}.2f (fallback %{public}.2f)",
                                         static_cast<int>(rw), static_cast<int>(rh),
                                         look->deltaE, look->deltaEFallback);
                        } else {
                            OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                                         "fit_look rejected: %{public}s", why.c_str());
                        }
                    }
                }
            }
        }
        dev = raw::developSensor(sensor, opts);
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
