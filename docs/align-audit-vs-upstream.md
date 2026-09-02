# ONS VM(我方) vs 上游 Onscripter 图层对齐审计（行号均指上游 Onscripter 源码，除非注明“我方”）

## 结论先行（三焦点）
1) 按钮层位：上游按钮=ButtonLink 链，在整帧**最后**画（ONScripter_image.cpp:810-815），恒高于窗背/文字/bar/dialog，与编号无关，且平时 show_flag=0 **不显示**、仅 btnwait 等待/悬停期显示（ONScripter_command.cpp:3391-3415,3503-3507；ONScripter.cpp:966-1020）。我方把**编号≥300 的普通精灵层**硬当按钮恒置最上（ons_screen.cpp:424-437,451-452），与"只按 z_order 一分为二"的上游不符：游戏凡用 300+ 号做普通背景/前景演出精灵（CLANNAD 大量 300+ 精灵即此类，非 ButtonLink）会被误顶到文字/窗背之上。
2) 窗背在"文本句→例程→bg"的真实状态：显示中窗背(sentence_font_info/shadowTextDisplay)与字(text_info)每帧重合成（752-755 或 793-798；窗背实现 ONScripter.cpp:1258-1318）；**翻页只清字不清窗背**（newPage→clearCurrentPage 1254，text_info.fill(0)，窗背不动）；erasetextwindow 1 仅置 erase_text_window_mode=1（2873-2879；默认即 1，ONScripter.cpp:762），不立即擦；真正擦窗只发生在 leaveTextDisplayMode（非 force 需 erase≠0；force 如 textoff 266），由 bg/vsp/tal/btnwait/print/lsp/msp/csp/ld/amsp 等调用（3766,141,318,3362,3366,1248,1701,1339,3133,1908,3890），并以 window_effect 转场擦除（482-483）。我方在"页断点击/texec/br/例程返回"处一律 ClearText 整层=窗背+字一起消失（ons_vm.cpp:1451-52,3425-29,3442,1820；DrawTextPage 再整层重画窗背 4038-46），窗背状态与上游不一致，且缺 window_effect。
3) 转场：上游 effect_src=旧**整帧**快照、effect_dst=新状态**整帧**重合成（含精灵/窗背/按钮），逐帧 alpha 混合，特效中画面=两层快照（ONScripter_effect.cpp:38-58,247-250,452,467-469；alphaBlend src1=src src2=dst image.cpp:445-452）。我方快照只 AssignImages 主层=纯 bg（ons_screen.cpp:909），精灵/文本/按钮在快照层之上**直接露出**、不参与淡出，且非 1 号特效一律并成 crossfade。

## 1 绘制顺序（逐项）
[我方] bg(主层)→精灵>z 降序→文本层(窗背+字)→精灵≤z 降序→≥300 恒最上（ons_screen.cpp:414-456）
[上游] bg(720)→>z 精灵降序(727-731)→tachi human_order[2..0](733-738)→{窗背分支: neg/mono(741-743),sprite2 降序(745-750),窗背+字(752-755)}→0..z 精灵降序(758-767)→{非窗背: sprite2(770-775),neg/mono(777-779)}→bar(783-786)→prnum(787-790)→{非窗背:窗背+字(793-798)}→光标(800-805)→dialog(807)→按钮链(810-815)
[差异/风险] ① 我方文本层永远夹在 ≤z 精灵之下，等价上游 windowback_flag=**恒真**；上游默认 false（ScriptParser.cpp:135），窗背+字应画在**全部精灵之上**——凡 windowback=false 游戏的立绘/前景精灵压窗背场景，我方恰好画反。② 立绘=我方 TACHI_BASE 1000+ 归入 bg 组降序排（ons_vm.cpp:323），被 200..999 号精灵盖住，上游 tachi 永远在 >z 精灵之上。③ sprite2/bar/prnum/cursor/dialog 我方未建模（lsp2 并入 sprite1；bar/prnum 空操作 ons_vm.cpp:3531-46）。
[建议] ①引入 windowback_flag 切换文本层 order（在 fg 组上/下），默认放 fg 之上（对齐默认关窗背）；tachi 固定"bg 组顶端"。②普通精灵只按 z_order 分两组；真正的"恒最上"交给 VM 维护的按钮/光标/dialog 顶层组。③补齐 sprite2 组（按 745-750/770-775 位置插序）与 bar/prnum 精灵槽。

## 2 窗背/文本模式生命周期
[我方] EnterTextDisplayMode 只置标志(ons_vm.cpp:3639-47)；LeaveTextDisplayMode(force||erase≠0) 置假+ClearText 整层(3649-56)；页断/texec/br 清整层(1452,3428,3442)；每次 DrawTextPage 先 SetTextLayerWindowImage 重画窗背(4038-46)
[上游] enter: 首次进入才置 TEXT 并跑 window_effect 开窗(455-471)；leave: force||erase≠0 才置 NORMAL 并跑 window_effect 关窗(474-485)；翻页 newPage 只清字(1227-56,1320-32)；texec 仅对 CLICK_NEWPAGE 调 newPage、CLICK_WAIT|EOL 走 processEOT(295-307)；textclear=newPage(289-93)；br=enter+换行(3641-47)；窗口显示与否由 display_mode+refresh_shadow_text_mode(761,1516-22) 决定，窗背图(sentence_font_info)与文字**分开保存**
[差异/风险] 我方"窗背画进文本层、清=整层一起没"：a) 翻页/页断瞬间窗背消失再重画，与上游"窗背全程在、只字清"不等价→例程往返(bg 前 erasetextwindow 1)与转场交界出现"对话框残余/闪灭"；b) 纯色窗背(#)支路没实现，退化成清层(4040-41)——上游 is_transparent 走整窗区乘色(shadowTextDisplay 1260-1314)。
[建议] 文本层只承载字模；窗背独立层(sentence_font_info 层，setwindow 载入一次)由 leaveTextDisplayMode 显隐；页断只清字；DrawTextPage 不再整层重画窗背。同时实现 windoweffect(存值已有)进出转场。

## 3 bgCommand
[我方] Leave(false)→清 tachi(ons_vm.cpp:2363-67)→载新 bg→可选 StartBgTransition(2377)
[上游] leaveTextDisplayMode(3766)→删 3 tachi(3781-82)→删旧 bg/建新(3784-87)→parseEffect(true)/setEffect/doEffect(3790-92)
[差异/风险] 清除范围对齐（不清 sprite1/sprite2/按钮链）；但①我方 leave 依 erase 清整文本层而上游同条件确实擦窗背+字，等价；②特效内容语义错（见 4）→"转场中旧对话框残余露出"实为：新文本层(窗背)恰在快照层之上被直接显示/或快照只盖 bg 让下帧字先现；③windoweffect 未在 bg 的 leave 里触发。
[建议] 特效前先全帧快照（含文本层/精灵），新画面整体在快照下。

## 4 转场 doEffect
[我方] 快照=AssignImages 主层纯 bg，盖在上层淡出 255→0（ons_screen.cpp:898-924,802-822）；编号>1 全当 crossfade；阻塞 WM_EFFECT(2394)
[上游] 见结论 3；crossfade=effect_src→effect_dst 常量 alpha=256·t/T 混合(247-250)；幕布/百叶(2-9,11-18)逐片把 dst 贴进 accumulation，每帧 waitEvent 可中断(452-459)；结束把 dst 整帧拷回(467-469)
[差异/风险] 特效期间上游画面=纯快照合成，精灵/文字/按钮**冻结参与**；我方=新 bg+快照层+**所有子层实时跳动**，且旧帧缺失精灵/窗背→淡出"底"不对；无中断（effect_skip_flag 点击跳过）。
[建议] 快照改"主层+可见子层合成拷贝"或复用它 Present 像素管线整帧（保真且支持全部特效号）。

## 5 系统菜单/rmenu
[我方] 独立全屏菜单浮层 GetMenuLayer 懒建、DrawRMenu 文本居中(ons_vm.cpp:3729-3830)；不隐藏/备份文本与精灵；右键在文本等待/btnwait 时(1425-38,1484-97)；Close 仅隐藏浮层
[上游] enterSystemCall 摘按钮/选择链、强置 display_mode=TEXT、停光标(rmenu.cpp:87-101)；executeSystemCall 先备份 text_info→backup_surface(135)；executeSystemMenu 把菜单字写进 text_info(187)、菜单项=ButtonLink(TMP_SPRITE)插链恒最上(200-202,210-15)；主画面精灵/bg **不隐藏不备份**；离开恢复 display_mode+把备份拷回 text_info+重挂链+全屏 flush(103-131)；automode 只置标志(246-52)
[差异/风险] 层语义基本等价（浮层≈替换文本区+顶层按钮），但浮层不参与 RelayoutSprites 的 order 管理、为懒建最后插入→在后续新建精灵层/重排后可能被压在下面或残留；离开不强制全屏刷新，菜单层隐藏后脏区不足会留影（用户所见 rmenu 残留主因之一）。
[建议] 菜单/特效层固定最顶 order 并每次开/关 ForceFullRefresh；关闭后清空浮层内容；automode 停止沿用上游"吞一次输入"。

## 6 引擎自动 vs 脚本义务清理点
- 引擎自动：①翻页清字(clickNewPage 621→newPage 1320；字清窗背留)；②bg：清 tachi+旧 bg(3781-87)；③带 leave 的命令在 force||erase≠0 时擦窗背+字(476-483 及各调用点)；④enterTextDisplayMode 首开窗跑 window_effect(467)。
- 其余全部脚本义务（命令即清理）：csp(-1)/csp2 删精灵（我方已做 2471-76）；allsphide/allsp2hide 仅隐藏(3972/3941)；barclear(3804)/prnumclear(1187)；btndef 尾 deleteButtonLink/btnwait del(3496-98)；select/csel 结束删链；dialog/光标按命令开关。
[我方对照] bg 不清 sprite——对齐；页断清窗背=超清（见 2）；无任何精灵级自动清理——对齐上游"无"。

## 重构建议（按优先级）
P1 RelayoutSprites 改两段(z 分界)+顶层"UI 组"，删 BTN_BASE=300 硬分区；窗口back 独立层+windowback_flag 控制其夹层；页断只清字。P2 转场改全帧合成快照（复用 Present/CompositeSnapshot 能力整帧化），支持 2-9/10/11-14 与点击中断。P3 rmenu 浮层固定顶序+进出全屏刷新；P4 补 sprite2/bar/prnum/cursor 槽位并按上游 745-750/770-775/783-790/800-805 插序；tachi 固定 bg 顶端。P5 textgosub 往返状态机对齐 texec/erasetextwindow 的"清字不清窗"语义，避免例程往返+bg 交界的闪灭残留。
