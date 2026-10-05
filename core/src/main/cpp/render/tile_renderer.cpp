#include "render/tile_renderer.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <hilog/log.h>

#include "render/shaders.h"

namespace montage {
namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Render";
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

void TileRenderer::ensureInit() {
    if (inited_) {
        return;
    }
    checkerProg_ = buildProgram(shaders::kQuadVert, shaders::kCheckerFrag);
    tileProg_ = buildProgram(shaders::kQuadVert, shaders::kTileFrag);
    if (checkerProg_ == 0 || tileProg_ == 0) {
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
    cache_.clear();
    if (checkerProg_ != 0) glDeleteProgram(checkerProg_);
    if (tileProg_ != 0) glDeleteProgram(tileProg_);
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
    checkerProg_ = tileProg_ = 0;
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

void TileRenderer::drawFrame(const Document& doc, const Viewport& vp, int32_t vw, int32_t vh) {
    frame_++;
    glViewport(0, 0, vw, vh);
    // 画布外背板：中性灰（05 §7：与主题解耦，对齐 PS 行为）
    glClearColor(0.27f, 0.27f, 0.27f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!inited_ || doc.pixels == nullptr || doc.width == 0 || doc.height == 0) {
        return;
    }

    const float zoom = static_cast<float>(clampZoom(vp.zoom));
    const float panX = static_cast<float>(vp.panX);
    const float panY = static_cast<float>(vp.panY);

    // ① 棋盘格透明底（仅文档矩形内）
    glDisable(GL_BLEND);
    drawQuad(checkerProg_, 0.0f, 0.0f, static_cast<float>(doc.width), static_cast<float>(doc.height),
             zoom, panX, panY, static_cast<float>(vw), static_cast<float>(vh));

    // ② 可见瓦片：视口求交（03 §6 瓦片剔除），按需上传
    const TileGrid& grid = *doc.pixels;
    if (grid.cols == 0 || grid.rows == 0) {
        return;
    }
    const float docX0 = panX;
    const float docY0 = panY;
    const float docX1 = panX + static_cast<float>(vw) / zoom;
    const float docY1 = panY + static_cast<float>(vh) / zoom;
    const uint32_t tx0 = static_cast<uint32_t>(std::max(0.0f, std::floor(docX0 / kTileSize)));
    const uint32_t ty0 = static_cast<uint32_t>(std::max(0.0f, std::floor(docY0 / kTileSize)));
    const uint32_t tx1 = std::min(grid.cols - 1u,
                                  static_cast<uint32_t>(std::max(0.0f, std::floor(docX1 / kTileSize))));
    const uint32_t ty1 = std::min(grid.rows - 1u,
                                  static_cast<uint32_t>(std::max(0.0f, std::floor(docY1 / kTileSize))));
    const bool wantNearest = zoom >= 2.0f;  // 03 §6：≥200% 切 NEAREST

    glUseProgram(tileProg_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    const GLint texLoc = glGetUniformLocation(tileProg_, "uTex");
    glUniform1i(texLoc, 0);
    glActiveTexture(GL_TEXTURE0);

    for (uint32_t ty = ty0; ty <= ty1; ++ty) {
        for (uint32_t tx = tx0; tx <= tx1; ++tx) {
            const uint32_t idx = ty * grid.cols + tx;
            auto it = grid.tiles.find(idx);
            if (it == grid.tiles.end() || it->second == nullptr) {
                continue;  // 稀疏：空白瓦片不存在
            }
            const Tile* tile = it->second.get();
            CacheEntry& entry = cache_[idx];
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
            drawQuad(tileProg_, static_cast<float>(tx * kTileSize), static_cast<float>(ty * kTileSize),
                     static_cast<float>(kTileSize), static_cast<float>(kTileSize), zoom, panX, panY,
                     static_cast<float>(vw), static_cast<float>(vh));
        }
    }
    glDisable(GL_BLEND);

    // LRU 逐出（超预算时最久未用先走）
    if (cache_.size() > kCacheCap) {
        std::vector<uint32_t> victims;
        for (auto& kv : cache_) {
            if (frame_ - kv.second.lastFrame > 8) {
                victims.push_back(kv.first);
            }
        }
        std::sort(victims.begin(), victims.end(), [this](uint32_t a, uint32_t b) {
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
