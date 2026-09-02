# Layer 生命周期设计（ONS 屏幕的承载与呈现）

> 本文只保留**仍然有效且必须遵守**的部分：krkrsdl3 Layer 的生命周期与呈现规则。
> （曾经写过的 “SDL2 兼容层” 与 “引擎表面直接移植” 方案均已否决并删除，
> 当前执行模型为“指令转发”，见 docs/06。）

## 1. 为什么由插件管理 Layer 生命周期

krkrsdl3 的 Layer（tTJSNI_Layer / tTJSNI_BaseLayer）必须挂在某个 `TVPWindow`（layerTreeOwner）
上才会被合成上屏；Layer 的图像（MainImage）尺寸/存在性影响像素缓冲地址。把 ONS 屏幕的
Window/Layer 创建、尺寸、图像分配、失效、销毁全部放在插件（ONSScreen，C++ 侧）内，
可保证：

- 创建顺序正确（先窗口后主层、先尺寸后 hasImage）；
- 像素缓冲指针在 Layer 存活期内稳定（运行期不改 Layer 尺寸）；
- 引擎停止与 Layer 销毁的顺序安全（先 join 引擎线程，再销毁）；
- 不向 TJS 脚本暴露多余的对象（“不要引入多余的东西”）。

## 2. 生命周期规则

1. **创建（主线程，ONSScreen::start）**
   - 决定屏幕分辨率（脚本 `setwindow` 分辨率；默认 640x480）；
   - 通过全局类 `Window` / `Layer` 的 CreateNew 原生创建窗口对象与主 Layer
     （`new Layer(wnd, null)` 语义 → 成为该窗口 primary layer）；
   - 顺序：`SetSize(w,h)` → `SetHasImage(true)`（分配 MainImage）→ `SetVisible(true)`；
   - 窗口 `SetVisible(true)`（内部会 BringToFront 激活）；
   - 缓存 `GetMainImagePixelBufferForWrite()` / `GetMainImagePixelBufferPitch()`。
2. **呈现（引擎执行线程写，主线程合成）**
   - 每帧把画面拷贝进 Layer 像素缓冲（若目标分辨率不同则最近邻缩放）+ 置原子脏标志；
   - 主线程 tick（TVPTimer ~16ms）：脏 → `LayerManager::RequestInvalidation(全矩形)`
     → AddUpdateRegion → NotifyWindowInvalidation → UpdateToDrawDevice → Show（上屏）。
   - 指令转发模式（docs/06）下，多数绘制走 Layer 原生方法（loadImages/drawGlyph/drawText/
     fillRect），由 krkrsdl3 内部标脏；Present 仅用于“整帧位图”场景（如快照）。
3. **尺寸不变性**：运行期 Layer 尺寸固定；脚本运行时分辨率变化由呈现侧缩放处理，
   避免主线程外重分配像素缓冲。
4. **销毁（主线程，ONSScreen::stop）**
   - 通知 VM 退出 → join 执行线程 → 停 tick；
   - Layer TJS 对象 `Invalidate()`（自动 DetachPrimary / UnregisterSelfFromWindow / Part /
     释放 MainImage，见 tjsNativeLayer.cpp:501）→ 释放引用；
   - Window TJS 对象 `Invalidate()`（销毁 TVPWindow）→ 释放引用；
   - 应用退出路径（SDL_AppQuit → TJS 引擎反初始化 → 插件析构）同样安全（防御空指针/异常）。
5. **线程约定**：TJS/Layer 的一切操作只在主线程；执行线程只做脚本逻辑与事件入队；
   事件队列（ons_events）互斥+条件变量，线程安全。

## 3. 呈现细节

- 字节序：引擎画面 ARGB8888（A 高位）→ krkrsdl3 Layer 缓冲 RGBA8888（R 在前，与 PNG handler 一致）。
- Layer 尺寸 ≠ 脚本分辨率时最近邻缩放（运行期 setwindow 场景）。
- 首帧：Present 后 tick 自然触发重绘；空画面（纯黑）由 compositor 填充。

## 4. 输入

输入不再经 Patch（`Patch/sdl3_app.cpp` 方案已废弃并移除）：krkrsdl3 主循环把 SDL3 事件
转成 TJS Layer 事件（onMouseDown/onMouseUp/onMouseMove/onKeyDown/onKeyUp），
`demo/startup.tjs` 的 ONSLayer 重写这些事件方法转发给 `ONSEngine.onMouseDown(...)` 等
（ncbind 方法），经 `ons_events::FeedMouseButton/FeedMouseMotion/FeedKey` 入队，
VM 在等待点消费（全部主线程，无需互斥）。

- 键码：krkrsdl3 TJS 键码 → ONS(SDL1.2 风格) 键码，映射在 `onscripter/ons_events.cpp`
  （`ONS_MapTJSKeyToOns`）。
- 鼠标键：krkrsdl3 0=左 1=右 2=中；ONSEngine::MapTjsButton 转 ONS 1=左 2=中 3=右。
- 坐标：Layer 事件坐标为图层内坐标，与 ONS 屏幕 1:1（ONS 层尺寸=脚本分辨率）。
