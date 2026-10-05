# Montage（蒙太奇）

为鸿蒙平台打造的 Photoshop 级开源图像编辑器：ArkTS UI + C++ 引擎（NAPI 通路 + OpenGL ES 渲染），面向 2in1 / 鸿蒙 PC 自由窗口场景，文件互换以 PSD 为中心。

> 当前处于早期开发阶段。里程碑 M0（技术验证）已完成：native HAR 打包、EGL 渲染循环、NAPI 双向通路、像素读写均已在 2in1 模拟器实证通过。

## 技术栈

- **UI**：ArkTS（API 24，HarmonyOS 6.1.1(24)），目标设备 2in1 / 鸿蒙 PC（自由窗口）
- **引擎**：C++（`core` HAR）——typed NAPI 命令面 + 版本号事件面、EGL + OpenGL ES 3.0 渲染、256px 稀疏瓦片像素模型
- **工程结构**：单 HAP（`entry`）+ native HAR（`core`）

## 构建

1. 使用 DevEco Studio（含 HarmonyOS 6.1.1(24) SDK）打开工程根目录。
2. 首次构建前在 **File → Project Structure → Signing Configs** 生成自己的调试签名（仓库不含任何签名材料，`signingConfigs` 为空）。
3. 命令行构建：`hvigorw assembleHap --mode module -p module=entry@default -p product=default`。

## License

[MIT](LICENSE)
