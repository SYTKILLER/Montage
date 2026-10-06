#include "render/tile_renderer.h"

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
    for (int m = 0; m < kBlendModeCount; ++m) {
        blendProgs_[m] = buildBlendProgram(m);
        if (blendProgs_[m] == 0) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, kDomain, kTag, "blend program %{public}d failed", m);
        }
    }
    if (checkerProg_ == 0 || tileProg_ == 0 || plainProg_ == 0 || clipProg_ == 0 ||
        blendProgs_[0] == 0) {
        return;
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
    for (int m = 0; m < kBlendModeCount; ++m) {
        if (blendProgs_[m] != 0) glDeleteProgram(blendProgs_[m]);
        blendProgs_[m] = 0;
    }
    checkerProg_ = tileProg_ = plainProg_ = clipProg_ = 0;
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    vbo_ = vao_ = 0;
    inited_ = false;
}

void TileRenderer::drawQuad(GLuint program, float rx, float ry, float rw, float rh, float zoom,
                            float panX, float panY, float vw, float vh) {
    glUseProgram(program);
    const GLint rectLoc = glGetUniformLocation(program, "uRect");
    const GLint viewLoc = glGetUniformLocation(program, "uView");
    const GLint vpLoc = glGetUniformLocation(program, "uViewport");
    glUniform4f(rectLoc, rx, ry, rw, rh);
    glUniform4f(viewLoc, zoom, panX, panY, 0.0f);
    glUniform2f(vpLoc, vw, vh);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void TileRenderer::drawTiles(const Layer& layer, float zoom, float panX, float panY, float vw,
                             float vh, const StrokeOverlay* stroke) {
    const std::shared_ptr<const TileGrid> effective = layer.effectivePixels();
    if (effective == nullptr) {
        return;
    }
    const TileGrid& grid = *effective;
    if (grid.cols == 0 || grid.rows == 0) {
        return;
    }
    const float ox = static_cast<float>(layer.transform.originX);
    const float oy = static_cast<float>(layer.transform.originY);

    // 视口求交（03 §6 瓦片剔除）：doc 可见范围 ∩ 图层变换范围
    const float docX0 = panX;
    const float docY0 = panY;
    const float docX1 = panX + vw / zoom;
    const float docY1 = panY + vh / zoom;
    const uint32_t tx0 = static_cast<uint32_t>(std::max(0.0f, std::floor((docX0 - ox) / kTileSize)));
    const uint32_t ty0 = static_cast<uint32_t>(std::max(0.0f, std::floor((docY0 - oy) / kTileSize)));
    const uint32_t tx1 = std::min(grid.cols - 1u, static_cast<uint32_t>(
        std::max(0.0f, std::floor((docX1 - ox) / kTileSize))));
    const uint32_t ty1 = std::min(grid.rows - 1u, static_cast<uint32_t>(
        std::max(0.0f, std::floor((docY1 - oy) / kTileSize))));
    const bool wantNearest = zoom >= 2.0f;  // 03 §6：≥200% 切 NEAREST（crispZoom）

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
                             panX, panY, static_cast<float>(vw), static_cast<float>(vh));
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
                     static_cast<float>(vw), static_cast<float>(vh));
        }
    }
}

void TileRenderer::drawFrame(const Document& doc, const Viewport& vp, int32_t vw, int32_t vh,
                             const StrokeOverlay* stroke) {
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

    const float zoom = static_cast<float>(clampZoom(vp.zoom));
    const float panX = static_cast<float>(vp.panX);
    const float panY = static_cast<float>(vp.panY);
    const float vwF = static_cast<float>(vw);
    const float vhF = static_cast<float>(vh);

    // ① 自底向上逐层两趟合成（03 §4）：assemble → layerFBO，blend → acc ping-pong
    for (int i = 0; i < 2; ++i) {
        glBindFramebuffer(GL_FRAMEBUFFER, accFbo_[i]);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    int accIdx = 0;
    int composited = 0;
    bool baseTexValid = false;  // M5b-2：baseTex_ 是否持有当前剪贴组的基 alpha
    for (size_t li = 0; li < doc.layers.size(); ++li) {
        const Layer& layer = doc.layers[li];
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
                  stroke != nullptr && stroke->layerId == layer.id ? stroke : nullptr);

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
    glDisable(GL_BLEND);

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
