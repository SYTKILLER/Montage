# Q&A 补充：Q7 RAW 显影（第二次提问）

> 提问时间：2026-10-05（Q&A 1-13 之后的针对 Q7 的追问）。来源：官方开发助手，用户以对话形式获得后转入归档。
> 本文件为原始回答记录，结论已合入 [Q&A 1-13 决策整理.md](./Q&A%201-13%20决策整理.md) 的 Q7 小节。

---

## 原始回答（要点保全）

在鸿蒙（HarmonyOS）开发中，确实没有与 iOS 的 `CIRAWFilter` 直接对等的系统级 RAW 显影滤镜。当前 Image Kit 的能力主要集中在解码 RAW 文件中的**内嵌 JPEG 预览图**（API 22+ 支持 CR2、CR3、ARW、NEF 等格式的内嵌预览图），但这并不代表无法进行真正的 RAW 数据处理。

核心解决方案：**`createImageRawData()`** 接口获取 RAW 原始数据，自行实现显影算法。

### 1. 获取 RAW 原始数据（DNG 格式）

从 **API version 24** 开始，HarmonyOS 支持将符合格式的图片文件解码为 `ImageRawData`，获取图像传感器直接输出的原始数据（包含图像缓冲区和像素位数 bitDepth）。**当前支持的 RAW 文件格式为 DNG**。

```typescript
import { image } from '@kit.ImageKit';

let imageSource = image.createImageSource(filePath);   // 沙箱路径等
let imageRawData = await imageSource.createImageRawData();
// imageRawData 含 RAW 缓冲区（buffer）与像素位数（bitDepth）
// 可基于此实现自定义显影：去马赛克、白平衡、色彩校正等
await imageRawData.release();
await imageSource.release();
```

### 2. 系统级 RAW 显示支持（HarmonyOS 6）

开发者社区反馈：升级到 HarmonyOS 6 后，`Image` 组件已支持直接显示部分 RAW 格式（如索尼 ARW）。系统底层编解码能力增强，但**完整显影处理（降噪、锐化、镜头校正等）仍需开发者自行实现**。

### 3. 开发建议（原文要点）

- **格式支持差异**：`createImageRawData()` 目前仅明确支持 **DNG**；其他 RAW（ARW/CR2 等）暂时只能取内嵌 JPEG 预览图显示，无法获取传感器原始数据。
- **算法实现**：需自行实现或集成第三方库完成显影流程：黑电平扣除、去马赛克（demosaic）、白平衡、色彩矩阵转换、Gamma 校正等。
- **兼容性检查**：解码前调用 `image.getImageSourceSupportedFormats()` 动态查询设备实际支持列表。

### 原文代码片段（补充保留）

```typescript
function getFilePath(context: Context, fileName: string): string {
  const filePath: string = context.cacheDir + '/' + fileName;
  return filePath;
}
```
