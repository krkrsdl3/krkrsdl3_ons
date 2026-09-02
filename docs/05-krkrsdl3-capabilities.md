# krkrsdl3 能力清单（Layer 生态：字体/图片/文本/音频/视频）

> 结论先行：krkrsdl3 的 Layer 生态**已经具备 ONS 引擎所需的全部媒体能力**——
> 字形光栅化、文本绘制、图像加载、音视频播放都在 Layer/媒体栈上原生可用。
> 因此 ONS 集成应做**指令转发**（见 docs/06），而不是自带解码/渲染实现。

## 1. 字体（media/font + Layer）

| API | 位置 | 说明 |
|---|---|---|
| `tTVPFont` / `tTVPFontstruc.h` | media/font | 字体描述（面名/字号/粗体/阴影/描边/行距…） |
| `FontRasterizer` 接口 | FontRasterizer.h | `ApplyFont` / `GetTextExtent` / `GetAscentHeight` / `GetBitmap(tTVPFontAndCharacterData, aofsx, aofsy) -> tTVPCharacterData*` / `GetGlyphDrawRect` |
| `FreeTypeFontRasterizer` | FreeTypeFontRasterizer.h | krkrsdl3 自带的 FreeType 实现（含 fallback face） |
| `tTVPCharacterData` | CharacterData.h | 单字形位图（`GetData()` 8bit 覆盖度 + OriginX/Y + Metrics + Pitch + BlackBoxX/Y），可 Blur/Bold |
| **Layer.drawGlyph(x, y, charData, color, …)** | tjsNativeLayer.cpp:8393 | 把字形画进 Layer 位图 —— **ONS 逐字绘制的直接落点** |
| **Layer.drawText(x, y, text, color, …)** | tjsNativeLayer.cpp:8372 | 整串文本绘制（走层字体与光栅化链） |
| tTVPNativeBaseBitmap::DrawText/DrawTextSingle/Multiple | LayerBitmap.cpp | 位图级文本绘制（含阴影/描边） |
| TJS `Font` 类 | script/tjsNativeFont | TJS 侧字体对象（face/size/bold/shadow/edge…），赋给 Layer.font |
| TextRender 插件 | plugins/textrender.cpp | 富文本排版（render/setRenderSize/keyWait/renderCount…），可做 lookback/对话框高级排版 |

ONS 侧需求映射：字号/颜色/粗体/阴影/描边 → tTVPFont（或 TJS Font）；逐字形绘制 → Layer.drawGlyph；
整行文本 → Layer.drawText。

## 2. 图片（media/image + TVPGraphicsLoader + Layer）

| API | 说明 |
|---|---|
| `TVPLoadGraphic(dest, name, keyidx, desw, desh, mode)` | 按存储名解码到 `tTVPBaseBitmap`（32bit RGBA） |
| **`Layer.loadImages(name, colorkey)`** | 直接把存储图像载入 Layer 位图（返回状态对象）—— **ONS 图片指令的直接落点** |
| `Layer.assignImages(srcLayer)` / `AssignMainImage(bmp)` | 位图赋值 |
| 内置格式 | **BMP（core/media/image/LoadBMP.cpp，含 RLE）**、TLG（LoadTLG.cpp）—— NBZ/SPB 解出的 BMP 直接可用 |
| 插件格式 | PNG/JPEG/WEBP（Kirikiroid2 插件 handler，onscripter 模块初始化时注册） |
| `tTVPBaseBitmap` | LayerBitmap.h：GetScanLine/GetPitch/GetWidth/GetHeight/BitBlt/CopyRect/填充… |
| 图像缓存 | TVPAllocGraphicCacheOnHeap / TVPClearGraphicCache 等 |

ONS 侧需求映射：`bg/ld/ls` 等指令 → 存储名（含 `>arc.nsa>` 归档路径）→ Layer.loadImages；
矩形色块（`>w,h,color` 语法）→ Layer.fillRect/colorRect；alpha 由格式自带。

## 3. 文本/输入相关（Layer 本身）

- Layer 原生方法（tjsNativeLayer.cpp）：`fillRect`、`colorRect`、`drawText`、`drawGlyph`、
  `loadImages`、`assignImages`、`transition`（内置转场）、`update`、`visible/opacity/width/height/left/top` 属性。
- 输入：krkrsdl3 主线程 SDL_AppEvent → KRKR_Trig_* → 窗口事件队列 → TJS Layer 事件；
  ONS 侧由 startup.tjs 的 ONSLayer 事件方法转发到 ons_events 队列（VM 主线程消费），
  见 docs/04 §4（不再使用 Patch/sdl3_app.cpp）。

## 4. 音频（media/sound + environ/sdl3 + 插件解码器）

| API | 说明 |
|---|---|
| `TVPInitDirectSound(freq=48000)` | 初始化输出（WaveDecodeThread/VideoPlayer 已自动调用；可重复调用） |
| `TVPCreateSoundBuffer(tTVPWaveFormat&, bufcount)` -> `iTVPSoundBuffer` | 输出缓冲：`Play/Pause/Stop/Reset/IsPlaying/SetVolume/SetPan/AppendBuffer(buf,len)` 流式写入 —— **ONS 音频指令的直接落点** |
| `tTVPWaveFormat` | WaveFormatConverter.h：采样率/通道/格式（s16/f32） |
| `TVPCreateWaveDecoder(storagename)` | 按存储名建解码器（tTVPWaveDecoder：GetFormat/Render/SetPosition），由注册的 creator 分派 |
| 解码器 | RIFFWaveDecoder（core，WAV）、wuffmpeg（MP3/ffmpeg）、wuvorbis（OGG）、wuopus（Opus）插件 |
| TJS `WaveSoundBuffer` | script/tjsNativeWaveSoundBuffer：open(storage)/play(loop,fade)/stop/pause/volume/position + 循环与淡入淡出、标签事件 —— BGM 循环播放可直接用 |
| `WaveLoopManager` | 循环段管理（循环点、标签） |
| iTVPSoundBuffer | 多缓冲流式播放（AppendBuffer），sdl3_audio.cpp 用 SDL_AudioStream 实现 |

ONS 侧需求映射：SE/语音（一次性）→ iTVPSoundBuffer 一次性播放；BGM（循环/淡出）→
WaveSoundBuffer 语义（或 iTVPSoundBuffer + 循环重挂）；音量/暂停 → SetVolume/Pause。
引擎现有调用（Mix_*）在转发层里逐个映射到上述 API。

## 5. 视频（media/movie + Layer）

| API | 说明 |
|---|---|
| `TVPMoviePlayer`（VideoPlayer.cpp，55KB） | ffmpeg 解封装/解码（CodecDemuxFFmpeg/CodecVideoFFmpeg/CodecAudioFFmpeg），音视频同步（VideoReferenceClock），消息队列 |
| `KRMovieLayer`（MoviePlayerLayer） | 把视频帧呈现到 `tTVPBaseTexture` 双缓冲 —— 可接 Layer 树 |
| `KRMovieOverlay` | 视频覆盖层 |
| `iTVPVideoCallback` | 视频回调接口（BuildGraph 时传入） |
| AlphaMovie 插件 | plugins/AlphaMovie.cpp：TJS `MovieLayer`（在 Layer 上播视频，ffmpeg） |

ONS 侧需求映射：`playmovie/playvideo` → MoviePlayerLayer/AlphaMovie（M4）。

## 6. Layer 生命周期相关（回顾）

- 创建：`new Window()`（tjsNativeWindow）→ `new Layer(wnd, null)` 成为 primary layer；或 C++ 侧
  CreateNew 等价（ons_screen.cpp 已实现）。
- 图像分配：SetSize 后 `hasImage=true` → MainImage（tTVPBaseTexture）；`GetMainImagePixelBufferForWrite/Pitch`。
- 失效/合成：`LayerManager::RequestInvalidation(rect)` → AddUpdateRegion → NotifyWindowInvalidation
  → UpdateToDrawDevice → Show（DrawDevice.cpp/LayerTreeOwner.cpp）。
- 销毁：`Invalidate()` 自动 DetachPrimary/UnregisterSelfFromWindow/Part（tjsNativeLayer.cpp:501）。
- 转场：Layer.StartTransition（内置转场处理器），ONS effect 可映射。

## 7. 结论（对 ONS 集成的影响）

- **ons_font / ons_image / ons_audio / 表面 API 不再需要自研**：全部转发到上表 krkrsdl3 API。
- 保留的“自有”部分：ons_archive（封包）、ons_events（引擎线程输入桥）、ons_screen（Layer 生命周期）。
- 引擎侧改造方式：**指令转发**（docs/06）——保留脚本解析，命令执行改为调用 krkrsdl3 组件。
