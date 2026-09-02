# ONS 指令转发设计（ons 脚本 → krkrsdl3 执行）

> 核心思想（用户确定的方向）：**把 ONScripter 脚本的解析/指令**作为唯一保留的引擎部分，
> 每条指令的执行**转发到 krkrsdl3**：画面=Layer 树，文本=Layer.drawGlyph/drawText（krkrsdl3 字体栈），
> 图片=Layer.loadImages/TVPLoadGraphic，音频=krkrsdl3 声音栈，视频=krkrsdl3 电影层。
> 不移植引擎的表面渲染管线（accumulation_surface/自绘混合），不做 SDL 兼容层。

## 1. 分层

```
┌─ ONS 脚本层（保留上游解析，on-scripter 语义）──────────────┐
│ ScriptHandler（脚本缓冲/标签/变量/存档）                     │
│ ScriptParser + 命令表（ONScripter_command.cpp 的命令名表）   │
└───────────────┬─────────────────────────────────────────────┘
                │ 每条指令的执行体 = “转发器”（本项目实现）
┌───────────────▼─────────────────────────────────────────────┐
│ 转发层 onscripter/（C++，主线程执行）                         │
│  - ons_screen：ONS 图层组（bg/text/sprite…）与 Layer 生命周期 │
│  - 指令→krkrsdl3 映射表（见 §3）                              │
│  - ons_events：输入桥（Patch 喂入，等待/点击用）               │
│  - ons_archive：>arc.nsa> 存储接入（已完成）                  │
└───────────────┬─────────────────────────────────────────────┘
                │ 调用
┌───────────────▼─────────────────────────────────────────────┐
│ krkrsdl3（执行者，能力见 docs/05）                            │
│ Layer 树（loadImages/drawGlyph/drawText/transition/fillRect） │
│ 存储（TVPCreateStream/归档）· 声音栈 · 电影栈 · 主循环合成      │
└──────────────────────────────────────────────────────────────┘
```

## 2. ONS 图层组（ons_screen 的职责，生命周期仍按 docs/04 §3）

| 角色 | Layer | 说明 |
|---|---|---|
| 背景 | bg Layer（primary） | `bg`/`bgcopy` 指令 → loadImages/assignImages；色块 → fillRect |
| 精灵 | sprite Layers（0..N） | `ld/ls/lsp/amsp/csp…` → 位置/尺寸/alpha/可见性直接映射 Layer 属性；图片 → loadImages |
| 文字 | text Layer | `textgosub` 文本 → drawGlyph/drawText（krkrsdl3 字体栈：字号/颜色/粗体/阴影/描边） |
| 效果 | 转场 | `effect` → Layer.transition（内置转场）；crossfade → opacity 渐变 |

- 全部 Layer 由转发层在主线程创建/持有/销毁；引擎执行线程只投递指令（见 §5）。
- 分辨率：bg 层尺寸=脚本 setwindow 分辨率；OS 窗口由 compositor letterbox。

## 3. 指令 → krkrsdl3 映射（首批子集，可扩展）

| ONS 指令 | krkrsdl3 动作 |
|---|---|
| `bg file` / `bgcopy` | 存储名（含 `>arc.nsa>`）→ Layer.loadImages |
| `ld x,y,file` / `ls` / `lsp` / `amsp` | sprite Layer：loadImages + 属性（位置/可见/alpha） |
| `csp n` / `print` / `cell` | sprite 显示/隐藏/单元格 |
| `>` 矩形 / `color` 指令 | Layer.fillRect / colorRect |
| 文本行 / `setwindow` / `textgosub` | text Layer：drawGlyph/drawText；字体属性 → tTVPFont/TJS Font |
| `click` / `wait` / `waitclick` | ons_events 队列等待（主线程转发输入） |
| `bgm` / `playstop` / `se` / `playvoice` | krkrsdl3 声音栈（iTVPSoundBuffer/WaveSoundBuffer 语义） |
| `effect` / `effectblank` | Layer.transition / opacity 渐变 |
| `movie` / `playmovie` | krkrsdl3 电影层（M4） |
| `jump/gosub/if/mov/inc`… | 引擎变量/流程逻辑（保留） |
| `end` / `quit` | 结束：通知插件 stop（生命周期收尾） |

## 4. 保留的引擎代码（解析/逻辑）

- ScriptHandler.cpp/h（脚本缓冲、标签表、变量、kidoku、存档读写）
- ScriptParser.cpp / ScriptParser_command.cpp（命令解析、DEFINE/EXEC 模式）
- 命令表结构（command_list）与命令名→处理函数映射 —— 处理函数体改为转发实现
- DirectReader/NsaReader/SarReader（读档；NBZ/SPB 解压产物为 BMP → krkrsdl3 LoadBMP 解码）
- 编码表（gbk/sjis/utf8 → UTF-16，文本转换）
- 不保留：AnimationInfo 表面渲染、ONScripter_image/text/effect 的自绘实现、SDL 一切。

> 注：由于命令处理函数分散在 ONScripter*.cpp 中且与表面模型耦合，实际实现采用
> “重写命令执行层”（新文件 onscripter/ons_vm.cpp 等），而非逐文件机械移植。
> 解析层文件可直接复用上游（仅需去除对 ONScripter 类表面的依赖或提供适配头）。

## 5. 线程与执行模型

- **主线程**：Layer 生命周期、指令转发执行、合成由 krkrsdl3 主循环驱动。
- **执行线程**：脚本解析与流程控制（DEFINE 模式建标签；EXEC 模式逐条执行）。
  - 图片/文本/音频等“触碰 Layer/声音栈”的指令**投递到主线程执行队列**（互斥队列，
    主线程 TVPTimer tick 消费，或沿用 TVPPostUserMessage）；流程线程等待结果/事件。
  - 简化替代：整个 VM 放在主线程，用“等待事件”驱动（click/wait 阻塞时让出主循环）——
    与 krkrsdl3 事件模型一致，避免跨线程；采用此方案（TVPDrawSceneOnce 模式）。
- 输入：Patch/sdl3_app.cpp → ons_events 队列 → VM 等待点。

## 6. 落地顺序

1. 解析层接入：编译 ScriptHandler/ScriptParser（去掉 SDL/ONScripter 依赖，提供最小适配头），
   跑通 nscript.dat 解析（标签表/分辨率/变量）；
2. 最小指令集转发：bg/ld/text(整行 drawText)/click/wait/end —— 跑通“首屏 + 点击推进”；
3. 精灵与 alpha/变换、字体属性（粗体/阴影/描边/字号）、矩形色块；
4. 音频转发（iTVPSoundBuffer + 解码器，BGM 循环）；
5. 转场/effect、存档/读档、rmenu、视频（电影层）、LUA（可选）。

## 7. 与之前方案的关系

- docs/04 的“直接移植（表面重命名）”已被本方案取代：连引擎表面模型都不保留，
  指令直接映射 krkrsdl3 能力（docs/05 证明 Layer 生态具备全部媒体能力）。
- docs/04 §3 的 Layer 生命周期规则（创建/缓冲/失效/销毁、主线程限定、先 join 后销毁）继续适用。
