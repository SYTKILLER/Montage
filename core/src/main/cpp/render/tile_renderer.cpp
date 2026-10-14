#include "render/tile_renderer.h"

#include "tiles/selection_outline.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>

#include "render/shaders.h"

namespace montage {
namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Render";

const char* const kBlendBodies[kBlendModeCount] = {
    shaders::kBlendNormalBody, shaders::kBlendDarkenBody, shaders::kBlendMultiplyBody,
    shaders::kBlendColorBurnBody, shaders::kBlendLinearBurnBody, shaders::kBlendLightenBody,
    shaders::kBlendScreenBody, shaders::kBlendColorDodgeBody, shaders::kBlendLinearDodgeBody,
    shaders::kBlendOverlayBody, shaders::kBlendSoftLightBody, shaders::kBlendHardLightBody,
    shaders::kBlendVividLightBody, shaders::kBlendLinearLightBody, shaders::kBlendPinLightBody,
    shaders::kBlendHardMixBody, shaders::kBlendDifferenceBody, shaders::kBlendExclusionBody,
    shaders::kBlendSubtractBody, shaders::kBlendDivideBody, shaders::kBlendHueBody,
    shaders::kBlendSaturationBody, shaders::kBlendColorBody, shaders::kBlendLuminosityBody,
};
}  // namespace

GLuint TileRenderer::buildProgram(const char* vertSrc, const char* fragSrc) {
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint sh = glCreateShader(type);
        glShaderSource(sh, 1, &src, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (ok != GL_TRUE) {
            char log[512] = {0};
            glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
            OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "shader compile failed: %{public}s", log);
            glDeleteShader(sh);
            return 0;
        }
        return sh;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, vertSrc);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragSrc);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return 0;
    }
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[512] = {0};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "program link failed: %{public}s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

GLuint TileRenderer::buildBlendProgram(int mode) {
    // 拼装：common 模板中的占位 blendB 函数体替换为对应模式实现（R3.3 独立 program）
    const std::string placeholder =
        "vec3 blendB(vec3 Cb, vec3 Cs);   // %%BODY%%";
    const std::string fn =
        std::string("vec3 blendB(vec3 Cb, vec3 Cs) {") + kBlendBodies[mode] + "\n}";
    std::string src(shaders::kBlendCommon);
    const size_t at = src.find(placeholder);
    if (at == std::string::npos) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "blend template placeholder missing");
        return 0;
    }
    src.replace(at, placeholder.size(), fn);
    return buildProgram(shaders::kQuadVert, src.c_str());
}

bool TileRenderer::ensureFbos(int32_t vw, int32_t vh) {
    if (fboW_ == vw && fboH_ == vh && layerFbo_ != 0) {
        return true;
    }
    destroyFbos();
    if (!float16Probed_) {
        const GLubyte* ext = glGetString(GL_EXTENSIONS);
        float16_ = ext != nullptr && std::strstr(reinterpret_cast<const char*>(ext),
                                                 "EXT_color_buffer_float") != nullptr;
        float16Probed_ = true;
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "acc FBO format: %{public}s",
                     float16_ ? "RGBA16F" : "RGBA8");
    }
    const GLint internal = float16_ ? GL_RGBA16F : GL_RGBA8;
    const GLenum type = float16_ ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
    auto makeFbo = [&](GLuint* fbo, GLuint* tex) -> bool {
        glGenFramebuffers(1, fbo);
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, vw, vh, 0, GL_RGBA, type, nullptr);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return st == GL_FRAMEBUFFER_COMPLETE;
    };
    bool ok = makeFbo(&accFbo_[0], &accTex_[0]);
    ok = makeFbo(&accFbo_[1], &accTex_[1]) && ok;
    ok = makeFbo(&layerFbo_, &layerTex_) && ok;
    ok = makeFbo(&clipFbo_, &clipTex_) && ok;
    ok = makeFbo(&baseFbo_, &baseTex_) && ok;
    for (int g = 0; g < kGroupDepth; ++g) {
        ok = makeFbo(&groupAccFbo_[g][0], &groupAccTex_[g][0]) && ok;
        ok = makeFbo(&groupAccFbo_[g][1], &groupAccTex_[g][1]) && ok;
    }
    // 256×1 LUT 纹理（RGBA8；三通道同表，LINEAR 平滑插值）
    glGenTextures(1, &lutTex_);
    glBindTexture(GL_TEXTURE_2D, lutTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (!ok) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "FBO incomplete");
        destroyFbos();
        return false;
    }
    fboW_ = vw;
    fboH_ = vh;
    return true;
}

void TileRenderer::destroyFbos() {
    for (int i = 0; i < 2; ++i) {
        if (accFbo_[i] != 0) glDeleteFramebuffers(1, &accFbo_[i]);
        if (accTex_[i] != 0) glDeleteTextures(1, &accTex_[i]);
        accFbo_[i] = 0;
        accTex_[i] = 0;
    }
    if (layerFbo_ != 0) glDeleteFramebuffers(1, &layerFbo_);
    if (layerTex_ != 0) glDeleteTextures(1, &layerTex_);
    layerFbo_ = 0;
    layerTex_ = 0;
    if (clipFbo_ != 0) glDeleteFramebuffers(1, &clipFbo_);
    if (clipTex_ != 0) glDeleteTextures(1, &clipTex_);
    clipFbo_ = 0;
    clipTex_ = 0;
    if (baseFbo_ != 0) glDeleteFramebuffers(1, &baseFbo_);
    if (baseTex_ != 0) glDeleteTextures(1, &baseTex_);
    baseFbo_ = 0;
    baseTex_ = 0;
    if (lutTex_ != 0) glDeleteTextures(1, &lutTex_);
    lutTex_ = 0;
    fboW_ = fboH_ = 0;
}

void TileRenderer::ensureInit() {
    if (inited_) {
        return;
    }
    checkerProg_ = buildProgram(shaders::kQuadVert, shaders::kCheckerFrag);
    tileProg_ = buildProgram(shaders::kQuadVert, shaders::kTileFrag);
    plainProg_ = buildProgram(shaders::kQuadVert, shaders::kPlainFrag);
    clipProg_ = buildProgram(shaders::kQuadVert, shaders::kClipFrag);
    adjustProg_ = buildProgram(shaders::kQuadVert, shaders::kAdjustFrag);
    antsProg_ = buildProgram(shaders::kAntsVert, shaders::kAntsFrag);
    qmProg_ = buildProgram(shaders::kQuadVert, shaders::kQuickMaskFrag);
    for (int m = 0; m < kBlendModeCount; ++m) {
        blendProgs_[m] = buildBlendProgram(m);
        if (blendProgs_[m] == 0) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "blend program %{public}d failed", m);
        }
    }
    if (checkerProg_ == 0 || tileProg_ == 0 || plainProg_ == 0 || clipProg_ == 0 ||
        adjustProg_ == 0 || antsProg_ == 0 || qmProg_ == 0 || blendProgs_[0] == 0) {
        return;
    }
    if (antsVao_ == 0) {
        glGenBuffers(1, &antsVbo_);
        glGenVertexArrays(1, &antsVao_);
    }
    if (qmZeroTex_ == 0) {
        // 1×1 零灰度纹理：快速蒙版缺瓦片 = 未选中 = alpha 0.5 满红
        glGenTextures(1, &qmZeroTex_);
        glBindTexture(GL_TEXTURE_2D, qmZeroTex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const uint8_t zero[4] = {0, 0, 0, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, zero);
    }
    const float quad[8] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

    // 03 §3.4：GL 能力登记（模拟器实证，设备就绪后回归）
    const GLubyte* glVer = glGetString(GL_VERSION);
    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag, "GL init: ver=%{public}s maxTex=%{public}d",
                 glVer != nullptr ? reinterpret_cast<const char*>(glVer) : "?", static_cast<int>(maxTex));
    inited_ = true;
}

void TileRenderer::invalidate() {
    for (auto& kv : cache_) {
        if (kv.second.tex != 0) {
            glDeleteTextures(1, &kv.second.tex);
        }
    }
    for (auto& kv : strokeTex_) {
        if (kv.second != 0) {
            glDeleteTextures(1, &kv.second);
        }
    }
    strokeTex_.clear();
    cache_.clear();
    destroyFbos();
    if (checkerProg_ != 0) glDeleteProgram(checkerProg_);
    if (tileProg_ != 0) glDeleteProgram(tileProg_);
    if (plainProg_ != 0) glDeleteProgram(plainProg_);
    if (clipProg_ != 0) glDeleteProgram(clipProg_);
    if (adjustProg_ != 0) glDeleteProgram(adjustProg_);
    if (antsProg_ != 0) glDeleteProgram(antsProg_);
    if (qmProg_ != 0) glDeleteProgram(qmProg_);
    for (int m = 0; m < kBlendModeCount; ++m) {
        if (blendProgs_[m] != 0) glDeleteProgram(blendProgs_[m]);
        blendProgs_[m] = 0;
    }
    checkerProg_ = tileProg_ = plainProg_ = clipProg_ = adjustProg_ = antsProg_ = qmProg_ = 0;
    if (qmZeroTex_ != 0) glDeleteTextures(1, &qmZeroTex_);
    qmZeroTex_ = 0;
    for (auto& kv : qmCache_) {
        if (kv.second.tex != 0) {
            glDeleteTextures(1, &kv.second.tex);
        }
    }
    qmCache_.clear();
    for (auto& kv : qmStrokeTex_) {
        if (kv.second != 0) {
            glDeleteTextures(1, &kv.second);
        }
    }
    qmStrokeTex_.clear();
    if (antsVbo_ != 0) glDeleteBuffers(1, &antsVbo_);
    if (antsVao_ != 0) glDeleteVertexArrays(1, &antsVao_);
    antsVbo_ = antsVao_ = 0;
    antsVerts_ = 0;
    antsBuiltFor_ = nullptr;
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    vbo_ = vao_ = 0;
    for (int g = 0; g < kGroupDepth; ++g) {
        for (int i = 0; i < 2; ++i) {
            if (groupAccFbo_[g][i] != 0) glDeleteFramebuffers(1, &groupAccFbo_[g][i]);
            if (groupAccTex_[g][i] != 0) glDeleteTextures(1, &groupAccTex_[g][i]);
            groupAccFbo_[g][i] = 0;
            groupAccTex_[g][i] = 0;
        }
    }
    inited_ = false;
}

void TileRenderer::drawQuad(GLuint program, float rx, float ry, float rw, float rh, float zoom,
                            float panX, float panY, float vw, float vh, const double* xform) {
    glUseProgram(program);
    const GLint rectLoc = glGetUniformLocation(program, "uRect");
    const GLint viewLoc = glGetUniformLocation(program, "uView");
    const GLint vpLoc = glGetUniformLocation(program, "uViewport");
    glUniform4f(rectLoc, rx, ry, rw, rh);
    glUniform4f(viewLoc, zoom, panX, panY, 0.0f);
    glUniform2f(vpLoc, vw, vh);
    // uXform：行主序 [a,b,c, d,e,f]（x'=a·x+b·y+c）→ GL 列主序 mat3
    float m[9];
    if (xform != nullptr) {
        m[0] = static_cast<float>(xform[0]);
        m[1] = static_cast<float>(xform[3]);
        m[2] = 0.0f;
        m[3] = static_cast<float>(xform[1]);
        m[4] = static_cast<float>(xform[4]);
        m[5] = 0.0f;
        m[6] = static_cast<float>(xform[2]);
        m[7] = static_cast<float>(xform[5]);
        m[8] = 1.0f;
    } else {
        m[0] = 1.0f;
        m[1] = 0.0f;
        m[2] = 0.0f;
        m[3] = 0.0f;
        m[4] = 1.0f;
        m[5] = 0.0f;
        m[6] = 0.0f;
        m[7] = 0.0f;
        m[8] = 1.0f;
    }
    glUniformMatrix3fv(glGetUniformLocation(program, "uXform"), 1, GL_FALSE, m);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void TileRenderer::drawTiles(const Layer& layer, float zoom, float panX, float panY, float vw,
                             float vh, const StrokeOverlay* stroke, const double* xform) {
    const std::shared_ptr<const TileGrid> effective = layer.effectivePixels();
    if (effective == nullptr) {
        return;
    }
    const TileGrid& grid = *effective;
    if (grid.cols == 0 || grid.rows == 0) {
        return;
    }
    const float ox = static_cast<float>(layer.effectiveOriginX());
    const float oy = static_cast<float>(layer.effectiveOriginY());

    // 视口求交（03 §6 瓦片剔除）：doc 可见范围 ∩ 图层变换范围
    const float docX0 = panX;
    const float docY0 = panY;
    const float docX1 = panX + vw / zoom;
    const float docY1 = panY + vh / zoom;
    uint32_t tx0 = static_cast<uint32_t>(std::max(0.0f, std::floor((docX0 - ox) / kTileSize)));
    uint32_t ty0 = static_cast<uint32_t>(std::max(0.0f, std::floor((docY0 - oy) / kTileSize)));
    uint32_t tx1 = std::min(grid.cols - 1u, static_cast<uint32_t>(
        std::max(0.0f, std::floor((docX1 - ox) / kTileSize))));
    uint32_t ty1 = std::min(grid.rows - 1u, static_cast<uint32_t>(
        std::max(0.0f, std::floor((docY1 - oy) / kTileSize))));
    const bool wantNearest = zoom >= 2.0f;  // 03 §6：≥200% 切 NEAREST（crispZoom）
    // M8.1 变换预览：xform 层不做视口剔除（变换后 bbox 计算复杂且预览层瓦片有限），
    // 稀疏网格空白瓦片循环内天然跳过
    if (xform != nullptr) {
        tx0 = 0;
        ty0 = 0;
        tx1 = grid.cols - 1u;
        ty1 = grid.rows - 1u;
    }

    glUseProgram(tileProg_);
    glUniform1i(glGetUniformLocation(tileProg_, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);

    for (uint32_t ty = ty0; ty <= ty1; ++ty) {
        for (uint32_t tx = tx0; tx <= tx1; ++tx) {
            const uint32_t idx = ty * grid.cols + tx;
            // 绘制中的笔画瓦片：逐帧直传（独立纹理，不进 LRU）
            if (stroke != nullptr && stroke->active && stroke->layerId == layer.id &&
                stroke->gridCols == grid.cols && stroke->gridRows == grid.rows) {
                auto st = stroke->tiles->find(idx);
                if (st != stroke->tiles->end()) {
                    uint64_t skey = (static_cast<uint64_t>(layer.id) << 32) | idx;
                    uint32_t& tex = strokeTex_[skey];
                    if (tex == 0) {
                        glGenTextures(1, &tex);
                    }
                    glBindTexture(GL_TEXTURE_2D, tex);
                    const GLint f0 = wantNearest ? GL_NEAREST : GL_LINEAR;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f0);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTileSize, kTileSize, 0, GL_RGBA,
                                 GL_UNSIGNED_BYTE, st->second.data());
                    drawQuad(tileProg_, ox + static_cast<float>(tx * kTileSize),
                             oy + static_cast<float>(ty * kTileSize),
                             static_cast<float>(kTileSize), static_cast<float>(kTileSize), zoom,
                             panX, panY, static_cast<float>(vw), static_cast<float>(vh), xform);
                    continue;
                }
            }
            auto it = grid.tiles.find(idx);
            if (it == grid.tiles.end() || it->second == nullptr) {
                continue;  // 稀疏：空白瓦片不存在
            }
            const Tile* tile = it->second.get();
            const uint64_t key = (static_cast<uint64_t>(layer.id) << 32) | idx;
            CacheEntry& entry = cache_[key];
            if (entry.tilePtr != tile || entry.tex == 0) {
                // （重）上传：数据源一律经 PixelSource::mapCpu()
                if (entry.tex != 0) {
                    glDeleteTextures(1, &entry.tex);
                    entry.tex = 0;
                }
                const uint8_t* pixels = tile->pixels->mapCpu();
                if (pixels == nullptr) {
                    continue;
                }
                glGenTextures(1, &entry.tex);
                glBindTexture(GL_TEXTURE_2D, entry.tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                // 必须显式设置 MIN_FILTER：默认 NEAREST_MIPMAP_LINEAR 且无 mip 链 → 纹理
                // incomplete → 采样返回黑（M1 实测坑；glError 不报）
                const GLint filter0 = wantNearest ? GL_NEAREST : GL_LINEAR;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter0);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTileSize, kTileSize, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, pixels);
                tile->pixels->unmap();
                entry.tilePtr = tile;
                entry.nearest = wantNearest;
            }
            if (entry.nearest != wantNearest) {
                glBindTexture(GL_TEXTURE_2D, entry.tex);
                const GLint filter = wantNearest ? GL_NEAREST : GL_LINEAR;
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                entry.nearest = wantNearest;
            }
            entry.lastFrame = frame_;
            glBindTexture(GL_TEXTURE_2D, entry.tex);
            drawQuad(tileProg_, ox + static_cast<float>(tx * kTileSize),
                     oy + static_cast<float>(ty * kTileSize),
                     static_cast<float>(kTileSize), static_cast<float>(kTileSize), zoom, panX, panY,
                     static_cast<float>(vw), static_cast<float>(vh), xform);
        }
    }
}

void TileRenderer::uploadLut(const float* lut256) {
    // lut256: 256 个 0..1 值 → RGBA8 三通道同表
    uint8_t px[256 * 4];
    for (int i = 0; i < 256; ++i) {
        const uint8_t v = static_cast<uint8_t>(std::lround(
            std::min(1.0f, std::max(0.0f, lut256[i])) * 255.0f));
        px[i * 4] = v;
        px[i * 4 + 1] = v;
        px[i * 4 + 2] = v;
        px[i * 4 + 3] = 255;
    }
    glBindTexture(GL_TEXTURE_2D, lutTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

void TileRenderer::uploadLutRgba(const float r[256], const float g[256], const float b[256]) {
    // M9a：三通道独立 LUT（ColorBalance 等）；r/g/b 各 256 个 0..1 值
    uint8_t px[256 * 4];
    for (int i = 0; i < 256; ++i) {
        px[i * 4] = static_cast<uint8_t>(std::lround(
            std::min(1.0f, std::max(0.0f, r[i])) * 255.0f));
        px[i * 4 + 1] = static_cast<uint8_t>(std::lround(
            std::min(1.0f, std::max(0.0f, g[i])) * 255.0f));
        px[i * 4 + 2] = static_cast<uint8_t>(std::lround(
            std::min(1.0f, std::max(0.0f, b[i])) * 255.0f));
        px[i * 4 + 3] = 255;
    }
    glBindTexture(GL_TEXTURE_2D, lutTex_);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

// Levels/Curves → 256 LUT（v1：Curves 控制点线性插值）

void TileRenderer::rebuildAnts(const Document& doc) {
    antsVerts_ = 0;
    antsBuiltFor_ = doc.selection.get();
    if (doc.selection == nullptr || antsProg_ == 0 || antsVao_ == 0) {
        return;
    }
    std::vector<tiles::OutlinePolyline> loops;
    if (!tiles::extractSelectionOutline(*doc.selection, loops)) {
        return;
    }
    // 每段展开 6 顶点（两三角形）：p1, p2, t, side
    std::vector<float> verts;
    verts.reserve(loops.size() * 4 * 36);
    for (const tiles::OutlinePolyline& poly : loops) {
        const size_t n = poly.x.size();
        for (size_t i = 0; i < n; ++i) {
            const float x1 = poly.x[i];
            const float y1 = poly.y[i];
            const size_t j = (i + 1) % n;
            const float x2 = poly.x[j];
            const float y2 = poly.y[j];
            // strip 顺序：(-1,0)(+1,0)(-1,1) / (+1,0)(+1,1)(-1,1)
            const float data[6][6] = {
                {x1, y1, x2, y2, 0.0f, -1.0f},
                {x1, y1, x2, y2, 0.0f, +1.0f},
                {x1, y1, x2, y2, 1.0f, -1.0f},
                {x1, y1, x2, y2, 0.0f, +1.0f},
                {x1, y1, x2, y2, 1.0f, +1.0f},
                {x1, y1, x2, y2, 1.0f, -1.0f},
            };
            for (const auto& v : data) {
                verts.insert(verts.end(), v, v + 6);
            }
        }
    }
    antsVerts_ = static_cast<GLsizei>(verts.size() / 6);
    if (antsVerts_ == 0) {
        return;
    }
    glBindVertexArray(antsVao_);
    glBindBuffer(GL_ARRAY_BUFFER, antsVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    // aP1
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(0));
    // aP2
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));
    // aT
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(4 * sizeof(float)));
    // aSide
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          reinterpret_cast<void*>(5 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void TileRenderer::drawAnts(float phase) {
    if (antsVerts_ == 0 || antsProg_ == 0 || antsVao_ == 0) {
        return;
    }
    glDisable(GL_BLEND);
    glUseProgram(antsProg_);
    glUniform1f(glGetUniformLocation(antsProg_, "uPhase"), phase);
    glBindVertexArray(antsVao_);
    glDrawArrays(GL_TRIANGLES, 0, antsVerts_);
    glBindVertexArray(0);
}

// M10d 快速蒙版红 overlay：文档域逐瓦片单趟绘制（每像素恰画一次，同色半透明不叠画）——
// 草稿瓦片（笔画中）> 通道瓦片 > 1×1 零纹理（缺瓦片 = 未选中 = 满红）。
void TileRenderer::drawQuickMask(const Document& doc, float zoom, float panX, float panY,
                                 float vw, float vh, const StrokeOverlay* qmStroke) {
    if (doc.quickMask == nullptr || qmProg_ == 0 || qmZeroTex_ == 0) {
        return;
    }
    const TileGrid& qm = *doc.quickMask;
    if (qm.cols == 0 || qm.rows == 0) {
        return;
    }
    if (qmStroke == nullptr || !qmStroke->active || !qmStroke->quickMask) {
        // 笔画结束：清草稿纹理（逐帧直传生命周期）
        for (auto& kv : qmStrokeTex_) {
            if (kv.second != 0) {
                glDeleteTextures(1, &kv.second);
            }
        }
        qmStrokeTex_.clear();
    }
    glUseProgram(qmProg_);
    glUniform1i(glGetUniformLocation(qmProg_, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 视口求交（同 drawTiles 剔除）：可见瓦片范围
    const float docX0 = panX;
    const float docY0 = panY;
    const float docX1 = panX + vw / zoom;
    const float docY1 = panY + vh / zoom;
    const uint32_t tx0 = static_cast<uint32_t>(std::max(0.0f, std::floor(docX0 / kTileSize)));
    const uint32_t ty0 = static_cast<uint32_t>(std::max(0.0f, std::floor(docY0 / kTileSize)));
    const uint32_t tx1 = std::min(qm.cols - 1u, static_cast<uint32_t>(
        std::max(0.0f, std::floor(docX1 / kTileSize))));
    const uint32_t ty1 = std::min(qm.rows - 1u, static_cast<uint32_t>(
        std::max(0.0f, std::floor(docY1 / kTileSize))));

    for (uint32_t ty = ty0; ty <= ty1; ++ty) {
        for (uint32_t tx = tx0; tx <= tx1; ++tx) {
            const uint32_t idx = ty * qm.cols + tx;
            const std::vector<uint8_t>* strokeTile = nullptr;
            if (qmStroke != nullptr && qmStroke->active && qmStroke->quickMask &&
                qmStroke->gridCols == qm.cols && qmStroke->gridRows == qm.rows) {
                auto st = qmStroke->tiles->find(idx);
                if (st != qmStroke->tiles->end()) {
                    strokeTile = &st->second;
                }
            }
            if (strokeTile != nullptr) {
                // 草稿瓦片逐帧直传（同像素笔画 overlay 语义）
                uint32_t& tex = qmStrokeTex_[idx];
                if (tex == 0) {
                    glGenTextures(1, &tex);
                }
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTileSize, kTileSize, 0, GL_RGBA,
                             GL_UNSIGNED_BYTE, strokeTile->data());
            } else {
                auto it = qm.tiles.find(idx);
                if (it == qm.tiles.end() || it->second == nullptr) {
                    glBindTexture(GL_TEXTURE_2D, qmZeroTex_);
                } else {
                    const Tile* tile = it->second.get();
                    QmCacheEntry& entry = qmCache_[tile];
                    if (entry.tex == 0) {
                        const uint8_t* pixels = tile->pixels->mapCpu();
                        if (pixels != nullptr) {
                            glGenTextures(1, &entry.tex);
                            glBindTexture(GL_TEXTURE_2D, entry.tex);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kTileSize, kTileSize, 0,
                                         GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                            tile->pixels->unmap();
                        }
                    } else {
                        glBindTexture(GL_TEXTURE_2D, entry.tex);
                    }
                    entry.lastFrame = frame_;
                }
            }
            drawQuad(qmProg_, static_cast<float>(tx * kTileSize),
                     static_cast<float>(ty * kTileSize),
                     static_cast<float>(kTileSize), static_cast<float>(kTileSize), zoom, panX,
                     panY, vw, vh);
        }
    }
    // 陈旧缓存逐出（提交后 Tile* 换新 → 旧键 8 帧老化释放；键仅作身份比较不解引用）
    if (qmCache_.size() > 64) {
        std::vector<const Tile*> victims;
        for (auto& kv : qmCache_) {
            if (frame_ - kv.second.lastFrame > 8) {
                victims.push_back(kv.first);
            }
        }
        for (const Tile* k : victims) {
            if (qmCache_[k].tex != 0) {
                glDeleteTextures(1, &qmCache_[k].tex);
            }
            qmCache_.erase(k);
        }
    }
}



// M14a：组子纹理缓冲按需就绪（ensureFbos 同尺寸）
bool TileRenderer::ensureGroupFbos(int depth, int32_t vw, int32_t vh) {
    return fboW_ == vw && fboH_ == vh && depth >= 0 && depth < kGroupDepth &&
           groupAccFbo_[depth][0] != 0;
}

// M14a：合成一段层到当前 acc（组递归见分支；单层两趟与原 drawFrame 循环一致）
int TileRenderer::composeLayerRange(const Document& doc, int from, int to, const Viewport& vp,
                                    int32_t vw, int32_t vh, const StrokeOverlay* stroke,
                                    const LayerXformPreview* preview, int accIdx,
                                    bool baseTexValid, int depth, int& composited) {
    const float zoom = static_cast<float>(clampZoom(vp.zoom));
    const float panX = static_cast<float>(vp.panX);
    const float panY = static_cast<float>(vp.panY);
    const float vwF = static_cast<float>(vw);
    const float vhF = static_cast<float>(vh);
    for (int li = from; li < to; ++li) {
        const Layer& layer = doc.layers[li];
        // M14a 组节点：collapsed → 跳过整段；穿透（Normal+不透明 1）→ 子层直接进当前
        // acc（递归共享 ping-pong）；否则组子纹理合成后按组 blend/opacity 混入。
        if (layer.isGroup) {
            const int childEnd = groupChildEnd(doc.layers, li);
            if (!layer.collapsed && childEnd > li + 1) {
                if (layer.blendMode == BlendMode::Normal && layer.opacity >= 0.999) {
                    accIdx = composeLayerRange(doc, li + 1, childEnd, vp, vw, vh, stroke,
                                               preview, accIdx, false, depth, composited);
                } else if (depth + 1 < kGroupDepth &&
                           ensureGroupFbos(depth, vw, vh)) {
                    glBindFramebuffer(GL_FRAMEBUFFER, groupAccFbo_[depth][0]);
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    glBindFramebuffer(GL_FRAMEBUFFER, groupAccFbo_[depth][1]);
                    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                    glClear(GL_COLOR_BUFFER_BIT);
                    int subAcc = composeLayerRange(doc, li + 1, childEnd, vp, vw, vh, stroke,
                                                   preview, 0, false, depth + 1, composited);
                    const int dstIdx = accIdx;
                    const int outIdx = 1 - accIdx;
                    glBindFramebuffer(GL_FRAMEBUFFER, accFbo_[outIdx]);
                    glDisable(GL_BLEND);
                    const int mode = static_cast<int>(layer.blendMode);
                    GLuint blendProg = (mode >= 0 && mode < kBlendModeCount &&
                                        blendProgs_[mode] != 0)
                                           ? blendProgs_[mode]
                                           : blendProgs_[0];
                    glUseProgram(blendProg);
                    glUniform1i(glGetUniformLocation(blendProg, "uSrc"), 0);
                    glUniform1i(glGetUniformLocation(blendProg, "uDst"), 1);
                    glUniform1f(glGetUniformLocation(blendProg, "uOpacity"),
                                static_cast<float>(layer.opacity));
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, groupAccTex_[depth][subAcc]);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, accTex_[dstIdx]);
                    drawQuad(blendProg, 0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh),
                             1.0f, 0.0f, 0.0f, static_cast<float>(vw), static_cast<float>(vh));
                    accIdx = outIdx;
                }
                // 超深嵌套降级：组不可渲染（登记 kGroupDepth=8 上限）
            }
            continue;
        }
        if (layer.adjustment != nullptr) {
            // M6a：调整层——acc 全帧经 LUT 调色（ping-pong），opacity lerp；忽略剪贴/混合模式（v1）
            if (!layer.visible) {
                continue;
            }
            composited++;
            float lr[256], lg[256], lb[256];
            buildAdjustmentRgbLut(*layer.adjustment, lr, lg, lb);
            uploadLutRgba(lr, lg, lb);
            const int dstIdx = accIdx;
            const int outIdx = 1 - accIdx;
            glBindFramebuffer(GL_FRAMEBUFFER, accFbo_[outIdx]);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_BLEND);
            glUseProgram(adjustProg_);
            glUniform1i(glGetUniformLocation(adjustProg_, "uDst"), 0);
            glUniform1i(glGetUniformLocation(adjustProg_, "uLut"), 1);
            glUniform1f(glGetUniformLocation(adjustProg_, "uOpacity"),
                        static_cast<float>(layer.opacity));
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, accTex_[dstIdx]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, lutTex_);
            drawQuad(adjustProg_, 0.0f, 0.0f, vwF, vhF, 1.0f, 0.0f, 0.0f, vwF, vhF);
            accIdx = outIdx;
            continue;
        }
        if (!layer.visible || layer.effectivePixels() == nullptr) {
            // 非剪贴层缺席 → 其上剪贴组无有效基（PS：基隐藏则剪贴组隐藏）
            if (!layer.clipping) {
                baseTexValid = false;
            }
            continue;
        }
        if (layer.clipping && !baseTexValid) {
            continue;  // 无有效基（基隐藏/空/栈底剪贴）→ 整层不渲染（PS 语义）
        }
        composited++;
        const int mode = static_cast<int>(layer.blendMode);
        GLuint blendProg = (mode >= 0 && mode < kBlendModeCount && blendProgs_[mode] != 0)
                               ? blendProgs_[mode]
                               : blendProgs_[0];
        // Pass A：图层可见瓦片 → layerFBO（直通 alpha）
        glBindFramebuffer(GL_FRAMEBUFFER, layerFbo_);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_BLEND);
        drawTiles(layer, zoom, panX, panY, vw, vh,
                  stroke != nullptr && stroke->layerId == layer.id ? stroke : nullptr,
                  preview != nullptr && preview->active && preview->layerId == layer.id
                      ? preview->m : nullptr);

        GLuint srcTex = layerTex_;
        if (!layer.clipping) {
            // 剪贴基快照：紧随其后的剪贴层组以本层 assemble alpha 为蒙（M5b-2）
            baseTexValid = false;
            if (li + 1 < doc.layers.size() && doc.layers[li + 1].clipping) {
                glBindFramebuffer(GL_FRAMEBUFFER, baseFbo_);
                glDisable(GL_BLEND);
                glUseProgram(plainProg_);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, layerTex_);
                drawQuad(plainProg_, 0.0f, 0.0f, vwF, vhF, 1.0f, 0.0f, 0.0f, vwF, vhF);
                baseTexValid = true;
            }
        } else {
            // 剪贴层：alpha × 基 alpha（中间趟，Pass B 用乘积纹理）
            glBindFramebuffer(GL_FRAMEBUFFER, clipFbo_);
            glDisable(GL_BLEND);
            glUseProgram(clipProg_);
            glUniform1i(glGetUniformLocation(clipProg_, "uSrc"), 0);
            glUniform1i(glGetUniformLocation(clipProg_, "uClip"), 1);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, layerTex_);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, baseTex_);
            drawQuad(clipProg_, 0.0f, 0.0f, vwF, vhF, 1.0f, 0.0f, 0.0f, vwF, vhF);
            srcTex = clipTex_;
        }

        // Pass B：blend(src=layerTex/clipTex, dst=acc) 全屏 → acc 另一侧（R3.1：视口变仍全帧重绘）
        const int dstIdx = accIdx;
        const int outIdx = 1 - accIdx;
        glBindFramebuffer(GL_FRAMEBUFFER, accFbo_[outIdx]);
        glDisable(GL_BLEND);
        glUseProgram(blendProg);
        glUniform1i(glGetUniformLocation(blendProg, "uSrc"), 0);
        glUniform1i(glGetUniformLocation(blendProg, "uDst"), 1);
        glUniform1f(glGetUniformLocation(blendProg, "uOpacity"), static_cast<float>(layer.opacity));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, srcTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, accTex_[dstIdx]);
        // uRect/uView/uViewport 必须由 drawQuad 设置（缺省 0 → quad 退化不绘制，M2 实测坑）
        drawQuad(blendProg, 0.0f, 0.0f, vwF, vhF, 1.0f, 0.0f, 0.0f, vwF, vhF);
        accIdx = outIdx;
    }
    return accIdx;
}

void TileRenderer::drawFrame(const Document& doc, const Viewport& vp, int32_t vw, int32_t vh,
                             const StrokeOverlay* stroke, const LayerXformPreview* preview,
                             bool overlayOnly) {
    frame_++;
    glViewport(0, 0, vw, vh);
    // 画布外背板：中性灰（05 §7：与主题解耦，对齐 PS 行为）
    glClearColor(0.27f, 0.27f, 0.27f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!inited_ || doc.width == 0 || doc.height == 0) {
        return;
    }
    if (!ensureFbos(vw, vh)) {
        return;
    }
    if (overlayOnly && lastAccIdx_ < 0) {
        overlayOnly = false;  // 尚无可用合成结果（首帧）→ 退化为全帧
    }

    const float zoom = static_cast<float>(clampZoom(vp.zoom));
    const float panX = static_cast<float>(vp.panX);
    const float panY = static_cast<float>(vp.panY);
    const float vwF = static_cast<float>(vw);
    const float vhF = static_cast<float>(vh);

    // ① 自底向上逐层两趟合成（03 §4）：assemble → layerFBO，blend → acc ping-pong
    // M15d：overlayOnly 跳过两趟（acc 纹理持久持有上次合成结果），主合成趟归零
    int accIdx = lastAccIdx_ < 0 ? 0 : lastAccIdx_;
    int composited = 0;
    if (!overlayOnly) {
        for (int i = 0; i < 2; ++i) {
            glBindFramebuffer(GL_FRAMEBUFFER, accFbo_[i]);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        accIdx = 0;
        accIdx = composeLayerRange(doc, 0, static_cast<int>(doc.layers.size()), vp, vw, vh, stroke,
                                   preview, accIdx, false, 0, composited);
        lastAccIdx_ = accIdx;
    }

    // ② 屏幕：灰背板 → doc rect 棋盘格 → acc 终合成（按 doc rect 裁剪）
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, vw, vh);
    glClearColor(0.27f, 0.27f, 0.27f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    drawQuad(checkerProg_, 0.0f, 0.0f, static_cast<float>(doc.width), static_cast<float>(doc.height),
             zoom, panX, panY, vwF, vhF);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, accTex_[accIdx]);
    drawQuad(plainProg_, 0.0f, 0.0f, static_cast<float>(doc.width), static_cast<float>(doc.height),
             zoom, panX, panY, vwF, vhF);

    // M10d 快速蒙版红 overlay：图层合成之上、蚂蚁线之下（QM 模式 selection=null，ants 不画）
    if (doc.quickMask != nullptr) {
        drawQuickMask(doc, zoom, panX, panY, vwF, vhF,
                      stroke != nullptr && stroke->quickMask ? stroke : nullptr);
    }
    glDisable(GL_BLEND);

    // M7b 蚂蚁线：selection 指针身份变化时惰性重建，相位随帧推进
    if (doc.selection.get() != antsBuiltFor_) {
        rebuildAnts(doc);
    }
    if (antsVerts_ > 0) {
        glUseProgram(antsProg_);
        glUniform4f(glGetUniformLocation(antsProg_, "uView"), zoom, panX, panY, 0.0f);
        glUniform2f(glGetUniformLocation(antsProg_, "uViewport"), vwF, vhF);
        drawAnts(static_cast<float>(frame_ % 64) * 0.25f);
    }

    if (frame_ <= 8) {
        OH_LOG_Print(LOG_APP, LOG_INFO, kDomain, kTag,
                     "compose layers=%{public}d composited=%{public}d cache=%{public}d fbo=%{public}dx%{public}d f16=%{public}d",
                     static_cast<int>(doc.layers.size()), composited, static_cast<int>(cache_.size()),
                     fboW_, fboH_, float16_ ? 1 : 0);
    }

    // LRU 逐出（超预算时最久未用先走）
    if (cache_.size() > kCacheCap) {
        std::vector<uint64_t> victims;
        for (auto& kv : cache_) {
            if (frame_ - kv.second.lastFrame > 8) {
                victims.push_back(kv.first);
            }
        }
        std::sort(victims.begin(), victims.end(), [this](uint64_t a, uint64_t b) {
            return cache_[a].lastFrame < cache_[b].lastFrame;
        });
        const size_t target = kCacheCap / 2;
        for (size_t i = 0; i < victims.size() && cache_.size() > target; ++i) {
            if (cache_[victims[i]].tex != 0) {
                glDeleteTextures(1, &cache_[victims[i]].tex);
            }
            cache_.erase(victims[i]);
        }
    }
}

}  // namespace montage
