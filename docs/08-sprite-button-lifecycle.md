# 精灵(sprite)/按钮(button)存活周期与清理语义报告（对照上游 Onscripter）

源码: 上游 Onscripter（下文 `cmd`=ONScripter_command.cpp, `evt`=ONScripter_event.cpp, `rm`=ONScripter_rmenu.cpp, `f2`=ONScripter_file2.cpp, `img`=ONScripter_image.cpp, `anim`=ONScripter_animation.cpp, `txt`=ONScripter_text.cpp, `eff`=ONScripter_effect.cpp; 无前缀=ONScripter.cpp)

## 0. 数据模型与"清理"对象
- 精灵=sprite_info[1000]/sprite2_info[256] 两个 AnimationInfo 数组 (ONScripter.h:58-59,476)。AnimationInfo::remove() 只删 image_name/surface/tags (AnimationInfo.cpp:184-189)，**不改 visible**；reset()=remove()+visible=false(131-155)；removeTag() 把 current_cell/num_of_cells 归零(191-219)；setCell 越界钳制到 [0,num_of_cells-1](283-286)。
- 按钮=ButtonLink 单向链 root_button_link (ONScripter.h:375)。SPRITE_BUTTON 仅引用 sprite_no 不持图；NORMAL/TMP_SPRITE 自持 anim[0..1]，析构时 delete (ButtonLink.h:29-59)；exbtn_ctl[3] 为 exbtn 控制串。removeSprite(no) 只摘掉 sprite_no==no 的 SPRITE_BUTTON 结点 (ButtonLink.h:66-79)。
- deleteButtonLink() 只删链上所有结点 + exbtn_d 的 ctl 数组 + is_exbtn_enabled=false + current_over_button=-1 (1176-1195)，**完全不动任何精灵**。

## A. 精灵(AnimationInfo+layer)
1. lsp/lsph (cmd:1699-1730)：整格覆盖该 sprite_no。visible 由 lsph 显式置 false、lsp 置 true(1704,1711)；parseTaggedString→removeTag() 使 current_cell 归 0，num_of_cells 由新串标签决定；setupAnimationInfo 中同名文件早退不重建表面(anim:141-146)，换名才 deleteSurface 重载(148)。即"重复 lsp 同一文件=只改 pos/visible/trans，surface 沿用；换文件=彻底换图"。
2. cell (cmd:3326-3335)：按显式 sprite_no 调 setCell，钳制越界；不改 visible。不存在"当前精灵"概念。
3. csp/csp2 (cmd:3131-3178)：先 leaveTextDisplayMode()(3133)。no==-1 全清：逐项把可见者记 dirty→若有 image_name 把 orig_pos 挪到 (-1000,-1000)→root_button_link.removeSprite(i)(仅非 csp2)→si[i].remove()(3150-3165)；0≤no 单清(3166-3175)。其余负数无操作。remove() 不清 visible，但表面已删故不绘制(绘制条件 image_surface&&visible, img:728)。csp n 会顺带删引用该 sprite 的按钮；csp2 不碰按钮链(3163,3173 的 !csp2_flag 分支)。
4. visible 的修改点：lsp/lsph(1711)、vsp/vsp2(cmd:139-159)、cell 不改、texton/textoff 只切 windowchip 精灵 (cmd:249-271, windowchip 号在 define 里定 ScriptParser_command.cpp:52-56)、exbtn ctl→refreshSprite(img:818-834) 设 visible=active、allsphide/allspresume 只置全局旗标不动个体(3943-3974)、strsp/logsp 收尾置 true。
5. 文字精灵 `:s` 标签=TRANS_STRING，cell 数=后随 #RRGGBB 颜色数(anim:293-333)，供按钮式变色(见 B)。bg_info/立绘 tachi_info[3] 独立于 sprite 数组：bg 命令 remove 立绘+背景(3764-3795:3781-87)，**不碰 sprite_info**；bgcopy 把当前合成帧拷成新 bg(3750-3762)。层序：bg→大号精灵(>z_order, 默认499, ScriptParser.cpp:157)→立绘→(sprite2/窗/文字)→小号精灵(≤z_order)→bar/prnum→按钮(img:709-816)。换场景用 bg/lsp 不会自动带走旧精灵。
6. 移动命令 msp/amsp(+2 版) 只改 orig_pos/scale/rot 与 dirty(1337-1379,3888-3927)，与清理无关。

## B. 按钮生命周期
1. btndef 载"按钮图"，每次 btndef 命令体开头无条件 deleteButtonLink()(3539-3570:3565)——**旧按钮在下次 btndef 时被清**；bclear/btndef clear 只删链不载图(3541-3545)。spbtn (604-638)：校验 sprite 有图且 cells≥1(cellcheck 需≥2)，insert SPRITE_BUTTON(sprite_no,no)，**spbtn 自身不清旧链**。
2. btnwait 系列 (3355-3510)：进入即对链上每个 SPRITE_BUTTON 显像:有 exbtn_ctl[0] 则 decode 之，否则 visible=true+setCell(0)(3391-3404)；exbtn_d 的 ctl 也在 wait 前 decode(3417-3420)。事件模式=WAIT_BUTTON_MODE(3441)，textbtnwait 再加 WAIT_INPUT/WAIT_VOICE，并统一 |=WAIT_TIMER_MODE(3475)。**btnwait(非2) 点中(返回 button≥1) → 立即 deleteButtonLink()(3496-3498)**；未点中/btnwait2/textbtnwait/bexec 不删链，可跨轮复用。返回前 show_flag 全清(3503-3507)。**点击只删按钮对象，精灵画面保持原位且仍可见**——后续隐藏是脚本义务。
3. 悬停/按下表现全在精灵 cell 上：移入=cell1 或 exbtn_ctl[1]，移出=cell0/ctl[0]，bexec 点中 flash cell2/ctl[2] (mouseOverCheck:966-1022; evt:424-437,855-865)。无独立 down→up 状态机：主循环忽略 BUTTONDOWN(除非 btndown_flag)，只在鼠标 **UP** 时按"当时光标下按钮"结算 (evt:1263-1281, mousePressEvent 415-437)。
4. exbtn/exbtn_d/bdef (cmd:2819-2871) 每命令只读**一个** ctl 串存 exbtn_ctl[1]；exbtn_d 每次先释放旧数组(2827-2833)。decodeExbtnControl(1377-1435)：c/C=对区间 sprite 调 refreshSprite(active=false)即**隐藏**；p/P=refreshSprite(active=true)+cell>=0 时 setCell 即**显示并切 cell**；m/M=移动并置 visible=true；s/S=播放声音。refreshSprite(img:818-834) 内部按"目标可见性/cell/与检矩形的裁剪"变化才 setCell+visible+dirty。即 show/hide 由 p/c 字母改**精灵的 visible+cell**实现，按钮对象不变；exbtn_d 全局串在 btnwait 起始与"移出任意按钮"时补执行(3417-20, 989-991)。无独立 "exbtnctl" 命令名(LUT 仅 exbtn/exbtn_d/bdef/cellcheckexbtn, ONScripter_lut.cpp:297-379)。
5. select (777-940)：入口 enterTextDisplayMode，选项做成内建 anim 的文字按钮(877-889)；WAIT_TEXT_MODE|WAIT_BUTTON_MODE|WAIT_TIMER_MODE 循环(903-908)；选中后 deleteButtonLink→跳转→deleteSelectLink→newPage(914,935,937)。select 不碰精灵状态。
6. print 正文等待(text 点击翻页)仅 WAIT_TEXT_MODE|WAIT_INPUT|WAIT_TIMER(txt:494-507)，无 WAIT_BUTTON_MODE → mouseOverCheck 不运行，**clickwait 期间按钮不可点**(返回 button=0)。所以"残留按钮"平时只是画面。

## C. 右键菜单/系统调用
- enterSystemCall (rm:87-101)：**不备份/隐藏精灵**；只把 root_button_link/root_select_link 摘链存 shelter_button_link(89-92)、存 event_mode/display_mode/鼠标/光标旗标(93-100)，并把当前 text_info 内容拷到 backup_surface(executeSystemCall rm:135)。
- 菜单项按钮=TMP_SPRITE_BUTTON，anim 内建于按钮(getSelectableSentence:1334-1375; 对话框按钮 buildDialog rm:799-814)，绘制恒在最上层(img:810-815)，**不占 sprite 层**；每次菜单前 deleteButtonLink(rm:215,360,396,457,532,550)。
- 主画面精灵全程未被改动，退出 leaveSystemCall(rm:103-131) restore_flag 时恢复 shelter 链与 text_info(110-113)，全屏 dirty+flush(123-124)+refreshMouseOverButton(128)。lookback 进/出会隐/现 lookback_sp 两枚箭头精灵(584-586,620-622,671-675)；SYSTEM_WINDOWERASE 只临时隐 windowchip(276-291)。菜单间切换靠 system_menu_mode(rm:229-237)，SYSTEM_NULL=0 退出(ScriptParser.h:213-224)。选中跳转/读档分支见 D。

## D. 读档
- loadSaveFile2 (f2:27-454) 按档整体重建画面：bg_info.remove()+createBackground(101-103)、sprite_info 每个先 remove 再按档装 image_name/pos/visible/current_cell/trans(134-151)、sprite2 同(365-389)、btndef(325-334)、文本页与脚本行(412-447)；收尾 display_mode=shelter_display_mode=DISPLAY_MODE_TEXT、clickstr_state=CLICK_NONE(449-451)。档内**不含按钮/select/事件态**，故 loadSaveFile2 **不调** deleteButtonLink/deleteSelectLink。
- 清理由调用方做：loadgameCommand 读档返回 0(成功)后执行 deleteButtonLink()+deleteSelectLink()、text_on_flag=false、翻页状态清零、flush(1865-1904:1871-1888)，随后若有 loadgosub_label(define 内 loadgosub 指定, ScriptParser_command.cpp:798-803)则 gosubReal 跳去 r_load 类子程序，return 后回到档内脚本行(1897-1898; f2:436-447 已 setCurrent)。rmenu 读档同套路(rm:360,365-390)。**失败路径**(loadSaveFile 返 -1：文件缺失/版本过新, ONScripter_file.cpp:208-243)：不执行任何清理，旧画面/旧按钮保持，脚本从 loadgame 下一行继续(属预期"没读成")。resetCommand/resetSub(reset()/definereset) 是全清点：deleteButtonLink/deleteSelectLink + sprite_info/sprite2/bg/tachi 全部 reset(752-812;1113-1132)。

## E. 重绘模型
- 每处画面变化把影响矩形写 dirty_rect(add/fill, DirtyRect.h:41-43)；flush(mode) 只在 dirty 包围盒上合成一次并默认清空脏区(839-852)；flushDirect 在 accumulation_surface 按 refreshMode 重画该区后整纹上传 Present(854-904)。refreshMode:文本模式→刷新 shadow/text 全套，否则 NORMAL(1516-1521)。窗口进出文本显示各走一次 window_effect 转场(txt:455-485)。
- waitEvent/runEventLoop (evt:295-308,1155-1429) = SDL_WaitEvent 阻塞，**等待期不持续全屏重绘**；仅当 event_mode 带 WAIT_TIMER_MODE 时 timerEvent→proceedAnimation 逐帧 flushDirect 动画精灵/光标(evt:1077-1120; anim:76-137)；按钮悬停由 mouseMoveEvent→mouseOverCheck 即时 flush(evt:354-370; :1027)。
- 转场 eff:48-107(setEffect 快照 effect_src/生成 effect_dst)与 eff:109-479(doEffect)：每帧 waitEvent(0) 且 WAIT_INPUT_MODE(452-453)，点按/effect_skip 可提前结束(457-459)；帧间 flush(REFRESH_NONE_MODE,NULL,false) 保留脏区、直接呈现已直绘进 accumulation 的帧(462)；结束帧把 effect_dst 拷回 accumulation 再 flush(467-469)，effect_blank 收尾(473-475)。帧内容=当时精灵/文字状态合成，转场本身不清任何精灵。

## F. 引擎自动 vs 脚本义务
- **引擎自动**：(1) 按钮链结点删除时机=下次 btndef(3565)/btnwait 点中 del(3497)/select 选中(914)/读档成功(1879)/系统菜单各阶段(rm 多处)/reset(784)/csp n 对单号 removeSprite(3163,3173)；(2) exbtn_d ctl 内存随 deleteButtonLink(1188-1193) 或下一条 exbtn_d(2827-2833) 释放；(3) 精灵表面/状态清空仅发生在 reset/resetSub、definereset、读档按档重建、csp(-1 或单号)；csp/remove 会连带删对应 SPRITE_BUTTON 结点；(4) vsp/texton/textoff/windowerase 只切 visible。
- **脚本义务**：场景切换(goto/gosub/顺序执行、executeLabel:1087-1103 无清理；gotoCommand ScriptParser_command.cpp:1038-1043)不自动清精灵与按钮；bg 只清立绘+背景不碰 sprite_info(3764-3795)；btnwait 点击后精灵留在原画面；textclear/newpage/br/print/clickwait 与按钮、精灵清理无关(289-293,1320-1332)。CLANNAD 类游戏依赖 saveon 自动存档点(storeSaveFile, 见 savepoint/saveoff cmd:1016-1040 与 enterTextDisplayMode txt:455-460)与 loadgosub→r_load 内显式 csp -1/lsp 重建画面——仓库无游戏脚本可逐行引用，但引擎在 load 后只给"档内状态+空按钮链"，重建职责明确在脚本。回标题/新游戏走 resetCommand 才全清(1113-1132)；单纯跳 *start 不清。
