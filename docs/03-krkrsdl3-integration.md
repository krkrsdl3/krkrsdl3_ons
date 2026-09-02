# krkrsdl3 集成点研究（存储 / 图层 / 输入 / 插件 / 主循环）

> 上游 Onscripter 参考源码随仓库提供（不参与构建）。

本文是“把 ONS 引擎挂进 krkrsdl3”所依据的 krkrsdl3（Core 子模块）接口笔记。

## 1. 存储系统（M1 的落点）

头文件：`Core/core/archive/TVPStorage.h`、`TVPArchive.h`。

- 归档分隔符：`TVPArchiveDelimiter = '>'`；`"arc.nsa>inner/file.bmp"` 即“打开封包 arc.nsa 里的 inner/file.bmp”。
- 注册自定义封包格式：

```cpp
typedef tTVPArchive* (*TVPArchiveCreatorFunc)(const ttstr& name, tTJSBinaryStream* st, bool normalizeFileName);
void TVPRegisterArchiveFormat(TVPArchiveCreatorFunc creator);
void TVPUnregisterArchiveFormat(TVPArchiveCreatorFunc creator);
```

  参考实现：`Core/plugins/Kirikiroid2/kirikiroid2.cpp`（注册 TAR/ZIP/7Z 的 creator）。
  `TVPOpenArchive` 会按注册顺序尝试各 creator（读流探测魔数）。
- `tTVPArchive` 抽象：
  - `tjs_uint GetCount()` / `ttstr GetName(tjs_uint idx)`（**必须已归一化**：`tTVPArchive::NormalizeInArchiveStorageName`，
    小写化 + 反斜杠转正斜杠；**必须按名字排序**，供二分查找）/ `tTJSBinaryStream* CreateStreamByIndex(tjs_uint idx)`；
  - 基类提供 `CreateStream(name)`（哈希查找）与 `IsExistent(name)`；
  - 成员流可复用基类 `TArchiveStream`（子区间 seek/read 包装，把“偏移+长度”包成只读流）。
- 存储介质接口 `iTVPStorageMedia`（`TVPRegisterStorageMedia`）：自定义 `media://` 域（如 lzfs/varfile 参考）；
  NSA 不需要它（用 ArchiveFormat 即可），保留备用。
- 常用入口：`TVPCreateStream(name, flags)`、`TVPIsExistentStorage(name)`、`TVPGetPlacedPath(name)`、
  `TVPAddAutoPath(dir)`（把 demo 目录加入自动搜索路径，TJS 侧 `Storage`/`LoadGraph` 就能按名找到文件）。
- 打开流程：`TVPCreateStream(">arc.nsa>X")` → 找 `arc.nsa`（自动搜索路径）→ `TVPOpenArchive` 按注册表探测 → 成员查找 → 流。
- 注意：`TVPArchive` 名字归一化=小写，ONS 索引名=大写 → 实现里转小写后排序即可，查找时同样归一化。

## 2. 图像解码（IMG_* 的落点）

`Core/core/media/image/TVPGraphicsLoader.h`：

- `TVPRegisterGraphicLoadingHandler(tTVPRegisterGraphicInfo*)` —— JPEG/PNG/WEBP 的 handler
  （`Core/plugins/Kirikiroid2/LoadJPEG.cpp` 等），`onscripter/onscripter.cpp` stub 已在 `onscripter_init()` 注册。
- `TVPLoadGraphic(iTVPBaseBitmap* dest, const ttstr& name, keyidx, desw, desh, mode)` —— 从存储名解码到 `tTVPBaseBitmap`（32bit ARGB）。
- `tTVPBaseBitmap`（`Core/core/render/LayerBitmap.h`）：`GetWidth/GetHeight/GetPitch/GetScanLine(y)/GetPixelFormat/GetImageData`，
  像素为 32bit（B/G/R/A 顺序需以 `GetPixelFormat`/实际布局为准，写 IMG 转换时按 krkrsdl3 的 `tTVPBaseBitmap` 布局处理）。
- IMG_Load_RW(shim) = 把内存缓冲包成 `tTJSBinaryStream`（`tTVPMemoryStream`？或临时文件流）→ `TVPLoadGraphic` → 像素拷入 SDL_Surface。
- BMP 解码：krkrsdl3 无 BMP handler，NBZ/SPB 解压产物是 BMP —— shim 内自带简单 BMP loader（24/32bit，RLE 可选）。

## 3. Layer 图层（ONS 屏幕的落点）

- TJS 类 `Layer`（`Core/core/script/tjsNativeLayer.cpp`，构造 `new Layer(layerTreeOwner, parent, w, h, left, top, ...)`）。
- 主层/窗口：TJS 类 `Window`（tjsNativeWindow.cpp）`new Window()` 创建 `tTVPWindow`；
  `wnd.primaryLayer`（只读属性）取主层；`wnd.layerTreeOwnerInterface` 供 Layer 构造。
- C++ 侧像素访问（插件里缓存，避免跨线程调 TJS）：
  - `tTJSNI_BaseLayer::GetMainImagePixelBufferForWrite()` / `GetMainImagePixelBufferPitch()`（tjsNativeLayer.h:486-491）；
  - 或按 LayerExBase 模式：TJS 侧取 `layer.pixels`/`layer.pitch` 缓存（插件 `attachLayer` 时读一次即可，ONS 屏幕尺寸固定）。
- 标脏/上屏：主循环 `SDL_AppIterate` → `Application->Run()` → `krkrsdl3::TVPRenderOnce(w,h)` 合成并呈现（sdl3_app.cpp:543-553）。
  因此引擎线程只要“写完像素 + 置脏标志”，下个主循环 tick 自动上屏 —— 无需改 Core。
- Layer 相关头：`LayerManager.h`（tTVPLayerManager/iTVPLayerManager）、`tjsNativeLayer.h`（tTJSNI_BaseLayer）、`LayerBitmap.h`。

## 4. 输入链路（Patch 的落点）

- OS 事件 → `sdl3_app.cpp` 的 `SDL_AppEvent` → `krkrsdl3::KRKR_Trig_MouseDown/Up/Move/Scroll/KeyDown/KeyUp/TextInput`
  （`Core/core/main/WindowManager.cpp`，经 `TVPPostInputEvent` 进窗口事件队列）。
- 我们的 Patch：`Patch/sdl3_app.cpp`（Core 同名文件完整副本 + 少量改动）在 `SDL_AppEvent` 里额外调用
  `ons_sdl_feed_mouse/ons_sdl_feed_key/ons_sdl_feed_wheel`（声明在 `onscripter/sdl_shim.h`），把事件推入 shim 的 SDL2 事件队列。
- CMake：`SRC_ENVIRON_LIST` 中把 `Core/environ/sdl3/sdl3_app.cpp` 换成 `Patch/sdl3_app.cpp`（用户批准的“差异写 Patch + 改 CMake 引用”机制）。
- 引擎线程 `SDL_PollEvent/WaitEvent` 消费该队列 → 完成输入桥。坐标注意：ONS 层尺寸=屏幕尺寸，直接映射。

## 5. 插件注册（onscripter.dll）

- `Core/plugins/TVPPlugin.cpp`：`TVPLoadInternalPlugins()` 静态注册各 ncbind 模块；`Plugins.link("onscripter.dll")`
  （TJS 侧）→ `TVPLoadPlugin` → `ncbAutoRegister::LoadModule("onscripter.dll")` 按 `NCB_MODULE_NAME` 找到静态注册的模块。
- ncbind：`Core/plugins/ncbind/ncbind.hpp` —— `NCB_REGISTER_CLASS` / `NCB_REGISTER_FUNCTION` / `NCB_PRE_REGIST_CALLBACK` 等；
  现有 `onscripter/onscripter.cpp` stub 就是模板（`NCB_MODULE_NAME TJS_N("onscripter.dll")`）。
- 插件类要能被 TJS 用：ncbind 类 + `NCB_REGISTER_CLASS(ONSScreen)`，方法/属性用 `NCB_METHOD/NCB_PROPERTY`。

## 6. App 主循环与脚本执行

- `sdl3_entry.cpp`：`SDL_MAIN_USE_CALLBACKS`（SDL3 回调式 main）；`SDL_AppInit` → 参数/窗口/后端 → `Application->StartApplication()`。
- `TVPApplication::StartApplication()`（TVPApplication.cpp）：`TVPInitScriptEngine` → `TVPInitializeBaseSystems`
  （设当前目录=TVPProjectDir）→ `TVPLoadPluigins()`（静态模块）→ `TVPInitializeStartupScript()` 执行 `startup.tjs`。
- `TVPInitializeStartupScript`（TVPScript.cpp:1448）：`TVPSearchPlacedPath("startup.tjs")` 自动搜索路径查找 → 执行。
  自动搜索路径：可执行文件目录、`data` 等（`TVPCreateStream` 同款逻辑；demo 目录可 `TVPAddAutoPath` 加入）。
- 之后每帧：`SDL_AppIterate` → `Application->Run()`（消息/定时器）→ `TVPRenderOnce`（合成上屏）。startup.tjs 返回后循环照常跑。
- 因此 ONS 引擎线程启动后，TJS 侧无需再循环：主循环每帧合成 Layer 树即可。
- `TVPDrawSceneOnce(interval)`（sdl3_app.cpp:619）：TJS 可调的“迭代一次+事件泵”，可作参考。

## 7. 音频（M3 的落点）

- `Core/environ/sdl3/sdl3_audio.cpp` 模式：`SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL)` +
  `SDL_CreateAudioStream` + `SDL_BindAudioStream`；krkrsdl3 自身声音走 TVPSoundManager（WaveImpl 等）。
- shim 的 Mix_* 直接用 SDL3 音频设备 + 自己的 PCM 环形缓冲（与 krkrsdl3 设备并存，SDL3 支持多设备）。
- 解码复用：OGG → `libvorbisfile`（vcpkg 已有）；MP3/其他 → ffmpeg（`Core/plugins/wuffmpeg.cpp` 思路：avformat 解封装 + swresample 转 s16）。

## 8. 构建（CMake）

> 注：本节是早期“把上游源码直接编进工程”的尝试笔记，**已不采用** ——
> 上游 Onscripter 仅作语义参考，当前构建不含上游参考源码的任何源文件。

- 根 `CMakeLists.txt`：`SRC_PLUGINS_LIST` 已含 `onscripter/onscripter.cpp`；M2 需要：
  - `file(GLOB ...)` 增加上游引擎源码（排除宿主入口文件）
  - `onscripter/*.cpp`（shim/glue/plugin/archive）+ `Patch/sdl3_app.cpp` 替换
  - include：`onscripter`（提供 SDL2 头）、上游引擎源码目录、`Core/...`（已有）
  - 宏：`WIN32`/`_WIN32`（引擎要求）、项目已有 `_KRKRSDL3_*`
  - 链接：bzip2（vcpkg 新增）、freetype（已有）
- vcpkg.json：`dependencies` 增加 `bzip2`；无需 lua（USE_LUA 关闭）。
