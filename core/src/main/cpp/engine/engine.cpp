#include "engine/engine.h"

#include <hilog/log.h>

namespace montage {
namespace {
constexpr unsigned int kDomain = 0x4D30;
constexpr const char* kTag = "Montage.Engine";
}  // namespace

Engine& Engine::get() {
    static Engine inst;
    return inst;
}

void Engine::requestRender() {
    needsRender.store(true);
    render.wake();
}

void Engine::bumpRevisionLocked() {
    // 须持有 docMutex；版本经 TSFN 送 JS 线程（AppStorage docVersion，01 §6 事件面）
    docRevision++;
    if (revisionTsfn == nullptr) {
        return;
    }
    void* payload = reinterpret_cast<void*>(static_cast<intptr_t>(docRevision));
    if (napi_call_threadsafe_function(revisionTsfn, payload, napi_tsfn_nonblocking) != napi_ok) {
        OH_LOG_Print(LOG_APP, LOG_WARN, kDomain, kTag, "revision tsfn call failed");
    }
}

void Engine::resetDocument() {
    {
        std::lock_guard<std::mutex> lk(import.mtx);
        import.cancel = true;
    }
    std::lock_guard<std::mutex> lk(docMutex);
    doc = Document{};
    viewport = Viewport{};
    needsRender.store(true);
    render.wake();
}

}  // namespace montage
