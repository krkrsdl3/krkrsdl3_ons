# krkrsdl3_ons

> ## ⚠️ 重要声明
>
> 本项目**主要用于验证 krkrsdl3 的扩展能力**（把 ONScripter 脚本引擎作为外部
> 语义源接入 krkrsdl3 生态的可行性研究），**仅测试过少量游戏**（CLANNAD 中文
> demo、ever17 中文版等，且均未完整通关验证），**尚不适合实际使用**：存在大量
> 未实现命令、未对齐语义与未验证场景，画面/音频/存档等可能出错或缺失。
> 请勿将其作为可用的游戏运行时对待。

基于 [krkrsdl3](https://github.com/krkrsdl3/krkrsdl3) 开发的 ONScripter 运行时
实验 —— 把上游 Onscripter 的脚本语义融入 krkrsdl3 生态（宿主前端移植思路：引擎保留脚本解析，渲染/音频/输入交给 krkrsdl3）：

- **保留**：ONScripter 脚本解析 / 指令执行（`onscripter/`，语义一律以上游
  Onscripter 源码为准，见 `docs/07` 对照表）；
- **复用**：krkrsdl3 的 Layer 树（画面/文本/图片）、存储系统（`TVPRegisterArchiveFormat`
  接入 NSA/NS2/SAR 封包）、声音栈（BGM/SE/语音）、VideoOverlay（视频）、字体栈；
- **不改 Core**：krkrsdl3 为只读子模块；确需差异时写 `Patch/` 并在根 CMakeLists 替换
  源文件引用（当前无 Patch，输入桥走 TJS Layer 事件，无需改 Core）。

> 术语说明：文中“上游 Onscripter”指随仓库提供的上游参考源码（语义基准与编码表
> 来源，**不参与构建**）；`onscripter/` 为本项目自有实现。

## 构建（Windows / MSVC + vcpkg）

```powershell
# 依赖见 vcpkg.json；CMake 使用 VCPKG_ROOT 工具链
cmake -B build -S .
cmake --build build --config Debug
```

构建产物：`build/Debug/krkrsdl3_ons.exe`（依赖 DLL 与 `Res/`、`demo/` 由
POST_BUILD 自动拷贝到 exe 目录）。

## 运行

```powershell
build\Debug\krkrsdl3_ons.exe C:/完整/路径/demo/
```

- argv[1] = 游戏目录（建议绝对路径，**结尾带路径分隔符**）；`startup.tjs`/
  `AfterStartup.tjs` 在游戏目录内；
- 引擎入口：`demo/startup.tjs` 创建窗口 + ONSLayer（继承 Layer，转发鼠标/键盘事件到
  ONSEngine），`ONSEngine.start()` 解析脚本并启动 tick 驱动 VM；
- `ONSEngine.setEncoding("gbk")` 中文脚本 / `"sjis"` 日文原版；
- `demo/` 内的 CLANNAD 演示数据（nscript.dat / arc*.nsa / OP 视频等）体积较大且属游戏
  版权内容，不入库（.gitignore），自行放置；
- 存档目录 = 游戏目录下 `savedata/`（`ons0.sav` 启动时自动读档，可删除以从开头运行）。

## 架构（指令转发）

```
ONS 脚本（nscript.dat 解密/编码转换/标签/变量/流程，onscripter/ons_vm）
   │ 每条指令转发
   ▼
krkrsdl3 组件（onscripter/ 转发层）
   Layer 树（ons_screen：bg/精灵/文本窗/特效快照层）
   存储（ons_archive：>arc.nsa> 封包，TVPRegisterArchiveFormat）
   声音栈（ons_audio：tTJSNI_WaveSoundBuffer，BGM 循环/SE/语音/音量）
   视频（ons_movie：TJS VideoOverlay + ffmpeg）
   输入（TJS Layer 事件 -> ons_events 队列，主线程 tick 驱动）
```

设计文档：`docs/01`~`docs/08`；项目计划：`docs/plan.md`；上游图层对齐审计：
`docs/align-audit-vs-upstream.md`。

## 开发验证钩子

- `ONSEngine.setTrace(true/false)`：逐行执行日志（`[ONS] <行号>: <脚本行>`）；
- `dumpLines(from,count)` / `dumpScript(file)`：解码脚本行 / 全文（savedata/decoded_script.txt）；
- `dumpLayers(dir)` / `debugShot(w,h,file)`：图层 BMP 转储 / 全屏快照缩略图
  （验证方法详见 `docs/07 §1`）；
- `demo/AfterStartup.tjs`：开发用钩子入口（发行前收敛为单行 `setTrace(false)`）。

## 许可

- 本项目代码沿用 krkrsdl3 生态许可；`onscripter/*2utf16*` 为上游 Onscripter
  （GPL-2.0）编码表的适配副本，保留版权头。
- 上游 Onscripter 参考源码随仓库提供（不参与构建）；CLANNAD 演示数据版权归原作者，
  仅作本地研究用途。
