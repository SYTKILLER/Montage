#ifndef MONTAGE_BRIDGE_CODEC_BRIDGE_H
#define MONTAGE_BRIDGE_CODEC_BRIDGE_H

// M25 编解码批命令面（实现见 codec_bridge.cpp；注册在 napi_module.cpp）。

#include <napi/native_api.h>

namespace montage {
namespace bridge {

// 平面格式导出：flatten → encode → 写 fd（异步 Promise {width,height,bytes}）
napi_value ExportImage(napi_env env, napi_callback_info info);
// 能力表：{ok, data:{formats:[{format,read,write,depths,exts,note}], openExts, saveExts}}
napi_value GetCodecCapabilities(napi_env env, napi_callback_info info);

}  // namespace bridge
}  // namespace montage

#endif  // MONTAGE_BRIDGE_CODEC_BRIDGE_H
