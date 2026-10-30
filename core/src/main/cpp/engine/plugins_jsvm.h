#ifndef MONTAGE_ENGINE_PLUGINS_JSVM_H
#define MONTAGE_ENGINE_PLUGINS_JSVM_H

// M26 插件 JSVM 宿主接口（设备侧实现见 plugins_jsvm.cpp；宿主测试不编译该文件）。

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "engine/document.h"
#include "engine/plugins.h"

namespace montage {
namespace plugins {

// 注册期校验运行：建 VM → 载入源码 → montage_manifest() 读出 → validateManifest。
// 在调用线程执行（bridge 于 async work 线程调用）；时限 kRegisterTimeMs。
// JSVM 主线程执行器注入（bridge napi 注册期调用；threadsafe function 回调 = ArkTS JS 线程）。
using PluginHostExecutor = std::function<void(const std::function<void(std::string&)>&,
                                              std::string&, bool&)>;
void pluginSetHostExecutor(PluginHostExecutor executor);

bool pluginValidateSource(const std::string& source, Manifest& out, std::string& err);

// 单 band 元信息（api.image 载荷；x/y = band 输入域原点）。
struct BandMeta {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t overlap = 0;
    int32_t canvasW = 0;
    int32_t canvasH = 0;
    int32_t bandIndex = 0;
    int32_t bandCount = 1;
};

// 一次运行的 JSVM 会话（start → 逐 band applyBand → stop；线程受限——全程同线程）。
class PluginRunner {
  public:
    PluginRunner();
    ~PluginRunner();
    PluginRunner(const PluginRunner&) = delete;
    PluginRunner& operator=(const PluginRunner&) = delete;

    bool start(const std::string& source, int64_t timeMs, std::string& err);
    // pixels = iw×ih×4 f32（原地写回）。超时/异常 → false + err。
    bool applyBand(float* pixels, int iw, int ih, const BandMeta& meta,
                   const std::vector<std::pair<std::string, PValue>>& params, std::string& err);
    void stop();
    bool aborted() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// 弃线程路径登记员：deadline+预算 后若运行未完成（无完成通知机制——本函数不判完成，
// 由 bridge 用 done 旗标决定是否调用 poison）——v1 简化：监视线程到期直接 poison，
// 正常完成的运行随 Registry poison 幂等无害（完成后再 poison 语义=「下轮禁用」，可接受）。
void startPoisonMonitor(const std::string& pluginId, int64_t startedAt, int64_t timeMs);

// 滤镜执行全管线（方案 §4.1）：work = 文档快照副本（bridge 发布段复核后入账）。
// 选区 lerp / overlap band / 清洗 / DeepEditGuard 深色垫片 / publish 全在内。
bool pluginRunFilter(Document& work, LayerId layerId, const PluginEntry& entry,
                     const std::vector<std::pair<std::string, PValue>>& params,
                     std::string& err);

}  // namespace plugins
}  // namespace montage

#endif  // MONTAGE_ENGINE_PLUGINS_JSVM_H
