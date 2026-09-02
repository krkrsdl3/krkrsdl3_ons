# 实现状态与验证方法（2026-09-02 基线，随里程碑更新）

> 本文记录当前 onscripter/ 的实现状态、端到端验证方法与已验证结论，以及
> “语义以上游 Onscripter 为准”的对照表。所有结论都以真实运行 + 日志/像素证据为准，
> 不臆测。

## 1. 验证方法（可复用）

1. **执行路径验证**：`ONSEngine.setTrace(true)`（demo/AfterStartup.tjs）→ 每行执行
   打印 `[ONS] <行号>: <脚本行>`；与 `ONSEngine.dumpLines(from,count)` 输出的解码后
   脚本逐行对照，确认分支/跳转/等待点与上游 Onscripter 语义一致。
2. **渲染验证**：`ONSEngine.dumpLayers(dir)` → 把 bg/文本/菜单/转场快照/精灵层经
   `tTJSNI_BaseLayer::SaveLayerImage` 存成 BMP（savedata/<N>_/dbg_*.bmp）。
   图层像素缓冲读回可靠（CPU 驻留纹理；LoadImages/FillRect 内容均可读回）。
   再用 PowerShell（System.Drawing）统计主色 / 与源图逐像素比对。
3. **脚本离线检索**：`ONSEngine.dumpScript(file)` → savedata/decoded_script.txt
   （解码后全文，行号=VM pc），grep 定位 select/mov 等场景并构造对应存档。
4. **输入注入**：TJS Timer 里直接调 `onsEngine.onMouseDown(x,y,button,shift)`。
   注意 krkrsdl3 鼠标键 0=左 1=右 2=中（mbLeft/mbRight/mbMiddle）。
5. **音频/视频**：stderr `[AUDIO]`（ons_audio.cpp）、`[MOVIE]`（ons_movie.cpp）、
   `[ONS] -- fx start/end`（转场生命周期）与 ffmpeg "Video zoom" 行。
6. **进程**：`krkrsdl3_ons.exe <绝对路径 demo/>`，stderr 重定向到文件；
   `Start-Process -RedirectStandardError`。
7. **窗口截图**（辅助）：DPI 144（150%）下 GetWindowRect 给逻辑坐标，需按
   `GetDpiForWindow/96` 换算物理坐标再 CopyFromScreen。层转储更可靠，优先用层转储；
   视频覆盖层（VideoOverlay）不在 Layer 树内，只能窗口截图验证。

## 2. 已验证结论（CLANNAD 中文 demo 数据；逐条有日志/像素证据）

| 项 | 结论 | 证据 |
|---|---|---|
| nscript.dat 解密+GBK 转码 | ✅ 中文文本正确渲染 | 解码行 [DUMP] 输出 / decoded_script.txt |
| 标签/流程（jump/gosub/return/if/mov/数组） | ✅ 执行路径与脚本逐行一致 | trace 对照 |
| bg 磁盘图（zy.jpg） | ✅ 640x480 正确载入 | 层转储主色 148,140,138 与窗口截图一致 |
| bg 白色/矩形色块 | ✅ | 层转储全白、色块色值精确 |
| bg 封包图（arc1.nsa>bj/*.jpg） | ✅ 载入成功 | `[ONS] -- bg ... -> 'arc1.nsa>...'` + 像素比对（s1.jpg 本身即白闪帧） |
| 标题精灵 lsph/lsp（20 层） | ✅ | dbg_sp*.bmp |
| 文本行+文本框（form2.jpg 窗背 + 字形） | ✅ 灰窗+黑字像素 | 文本层转储 |
| textbtnwait / btnwait 输入循环 | ✅ 左键=0 / 右键=-1 开 rmenu | trace + 逐页推进 |
| rmenu 右键菜单流程 | ✅ | trace（403045-403106 循环） |
| BGM（bgm26a/bgm30.ogg，arc.nsa 内） | ✅ 循环播放 | `[AUDIO] start ... loop=1` |
| select 选择支 | ✅ 菜单绘制（黄=当前项）+ 点击 goto 目标 | OP 场景点击第一项 → *op1；文本层转储 0xffff00 |
| mov/mpegplay 视频（OP/clannad.mpg） | ✅ ffmpeg 解码（mpeg1 352x288→zoom 640x480）+ 窗口截图见蓝天白画面 | `[MOVIE] play` + Video zoom 行 + 截图 |
| 特效：effect 注册表 + bg 尾随特效 | ✅ `effect 2,10,500`→键2=crossfade500ms；WM_EFFECT 阻塞 | `[ONS] -- fx start/end` 时长 507/522ms；快照层淡出 |
| quake/quakex/quakey | ✅ 实现（未在 demo 触发） | OnsScreen Tick 抖动 |
| 存档 v2（bg/精灵/文本窗/文本模式/字符串变量） | ✅ 读档恢复标题画面（bg s2+20 精灵+窗体参数） | 读档后层转储=标题；pc 直达 178 |
| 存档槽位（savegame/loadgame n） | ✅ 参数生效 | ons1.sav |

## 3. 语义对照表（实现 ↔ 上游 Onscripter 依据）

所有命令语义出处：上游命令实现文件 `ONScripter_command.cpp`（行号为当前
上游快照），命令名表在 `ONScripter_lut.cpp`（`command_list[]`）。

| 命令 | 上游依据 | 现状 | 差距 |
|---|---|---|---|
| `bg` | bgCommand (command.cpp:3764) | ✅ 图/色块/white/black + 尾随特效（crossfade 等） | leaveTextDisplayMode 已做；其余特效编号按上游替换 crossfade |
| `ld/lsp/lsph/lsp2/amsp/msp/vsp/csp` | lspCommand(:1699) 等 | ✅ 基本（载图/位置/alpha/显示隐藏 + 状态记录） | cell 动画/ls 系列未实现；缩放模式未实现 |
| `print` | printCommand(:1246) | ⚠️ 离开文本模式已做，特效未接 | print 的 effect 参数（标题 print 10,500 现为即时） |
| `texec` | texecCommand(:295) | ✅ newPage + saveon | processEOT 细节 |
| `repaint` | repaintCommand | ✅ 等价实现（Layer 自动标脏） | — |
| `saveon/saveoff` | saveonCommand(:1023) | ✅ 标志 | autosaveoff 未实现 |
| `effect/effectblank/windoweffect/windowback` | effectCommand + ONScripter_effect.cpp | ✅ 注册表 + crossfade + quake + 未实现编号替换 crossfade | 百叶窗/帘幕/马赛克/掩膜真实现（上游同位置也替换 crossfade） |
| `quake/quakex/quakey` | quakeCommand(:1142) | ✅ 随机/正弦抖动阻塞 | — |
| `textbtnwait/btnwait/btnwait2/selectbtnwait/bexec` | btnwaitCommand(:3355) | ✅ 基本 | btntime 限时/selectbtnwait 结果差异 |
| `wait/delay` | waitCommand(:131)/delayCommand(:3096) | ✅ wait=WM_TIMER 不可跳过；delay=WM_TIMER+WAIT_INPUT_MODE（左键/回车/空格跳过，delay 7000 点击即过实测） | — |
| `select/selnum` | selectCommand | ✅ 端到端验证（OP 场景） | selectgosub/selectbtnwait |
| `savegame/loadgame` | savegameCommand(:1042) | ✅ 槽位参数 + v2 画面状态 | savegame2 字符串 |
| `systemcall` | systemcallCommand(:357) | ⚠️ 部分 | lookback 界面/windowerase 等未接 |
| `savescreenshot/savescreenshot2/getscreenshot/deletescreenshot` | savescreenshotCommand(:968)/getscreenshotCommand(:2333) | ✅ 快照(bg+精灵+文本合成)→缩放→save_dir BMP；debugShot 验证 160x120 缩略图 | 合成近似 accumulation_surface（无特效层参与） |
| `mov/mpegplay` | aviCommand/mpegplayCommand | ✅ 端到端验证（VideoOverlay + ffmpeg） | 循环/点击跳过参数已接 |
| `bgm/bgmstop/se/playstop/playvoice/dwave/dwavestop` | playSound 系列 | ✅ | — |
| `bgmvol/sevol/voicevol/getbgmvol/getsevol/getvoicevol` | mp3volCommand(:1396)/sevolCommand(:647)/voicevolCommand(:161) | ✅ 0-100 -> 0-100000，即时+起播应用 | fadein/fadeout 未实现 |
| `setwindow/setwindow3` | setwindowCommand | ✅ 基本 | 窗背图对合成（1280x120→640x120）已按 NScr 图对语义处理 |
| `textgosub` | textgosubCommand | ✅ 基本 | CLICK_NEWPAGE/CLICK_EOL 状态机细节 |
| `texton/textoff/texthide/erasetextwindow/br` | 各自 command | ✅ | — |
| `rmenu/menusetwindow/menuselectcolor` | rmenuCommand | ✅ 基本 | 菜单项滚动（>10 项） |
| `if/notif/for/next/mov/inc/dim/itoa2/len/rnd/...` | 表达式求值 | ✅ | div/mod 部分命令待查 |
| `lookbacksp/lookbackcolor/lookback` | lookbackCommand | ⚠️ rmenu 入口已接 | 回想界面本体未实现 |
| `nega/bar/ofscopy/drawbg/drawbg2/draw/tblgoto/...` | 各自 command | ❌ 未实现 | 低频命令 |

## 4. 特效表（ONScripter_effect.cpp doEffect 的 case）

| 编号 | 名称 | 编号 | 名称 |
|---|---|---|---|
| 0/1 | 立即显示 | 11-14 | 左/右/上/下滚动（scroll） |
| 2-5 | 左/右/上/下百叶窗（shutter） | 15 | 掩膜淡入（fade with mask） |
| 6-9 | 左/右/上/下帘幕（curtain） | 16/17 | 马赛克出/入 |
| 10 | 交叉淡化（crossfade） | 18 | 掩膜交叉淡化 |
| 99 | DLL 特效（breakup/cascade/whirl/trvswave，builtin_dll 有实现） | MAX+0..2 | quakey/quakex/quake；MAX+3 flushout |

- 定义：`effect 编号,参数,时长(ms)`（DEFINE 段）；`effectcut` 跳过缩短时长。
- 执行模型：`setEffect`（生成 effect_src = 当前屏，effect_dst = 新画面）→
  `while (doEffect(el))` 阻塞推进（引擎内 effect_counter/effect_duration）。
- krkrsdl3 映射：crossfade → 两层 opacity 渐变（或 Layer.StartTransition）；
  百叶窗/帘幕/马赛克 → 层蒙版动画（M4，用 Layer bitmap 逐帧生成）；quake → 层位移。
- CLANNAD 标题用 `print 10,500`（文本层 crossfade 出现），bg 常用 effect 2。

## 5. 上游语义要点笔记（防臆测）

- `bgCommand`：先 `leaveTextDisplayMode()`；"white"/"black" 名字特判；移除 3 个立绘
  （tachi_info）与 bg_info；`parseEffect(true)` 解析尾随 effect 参数并阻塞执行。
- `btnwaitCommand`：btnwait/btnwait2 先 `leaveTextDisplayMode()`（btnwait 还 del_flag）；
  进入 WAIT_BUTTON_MODE；textbtnwait 加 WAIT_INPUT_MODE（空白点击 → 0，右键 → -1）；
  btntime 超时 → -2；exbtn 悬停由 mouseOverCheck/decodeExbtnControl 处理。
- `texecCommand`：`textgosub_clickstr_state == CLICK_NEWPAGE` → newPage()；
  `CLICK_WAIT|CLICK_EOL` → processEOT() + page_enter_status=0；最后 saveon_flag=true。
- `printCommand`：= leaveTextDisplayMode + parseEffect(true) + doEffect（阻塞）。
  即“文本显示模式退出/进入的转场”；CLANNAD 标题 `print 10,500` = 文本窗 crossfade 出现。
- 存档：上游 saveAll 保存 sprite_info/bg_info/tachi_info（文件名+位置+可见性）等，
  读档后 `loadSaveFile` 重建全部画面 —— 本项目 LoadState 需补齐此部分（见 docs/plan.md M4）。
- 编码：`coding2utf16` 默认 GBK；`--enc:sjis` 日文；UTF-8 透传。本项目 VM 全 UTF-8。

## 6. 遗留问题清单（当前入口）

1. `print` 的 effect 参数（print 10,500 → 文本窗 crossfade）未接特效系统；
   标题场景已核实为上/下游等价（全屏自交叉淡化，无可见变化），剧情中的 print 需复查。
2. 百叶窗/帘幕/马赛克/掩膜特效未真实实现（沿用上游"未实现→替换 crossfade"分支）。
3. btntime 限时按钮、selectgosub/selectbtnwait、bexec 未实现。
4. bgm fadein/fadeout（mp3fadein/mp3fadeout）未实现。
5. lookback 回想界面、kidoku 标记、gloval.sav（M5）。
6. cell/ls/ls2 精灵动画序列、lsp2/amsp 缩放（M5）。
7. 调试钩子（AfterStartup.tjs）发行前收敛为单行 setTrace(false)。
