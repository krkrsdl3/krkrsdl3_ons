# plan.md —— krkrsdl3 × 上游 Onscripter 融合项目计划

> 术语说明：本文的“上游 Onscripter”即随仓库提供的上游参考源码（目录结构见根 README）
> （仅语义基准，不参与构建）。

> 目标：把上游 Onscripter（ONS 脚本引擎）融入 krkrsdl3 生态。核心原则：
> **ONScripter 脚本的解析与指令执行是唯一保留的引擎部分**，每条指令转发到
> krkrsdl3 执行（画面=Layer 树、文本=字体栈+drawGlyph、图片=loadImages、
> 音频=声音栈、视频=VideoOverlay）。**所有语义以上游 Onscripter 源码为准，禁止臆测**（对照表见 docs/07 §3，出处行号已标注）。
>
> 约束（用户定）：自有代码全部在 `onscripter/`；原则上不改 `Core/`（krkrsdl3
> 子模块），确有差异时写 `Patch/` 并在根 CMakeLists 替换源文件引用；尽量不引入
> 新依赖（仅 bzip2 用于 NBZ）；demo 由 `startup.tjs` 最小启动 ONS 引擎。

## 0. 当前状态（2026-09-02，逐条实测，证据见 docs/07 §2）

**已跑通**：CLANNAD 中文 demo 全链路 —— nscript.dat 解密/GBK 转码、标签/流程控制、
bg（磁盘 + `arc1.nsa>` 封包 + 色块 + 尾随特效 crossfade 500ms）、标题精灵（20 层）、
文本行 + 窗体图对（NScr 图对 1280→640 合成）、textbtnwait/btnwait 输入循环、
rmenu 右键菜单、select 选择支（OP 场景点击→*op1）、mov/mpegplay 视频
（VideoOverlay+ffmpeg，mpeg1 352x288→zoom 640x480，窗口截图见画面）、
BGM 循环（arc.nsa 内 ogg）、存档 v2（bg/精灵/文本窗/变量，读档恢复标题画面）、
存档槽位参数、特效注册表（effect 2,10,500=键2 crossfade）与 quake。

**M3 补完已完成**：文本显示模式（print/texec/repaint/saveon/texton/textoff/br）、
特效系统（crossfade + 上游"未实现→crossfade"分支）、存档完整性（v2 画面状态）。

**已验证的架构决策**（沿袭 docs/06 指令转发，不再变更）：
- VM 全主线程 tick 驱动（TVPTimer 16ms），等待点（click/wait/btnwait/select/effect/movie）让出；
- Layer 生命周期由 OnsScreen（C++）管理（docs/04）；
- 输入桥 = TJS Layer 事件 → ons_events 队列（Patch/sdl3_app.cpp 方案已废弃移除）；
- 调试/验证工具：ONSEngine.setTrace/dumpLines/dumpScript/dumpLayers（方法见 docs/07 §1）。

## 1. 里程碑路线

### M3 补完：指令集缺口 ✅（已完成，逐项验证）
1. ✅ 文本显示模式（enter/leaveTextDisplayMode）：
   `print`/`texec`/`repaint`/`saveon`/`saveoff`/`br`/`texton`/`textoff`/`texthide`，
   bg/vsp/btnwait 离开文本模式（语义按上游）。
2. ✅ 特效系统：DEFINE `effect` 注册表 + parseEffect（键查表/行内 effect,duration），
   bg 尾随特效 = 旧画面快照层 crossfade（500ms 实测 507/522ms）；quake/quakex/quakey；
   其余编号沿用上游"未实现→替换 crossfade"分支（ONScripter_effect.cpp:445）。
3. ⏳ 输入/按钮细节：btntime 限时、selectbtnwait、bexec（未实现，见 docs/07 §6）。
4. ✅ 存档完整性：v2 格式（bg 规格/精灵表/文本窗/文本模式/字符串变量），
   读档 RestoreVisualState 重建画面；savegame/loadgame 槽位参数生效。
5. ⏳ 系统调用：lookback 界面/windowerase 未接（M5）。

### M4 验证与收尾
- ✅ `select/selnum` 真实场景端到端验证（OP 选择支：菜单黄标 + 点击 goto）。
- ✅ `mpegplay`（OP/clannad.mpg）端到端验证（ffmpeg 解码 + 窗口截图）。
- ✅ `savescreenshot/savescreenshot2/getscreenshot/deletescreenshot`
  （CompositeSnapshot：bg+可见精灵+文本层合成 -> 缩放 -> save_dir BMP）。
- ✅ 音频音量：bgmvol/sevol/voicevol/getbgmvol/getsevol/getvoicevol。
- ✅ README 使用说明（构建/运行/架构/调试钩子/许可）。
- ⏳ print 的 effect 参数接入特效系统（已核实标题场景上下/游等价，剧情中待查）。
- ⏳ 调试钩子发行前收敛：AfterStartup.tjs -> 单行 setTrace(false)。
- 阶段性 git 提交（每项完成即提交）——已按里程碑提交。

### M5 远期（不在本轮）
- lookback 回想界面、文本历史、kidoku 标记（gloval.sav/kidoku.dat）；
- lsp2/amsp 缩放、cell 动画、ls/ls2 精灵动画序列；
- 存档界面 UI（游戏内 save/load 菜单）；
- 移动端/Web（vcpkg triplet 已有 android/wasm，待验证）；
- LUA（可选，不引入）。

## 2. 目录职责（现状）

| 目录 | 职责 |
|---|---|
| `Core/` | krkrsdl3 子模块，只读（CMake 引用） |
| 上游参考源码 | 上游 Onscripter 参考源码（语义基准 + 编码表来源，不参与构建） |
| `onscripter/` | 自有代码：onscripter.cpp（插件/ONSEngine）、ons_vm（指令 VM）、ons_screen（Layer 生命周期）、ons_archive（NSA/NS2/SAR 封包）、ons_audio、ons_movie、ons_events、coding2utf16/gbk2utf16/sjis2utf16（转码表） |
| `Patch/` | Core 差异覆盖副本（当前为空——输入桥改走 TJS 后无需 Patch） |
| `demo/` | 演示游戏数据（CLANNAD demo：nscript.dat/arc*.nsa/zy.jpg/OP/...）+ startup.tjs/AfterStartup.tjs |
| `docs/` | 调研与设计文档 01-07（01-06 已恢复并修订；07=状态/验证/语义对照） |

## 3. 风险与对策

- **语义偏差**：每个命令实现前先读上游 Onscripter 对应函数（行号在 docs/07 §3），
  以 trace + 解码脚本逐行对照验证执行路径；拿不准时不实现也不乱猜，记入 docs/07 清单。
- **Layer 生命周期**：全部主线程；先停 tick/VM 再销毁（docs/04）。
- **效果与 tick 驱动**：effect 状态机放 OnsScreen.Tick，避免阻塞主循环；
  与上游 `while(doEffect)` 阻塞模型的差异 = 等待点在 VM 侧（WM_EFFECT 等待态）。
- **GPL 许可**：复用上游编码表已标注版权头（onscripter/*2utf16*）；新写代码不复制
  上游实现，仅按语义重写。

## 4. 下一步（下一轮任务）

1. print 的 effect 参数（文本窗 crossfade 出现/消失）接入特效系统（剧情场景验证）。
2. btntime 限时按钮、selectgosub/selectbtnwait。
3. bgm fadein/fadeout（mp3fadein/mp3fadeout）。
4. lookback 回想界面、kidoku 标记（M5）；cell/ls 精灵动画（M5）。
5. AfterStartup.tjs 发行前收敛为单行 setTrace(false)。
