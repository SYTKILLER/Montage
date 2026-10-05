import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { CompositorEngine, EngineStats, PixelTestData, PixelProbe } from 'core';

const TAG: string = '[Montage.M0]';

function probeLine(p: PixelProbe): string {
  return `${p.allocator}: create=${p.createOk ? 'ok' : `fail(${p.createRc})`}` +
    ` size=${p.width}x${p.height} stride=${p.rowStride}` +
    ` 访问=${p.accessOk ? 'ok' : `fail(${p.accessRc})`}` +
    ` 读写=${p.writeReadOk ? '通过' : '不通过'}` +
    (p.error === '' ? '' : ` err=${p.error}`);
}

/**
 * M0 技术尖刺验证页（临时脚手架，M1 起被编辑器骨架替代）：
 * 1) HAR native 打包 + NAPI 模块注册（import 'libcompositor.so'）
 * 2) XComponent SURFACE → surfaceId → EGL 清屏 + NativeVsync 60fps
 * 3) 命令往返 + 版本号事件（AppStorage docVersion）
 * 4) 像素指针读写 + stride 实测
 */
@Entry
@Component
struct Index {
  @StorageProp('docVersion') docVersion: number = 0;
  @State statsText: string = '等待渲染…';
  @State pixelText: string = '未运行';
  @State surfaceIdText: string = '尚未取得';
  @State logLines: string[] = [];
  private engine: CompositorEngine = new CompositorEngine();
  private xcController: XComponentController = new XComponentController();
  private statsTimer: number = -1;

  aboutToAppear(): void {
    this.engine.bindRevisionListener((revision: number): void => {
      this.log(`版本号事件 → ${revision}（已写入 AppStorage docVersion）`);
    });
    this.statsTimer = setInterval((): void => {
      this.refreshStats();
    }, 1000);
  }

  aboutToDisappear(): void {
    if (this.statsTimer >= 0) {
      clearInterval(this.statsTimer);
      this.statsTimer = -1;
    }
    this.engine.dispose();
  }

  private log(msg: string): void {
    this.logLines.splice(0, 0, msg);
    if (this.logLines.length > 30) {
      this.logLines.splice(30);
    }
    hilog.info(0x4D30, TAG, '%{public}s', msg);
  }

  private onCanvasReady(): void {
    try {
      const id: string = this.xcController.getXComponentSurfaceId();
      this.surfaceIdText = id;
      this.log(`取得 surfaceId=${id}`);
      this.doCreateSurface();
      this.doNewDocument();
      this.doPixelTest();
    } catch (e) {
      const err: BusinessError = e as BusinessError;
      this.log(`画布初始化失败: ${err.code} ${err.message}`);
    }
  }

  private doCreateSurface(): void {
    const ok: boolean = this.engine.createSurface(this.surfaceIdText);
    this.log(`创建渲染表面: ${ok ? '成功（EGL 初始化完成）' : '失败，见 hilog'}`);
    this.refreshStats();
  }

  private doNewDocument(): void {
    const ok: boolean = this.engine.newDocument(4096, 4096);
    this.log(`新建文档 4096x4096: ${ok ? '成功（命令往返 + 版本号递增）' : '失败'}`);
  }

  private doPixelTest(): void {
    this.pixelText = '运行中…';
    this.engine.runPixelTest().then((data: PixelTestData): void => {
      const converted: boolean = data.samplePixelMap !== null;
      this.pixelText = `DMA 读写=${data.dma.writeReadOk ? '通过' : '不通过'}  ` +
        `共享内存读写=${data.shared.writeReadOk ? '通过' : '不通过'}  ` +
        `PixelMap 跨桥=${converted ? '成功' : '未返回'}`;
      this.log(`像素测试完成 → ${probeLine(data.dma)}`);
      this.log(`像素测试完成 → ${probeLine(data.shared)}`);
    }).catch((e: BusinessError): void => {
      this.pixelText = `失败: ${e.message}`;
      hilog.error(0x4D30, TAG, 'runPixelTest error %{public}s', e.message);
    });
  }

  private refreshStats(): void {
    const s: EngineStats = this.engine.getStats();
    this.statsText = `fps=${s.fps.toFixed(1)} frames=${s.frames} buf=${s.bufferWidth}x${s.bufferHeight} ` +
      `docRev=${s.docRevision} open=${s.docOpen} running=${s.renderRunning}` +
      (s.lastError === '' ? '' : ` err=${s.lastError}`);
  }

  build() {
    Column({ space: 10 }) {
      Text('Montage · M0 技术尖刺')
        .fontSize(20)
        .fontWeight(FontWeight.Bold)

      Row({ space: 16 }) {
        Text(`文档版本号: ${this.docVersion}`)
          .fontSize(14)
          .fontColor($r('app.color.text_primary'))
        Text(`surfaceId: ${this.surfaceIdText}`)
          .fontSize(12)
          .fontColor($r('app.color.text_secondary'))
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }
      .width('100%')

      XComponent({ id: 'montage-canvas', type: XComponentType.SURFACE, controller: this.xcController })
        .width('100%')
        .layoutWeight(1)
        .onLoad((): void => {
          this.onCanvasReady();
        })

      Flex({ wrap: FlexWrap.Wrap, justifyContent: FlexAlign.Start }) {
        Button('新建文档').onClick((): void => this.doNewDocument())
        Button('创建表面').onClick((): void => this.doCreateSurface()).margin({ left: 8 })
        Button('销毁表面').onClick((): void => {
          const ok: boolean = this.engine.destroySurface();
          this.log(`销毁渲染表面: ${ok ? '成功' : '失败'}`);
          this.refreshStats();
        }).margin({ left: 8 })
        Button('像素测试').onClick((): void => this.doPixelTest()).margin({ left: 8 })
        Button('刷新统计').onClick((): void => this.refreshStats()).margin({ left: 8 })
      }
      .width('100%')

      Text(`统计: ${this.statsText}`)
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
        .width('100%')

      Text(`像素测试: ${this.pixelText}`)
        .fontSize(12)
        .fontColor($r('app.color.text_secondary'))
        .width('100%')

      List({ space: 2 }) {
        ForEach(this.logLines, (line: string) => {
          ListItem() {
            Text(line)
              .fontSize(11)
              .fontColor($r('app.color.text_primary'))
              .width('100%')
          }
        }, (line: string, index: number): string => `${index}|${line}`)
      }
      .width('100%')
      .height(140)
      .scrollBar(BarState.Off)
    }
    .width('100%')
    .height('100%')
    .padding(16)
    .backgroundColor($r('app.color.page_background'))
  }
}
