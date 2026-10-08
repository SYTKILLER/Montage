#include <napi/native_api.h>

#include "bridge/engine_bridge.h"

namespace {

napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor props[] = {
        {"newDocument", nullptr, montage::bridge::NewDocument, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"closeDocument", nullptr, montage::bridge::CloseDocument, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"openFileFromFd", nullptr, montage::bridge::OpenFileFromFd, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"openPsdFile", nullptr, montage::bridge::OpenPsdFile, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"saveProject", nullptr, montage::bridge::SaveProject, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"openProject", nullptr, montage::bridge::OpenProject, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setBrushSettings", nullptr, montage::bridge::SetBrushSettings, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setBrushTarget", nullptr, montage::bridge::SetBrushTarget, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addLayerMask", nullptr, montage::bridge::AddLayerMask, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"beginStroke", nullptr, montage::bridge::BeginStroke, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"continueStroke", nullptr, montage::bridge::ContinueStroke, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"endStroke", nullptr, montage::bridge::EndStroke, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"undo", nullptr, montage::bridge::Undo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"redo", nullptr, montage::bridge::Redo, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setViewport", nullptr, montage::bridge::SetViewport, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"fitToWindow", nullptr, montage::bridge::FitToWindow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addLayer", nullptr, montage::bridge::AddLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"addAdjustmentLayer", nullptr, montage::bridge::AddAdjustmentLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setAdjustmentParams", nullptr, montage::bridge::SetAdjustmentParams, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setAdjustmentKind", nullptr, montage::bridge::SetAdjustmentKind, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getHistogram", nullptr, montage::bridge::GetHistogram, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRectSelection", nullptr, montage::bridge::SetRectSelection, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"clearSelection", nullptr, montage::bridge::ClearSelection, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setWandSelection", nullptr, montage::bridge::SetWandSelection, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"removeLayer", nullptr, montage::bridge::RemoveLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"selectLayer", nullptr, montage::bridge::SelectLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLayerVisible", nullptr, montage::bridge::SetLayerVisible, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLayerMaskEnabled", nullptr, montage::bridge::SetLayerMaskEnabled, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLayerClipping", nullptr, montage::bridge::SetLayerClipping, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLayerOpacity", nullptr, montage::bridge::SetLayerOpacity, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setLayerBlendMode", nullptr, montage::bridge::SetLayerBlendMode, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"reorderLayer", nullptr, montage::bridge::ReorderLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLayerListSnapshot", nullptr, montage::bridge::GetLayerListSnapshot, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getLayerThumbnail", nullptr, montage::bridge::GetLayerThumbnail, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createSurface", nullptr, montage::bridge::CreateSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroySurface", nullptr, montage::bridge::DestroySurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"getStats", nullptr, montage::bridge::GetStats, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"onRevisionChanged", nullptr, montage::bridge::OnRevisionChanged, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"onImportProgress", nullptr, montage::bridge::OnImportProgress, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"runPixelTest", nullptr, montage::bridge::RunPixelTest, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"renameLayer", nullptr, montage::bridge::RenameLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"samplePixel", nullptr, montage::bridge::SamplePixel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exportPng", nullptr, montage::bridge::ExportPng, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"exportPsd", nullptr, montage::bridge::ExportPsd, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"flattenToPixelMap", nullptr, montage::bridge::FlattenToPixelMap, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resizeCanvas", nullptr, montage::bridge::ResizeCanvas, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"trimCanvas", nullptr, montage::bridge::TrimCanvas, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"revealAll", nullptr, montage::bridge::RevealAll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"rotateDocument90", nullptr, montage::bridge::RotateDocument90, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"flipDocument", nullptr, montage::bridge::FlipDocument, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"translateLayer", nullptr, montage::bridge::TranslateLayer, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"translateSelection", nullptr, montage::bridge::TranslateSelection, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"resampleDocument", nullptr, montage::bridge::ResampleDocument, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"rotateDocumentArbitrary", nullptr, montage::bridge::RotateDocumentArbitrary, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"bakeLayerTransform", nullptr, montage::bridge::BakeLayerTransform, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"dispose", nullptr, montage::bridge::Dispose, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
    return exports;
}

static napi_module g_montageCompositorModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "compositor",
    .nm_priv = nullptr,
    .reserved = {0},
};

}  // namespace

// .so 被加载时静态注册 NAPI 模块（HAR native 打包验证项：注册与导入方式实测）。
extern "C" __attribute__((constructor)) void MontageCompositorModuleRegister(void) {
    napi_module_register(&g_montageCompositorModule);
}
