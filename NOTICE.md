# NOTICE — Montage（蒙太奇）

> 本文件是项目状态与约定的权威快照（跨会话恢复用）。详细文档见 `docs/`。

## 项目定位

**Montage（蒙太奇）**——为鸿蒙平台打造的 Photoshop 级开源替代软件。上游 Compositor（robbietilton/Compositor，macOS 开源图像编辑器）仅作为参考实现与脚手架；文件互换以 **PSD 为中心**。

- GitHub：`github.com/SYTKILLER/Montage`（Public，2026-10-05 建仓并推送）
- bundleName：`com.sytkiller.montage`；应用名：Montage
- 自有工程格式：`.montage` 包（manifest.json + 每层 PNG），格式标识 `com.sytkiller.montage.project`
- 基线：API 24（6.1.1(24)），目标 2in1/鸿蒙 PC（自由窗口），开发环境 = 模拟器 `Huawei_2in1_Foldable`（暂无 PC 真机）

## 新会话开工流程（规定动作）

1. 读本文件（状态快照）→ 2. 读 `docs/README.md` → 3. 写代码前读工作区 `D:\HarmonyOS_Develop\docs\开发规范.md` + `踩坑记录汇总.md` → 4. 方案细节进 `docs/40-方案设计/`（01–05 已定案）→ 5. API 结论一律过本地 SDK 核验（官方助手曾答错两处，见 `docs/30-移植问答/Q&A 1-13 决策整理.md` §2）→ 6. 改完跑 hvigor 门禁、提交推送。

## 必读文档（编码前）

1. `D:\HarmonyOS_Develop\docs\开发规范.md` + `D:\HarmonyOS_Develop\docs\踩坑记录汇总.md`（工作区强制基线）
2. `docs/20-架构分析/源项目架构梳理.md`（上游架构）
3. `docs/30-移植问答/Q&A 1-13 决策整理.md`（技术路线 + SDK 核验清单）
4. `docs/40-方案设计/00-方案设计路线图.md` → 01–05（逐层方案，01–04 已定案、05 待确认）

## 关键决策索引（截至 2026-10-05）

- **架构**：ArkTS UI + C++ 引擎（compositor HAR）；文档模型住 C++；四线程模型；typed NAPI 命令面 + 版本号事件面；单 HAP + 多 HAR（**HAR native 打包已 M0 实证**）
- **像素**：全图层 256px 稀疏瓦片（TileGrid，不可变 + shared_ptr 快照共享撤销）；PixelSource 抽象；PixelMap 仅 IO 边界；导入 = 分带区域解码 + RAII Guard
- **选区**：8-bit 软选区掩码主表示；蚂蚁线 = Marching Squares（命中绝不走轮廓几何）
- **渲染**：Native XComponent + GLES 3.0；视口空间每层两趟（assemble+blend，ping-pong acc，acc 优先 RGBA16F）；23 混合模式每模式独立 program；覆盖层分工 = GL 像素真相 + ArkTS Canvas 几何交互；CPU 导出合成器必需，实时 CPU 兜底不做；色彩固定 sRGB 非线性
- **文件**：PSD 导入 P0（M2.5 插入，上游解析器 C++ 翻译，PSD-1 = 栅格/组/蒙版/混合/剪贴）；自有 .montage 格式主存；PSD 导出 P1/v2；**RAW v1 仅 DNG**（createImageRawData + 自研显影链）
- **里程碑**：M0 技术尖刺 → M1 打开浏览 → M2 图层合成 → M2.5 PSD-1 → M3 笔刷 → M4 撤销+保存 → M5 蒙版 → M6 调整层 → M7 选区 → M8 变换/裁剪

## 遗留决策 / 待办

- ~~05 层四项~~ → **已定案（2026-10-05，D5.1–D5.4）**：主题三态自选（深色/浅色/跟随系统，否决固定深色）；图标缺口自绘豁免；v1 单文档；滚轮直接缩放
- **图标红线例外登记（2026-10-05 用户批准）**：套索/渐变/通用形状/光标徽标等专业工具图标在 sys.symbol 无对应，允许自绘 SVG（media 资源）；其余图标一律 SymbolGlyph
- **M0 技术尖刺已完成（2026-10-05）**：HAR native 打包 ✅ / XComponent+EGL 清屏 ✅ / NativeVsync 60fps（实测 59.1）✅ / NAPI 双向通路（命令往返 + 版本回调）✅ / 像素指针读写 ✅（DMA+共享内存双通过、PixelMap 跨桥成功）。实证修订见 01 文档 §10 D6
- 下一步：06 功能映射矩阵 或直接开工 M1（打开浏览）
- **待用户决策：调试签名材料**——`~/.ohos/config` 现有 profile 均绑定他项目 bundleName，Montage 真机调试/发版前需在 DevEco 里自动生成 com.sytkiller.montage 专属签名（用户登录 AGC 操作；模拟器当前免签名不受影响）

## 项目坑（按日期追加）

- **2026-10-05（M0）模拟器是 x86_64 镜像**：abiFilters 必须含 x86_64（已配 arm64-v8a + x86_64 双 ABI）；只配 arm64 装包报 9568347 ABI 不匹配。
- **2026-10-05（M0）OH_NativeVSync 回调不在创建线程**（模拟器实证，两线程 id 不同）：GL 上下文在首帧回调线程惰性 makeCurrent（`render_loop.cpp` ensureCurrent），初始化线程不保 current，GL 销毁也在回调线程做。
- **2026-10-05（M0）模拟器免签名**：调试镜像 `hdc install` 接受未签名 HAP；真机强制 AGC 签名（见上"待用户决策"）。

## 约定

- 本项目所有文档放 `docs/` 下编号目录（10-源码仓库 / 20-架构分析 / 30-移植问答 / 40-方案设计）
- `docs/10-源码仓库/` 不入库（.gitignore 排除）；**2026-10-05 已删除归档内 `.git`**（上游 v1.4.5 纯本地快照，无远程关联），一律依据本地代码开发
- 真机操作必须先经用户授意（当前无真机）；图标一律 SymbolGlyph（缺口自绘需用户豁免）
- 上游 Compositor 的名字/格式（.comp）与本项目无关；本项目叫 Montage，别在代码/文档里混用
