# 上游 Onscripter 引擎架构研究

> 术语说明：本文研究的引擎统称“上游 Onscripter”（参考源码随仓库提供，仅作语义
> 基准，不参与构建）。行号/符号均指该源码。

## 1. 仓库与定位

- 上游基于 ONScripter-Jh（SDL2）的增强分支，支持 Windows/Linux/mac/Android/web/PSV/RetroArch 等平台。
- 引擎本体约 30 个 .cpp / .h（GPL-2.0），直接依赖 SDL2 全家桶（SDL/SDL_image/
  SDL_ttf/SDL_mixer）。
- **宿主前端移植**（把引擎挂进外部运行时的现成思路，本项目沿用）：
  1. 引擎源码**原样编译**，仅替换 SDL2 实现（上游自带约 700 行的“最小 SDL2
     实现”可作参照：提供窗口/事件/定时/线程/表面等子集）；
  2. 宿主 glue 提供引擎全局量（`ONScripter ons`、`coding2utf16`）与入口：
     加载回调 → `ons.openScript()` + `ons.init()` + 起线程跑 `ons.executeLabel()`；
     宿主每帧回调 → 泵输入事件、取帧缓冲刷新画面、取 PCM 送音频。
  本项目完全照搬这个模式，把宿主前端换成 krkrsdl3。

## 2. 启动链路

```
main (onscripter_main.cpp)
  → parseOption(...)                // --root/--font/--enc:... 等
  → ons.openScript()                // 建 NsaReader/DirectReader；读 nscript.dat；解析标签表；得到 screen_width/height
  → ons.init()                      // initSDL() + openAudio()；分配 surface/纹理；开字体
  → ons.executeLabel()              // 主循环（阻塞，直到退出）
```

宿主前端版把 `openScript/init` 放在加载回调，`executeLabel` 放独立线程。
本项目：`ONSScreen.start()`（TJS 调用）里做同样三件事。

## 3. 关键类

| 类 | 文件 | 职责 |
|---|---|---|
| `ONScripter` | ONScripter.cpp/h + 若干 ONScripter_*.cpp | 引擎主体：状态、命令执行、事件循环、绘制 |
| `ScriptHandler` | ScriptHandler.cpp/h | 脚本缓冲、变量表、标签、kidoku 标记、读写存档 |
| `ScriptParser` | ScriptParser.cpp/h + ScriptParser_command.cpp | 命令解析与执行（DEFINE_MODE/EXEC_MODE 双模式） |
| `BaseReader` / `DirectReader` | DirectReader.cpp/h | 直接文件读取 + NBZ/SPB/LZSS 解压 + 大小写回退 |
| `NsaReader` | NsaReader.cpp/h | arc*.nsa / arc*.ns2 / arc.sar 索引与读取 |
| `SarReader` | SarReader.cpp/h | SAR 头解析（NsaReader 的基类） |
| `AnimationInfo` | AnimationInfo.cpp/h | 精灵/动画（图片 + 变换 + 透明色） |
| `FontInfo` | FontInfo.cpp/h | 字体信息（ttf 路径、字号、颜色） |
| `DirtyRect` / `resize_image` | — | 脏矩形 / 缩放 |
| `LUAHandler` | LUAHandler.cpp | Lua 扩展（`USE_LUA` 编译开关，本项目默认关闭） |
| `Parallel` | Parallel.cpp | 线程池并行图像解码（`USE_PARALLEL`，可关） |

## 4. SDL2 API 面（仅作理解参考，不移植）

> **重要**：本文描述的是上游引擎的原始实现。本项目不移植引擎的渲染/事件/音频实现，
> 而是**指令转发**到 krkrsdl3（见 docs/06）；下表仅用于理解引擎内部依赖与工作量参考。

引擎直接调用的 SDL2 符号清单（grep 统计）分类：

- **视频/渲染**：SDL_CreateWindow / CreateRenderer / CreateTexture(FromSurface) / UpdateTexture / RenderCopy /
  RenderClear / RenderPresent / RenderReadPixels / RenderSetLogicalSize / SetRenderDrawColor / GetRendererInfo /
  GetRendererOutputSize / SetWindowFullscreen / SetWindowTitle / GetWindowSize / GetWindowFlags / SDL_GL_SetAttribute（可空转）/
  YUV Overlay（Create/Display/Free/Lock/Unlock，M4 处理）
- **表面**：SDL_CreateRGBSurface / BlitSurface / BlitScaled / FillRect / ConvertSurface / Lock/UnlockSurface /
  SetSurfaceBlendMode / FreeSurface / SaveBMP_RW / MapRGBA / PixelFormat / SDL_PIXELFORMAT_* / SDL_BYTEORDER
- **事件**：SDL_PollEvent / WaitEvent / PushEvent / PeepEvents / SDL_Event 结构（MOUSEBUTTON/MOTION/WHEEL、KEYDOWN/UP、
  FINGER、CONTROLLER、WINDOW、QUIT、USEREVENT）/ SDL_BUTTON_* / SDLK_*
- **定时**：SDL_GetTicks / SDL_Delay / SDL_AddTimer / SDL_RemoveTimer（用户事件唤醒等待）
- **线程**：SDL_CreateThread / WaitThread / DetachThread / SDL_mutex（Create/MutexP/MutexV/Destroy）/ SDL_Semaphore / SDL_Atomic*
- **文件**：SDL_RWFromFile / Mem / ConstMem / FP / SDL_RWclose / RWops 读写 seek 接口 / SDL_SaveBMP_RW
- **图像(SDL_image)**：IMG_Load_RW / IMG_LoadJPG_RW / IMG_isPNG / IMG_GetError
- **字体(SDL_ttf)**：TTF_Init / OpenFontRW / RenderGlyph_Shaded / FontHeight / FontAscent / GlyphMetrics /
  SetFontOutline / SetFontStyle / GetFontStyle / STYLE_NORMAL/BOLD / GetError
- **音频(SDL_mixer)**：Mix_OpenAudio / QuerySpec / LoadWAV_RW / LoadMUS_RW / PlayChannel / PlayMusic / HaltMusic /
  Pause/Resume(PauseMusic/ResumeMusic) / Volume / VolumeMusic / HookMusic / HookMusicFinished / ChannelFinished /
  FreeChunk / FreeMusic / AllocateChannels / CloseAudio / SetMusicPosition / SetMusicCMD / Playing / PlayingMusic / GetError
- **杂项**：SDL_Init / InitSubSystem / GetError / SetHint / GetMouseState / WarpMouseInWindow / ShowCursor /
  CaptureMouse / GetNumTouchFingers / GameControllerOpen/Close / CD*（可 stub）/ SDL_atof / SDL_strdup

≈ 120 个符号；上游自带的“最小 SDL2 实现”（约 700 行）可作参照。

## 5. 渲染流程（每帧）

1. 命令执行产生绘制：各动画帧 blit 到 `accumulation_surface`（整屏 32bit surface）。
2. `SDL_UpdateTexture(texture, NULL, accumulation_surface->pixels, pitch)`
3. `SDL_RenderClear(renderer)` → `SDL_RenderCopy(renderer, texture, NULL, NULL)`（引擎默认 1:1）→ `SDL_RenderPresent(renderer)`
4. 特效（crossfade 等）在 effect_*_surface 间做 surface blit（我们全软件实现即可）。

引擎另有 `render_view_rect / screen_scale_ratio`（拉伸适配逻辑）——shim 里让 RenderCopy 目标=全屏即可。

## 6. 事件循环

`runEventLoop()`：`SDL_WaitEvent` 阻塞；吞并连续 MOUSEMOTION；分发 mousePress/mouseMove/keyDown/…；
动画推进用 `SDL_AddTimer` 定时推 `ONS_BREAK_EVENT`（用户事件）唤醒。
=> shim 的事件队列必须支持“跨线程 Push + 阻塞 Wait + 定时器线程”。

## 7. 字体

`FontInfo` + `ONScripter_text.cpp`：`TTF_OpenFontRW(默认 default.ttf)`，逐字形 `TTF_RenderGlyph_Shaded`
（8bit alpha 表面 + fg/bg 色），缓存字形表面（`font_cache`）。TTF_SetFontOutline 仅在 `--render-font-outline` 时用。
=> shim 用 FreeType（krkrsdl3 已依赖）实现：FT_New_Memory_Face → FT_Load_Glyph → FT_Render_Glyph(NORMAL) → 8bit 位图。

## 8. 图像

`ONScripter_image.cpp`：`createSurfaceFromFile` → 引擎 reader 读整文件进内存 → `IMG_Load_RW` / `IMG_LoadJPG_RW`；
`>w,h[,colors]` 语法生成纯色矩形面；`has_alpha` 由格式/PNG 判断。
=> shim 的 IMG_Load_RW 走 krkrsdl3 `TVPLoadGraphic`（JPEG/PNG/WEBP handler 已在 onscripter.cpp stub 注册），
把 tTVPBaseBitmap 像素拷贝到 SDL_Surface。BMP 之外格式（如 NS2 时代的 .bmp）krkrsdl3 未必有 handler——BMP 解码需自带
（引擎里有 bmp 读取代码？无——但 NBZ/SPB 解出来就是 BMP，需要自带 BMP 解码进 SDL_Surface；计划放在 M2 内实现简单 BMP loader）。

## 9. 音频

`ONScripter_sound.cpp`：整文件读入内存 → `Mix_LoadWAV_RW`（chunk，SE/语音）/ `Mix_LoadMUS_RW`（music，BGM，格式由 SDL_mixer 决定：mp3/ogg/…）；
`Mix_PlayChannel`（16 通道）与 `Mix_PlayMusic`（单路）；音量 Mix_Volume/Mix_VolumeMusic；`Mix_HookMusic` 拉流（M3 用）。
=> M3 实现：WAV(PCM) 自解；OGG 用 libvorbisfile（vcpkg 已有）；MP3/其他用 ffmpeg（krkrsdl3 已链接）；
PCM 进环形缓冲 → SDL3 `SDL_OpenAudioDevice`+`SDL_BindAudioStream`（与 krkrsdl3 sdl3_audio.cpp 同款模式）。

## 10. 其他

- 存档：`save_dir` 下 `*.sav`（NScr 格式，ScriptHandler 读写，FILE*）；lookback 等。
- 视频：AVIWrapper（USE_AVIFILE 才编）与 `SDL_Overlay` 路径；无则 `video_init` 失败降级（M2 可接受）。
- 全局量：`ONScripter ons`、`Coding2UTF16 *coding2utf16`、`g_stdoutpath/g_stderrpath` 由“宿主 glue”提供
  （上游在宿主入口文件里定义，本项目在 `ons_glue.cpp` 提供）。
