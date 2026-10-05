# DEVELOPMENT_LOG — Montage

> 按《开发规范》§1.5 收尾要求记录（`## 日期 · 变更原因`）。权威状态快照见 NOTICE.md。

## 2026-10-05 · M0 技术尖刺

- 新增 `core` HAR：C++ 引擎骨架（`bridge/` NAPI 模块与命令面、`render/` EGL+NativeVsync 渲染循环、`tiles/` 像素探针；CMake，abiFilters = arm64-v8a + x86_64）+ ArkTS typed 包装（`CompositorEngine` + `EngineTypes`）；NAPI 静态注册（modname=compositor）。
- `entry` 依赖 core（`file:../core`）；`pages/Index.ets` 改为 M0 尖刺验证页（临时脚手架，M1 起被编辑器骨架替代）；补双主题 color 资源。
- 实证通过：HAR native 打包全链路（编译/打包/安装/运行）；XComponent SURFACE → surfaceId → EGL 清屏 **59.1fps**（模拟器 60Hz）；NAPI 命令往返 + 版本回调（threadsafe function → AppStorage `docVersion` 驱动 UI）；像素实测 **DMA（1024²，stride=4096）与共享内存（256²，stride=1024）AccessPixels 指针写读回双向通过**、`ConvertPixelmapNativeToNapi` 跨桥成功。
- 方案修订：01 文档 §7 vsync 回调线程语义（**回调不在创建线程执行**，GL 上下文首帧回调线程惰性绑定）；决策日志增 D6。
- 新坑沉淀（工作区踩坑汇总，【Montage】）：2in1 模拟器 x86_64 镜像 ABI；HAR 需 src/main/module.json5；模拟器调试镜像免签名；OH_NativeVSync 回调线程语义。
- 门禁：`--no-daemon` 全量 BUILD SUCCESSFUL 0 error；模拟器安装运行 + 截图验收通过。
