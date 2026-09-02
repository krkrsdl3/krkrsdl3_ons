/* -*- C++ -*-
 *  onscripter/ons_vm.h
 *
 *  ONS 指令转发 VM（docs/06）：
 *   - 脚本读取（经 krkrsdl3 存储）/ 解密 / 编码转换（GBK/SJIS/UTF-8 -> **UTF-8**，
 *     与 krkrsdl3 底层一致 —— 本移植中 tjs_char 为 UTF-8 字节）
 *   - 标签表、变量、表达式、流程控制（jump/gosub/if/mov…）
 *   - 显示/文本指令转发到 krkrsdl3 Layer（OnsScreen 的图层组）
 *   - 全部在主线程执行：Step() 由插件 tick 驱动，推进到等待点（click/wait）后返回
 */

#ifndef ONS_VM_H
#define ONS_VM_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tjsCommHead.h"

struct OnsEvent;
class OnsScreen;
class OnsAudio;
class Coding2UTF16;

class OnsVM
{
public:
    OnsVM();
    ~OnsVM();

    // 主线程：读取并解析脚本（含首屏分辨率预扫描 + DEFINE 段设置）。失败返回 false。
    bool Load(const std::string& scriptName, int encoding);

    // 主线程：推进执行到等待点。返回 false 表示脚本结束。
    bool Step();

    // 屏幕/字体绑定（在 Load 前或后均可，Step 前必须）
    void SetScreen(OnsScreen* screen);
    void SetFontFile(const std::string& f) { m_fontFile = f; }
    void SetAudio(OnsAudio* audio) { m_audio = audio; }
    void SetMovie(class OnsMovie* movie) { m_movie = movie; }
    void SetSaveDir(const std::string& dir) { m_saveDir = dir; }

    // 图片透明模式解析（上游 parseTaggedString）：raw 可为
    // ":a;file" ":l;file" ":r;file" ":c;file" 或无前缀 file。
    // 剥前缀并解析存储名到 storage，返回 OnsScreen::IMG_*（无前缀按
    // transmode 默认；dflt 仅当 transmode 未设置时兜底）。
    int ResolveImageSpec(const std::string& raw, std::string& storage, int dflt);

    // 覆盖默认屏幕分辨率（startup.tjs 配置：无 $mode 的 NScr 游戏，如 ever17 800x600）
    void OverrideScreenSize(int w, int h)
    {
        if (w > 0)
            m_screenW = w;
        if (h > 0)
            m_screenH = h;
    }

    // 存档：槽位 n -> savedata/ons<n>.sav（变量 + pc + 调用栈）
    void SaveState(int slot);
    bool LoadState(int slot);
    void SaveGlovalState();
    bool LoadGlovalState();

    bool IsLoaded() const { return m_loaded; }
    bool IsFinished() const { return m_finished; }
    int GetScreenWidth() const { return m_screenW; }
    int GetScreenHeight() const { return m_screenH; }

    // 调试：错误信息
    const std::string& GetError() const { return m_error; }
    // 脚本执行追踪（TraceLine 打印；TJS setTrace 开关）
    void SetTrace(bool on) { m_trace = on; }
    bool m_trace = false;
    // 调试：打印解码后的脚本行 [from, from+count)
    void DumpLines(int from, int count);
    // 调试：把解码后的完整脚本写到文件（供离线检索 select/mov 等场景）
    bool DumpScript(const std::string& fileName);
    // 把 setwindow 解析出的窗口参数应用到文本层/背景层（screen 就绪时）
    void ApplyTextWindow();

    // 变量访问（表达式求值器使用）
    bool GetVar(const std::string& name, int& value) const;
    void SetVar(const std::string& name, int value);
    bool GetStrVar(const std::string& name, std::string& value) const;
    void SetStrVar(const std::string& name, const std::string& value);

    // 截图链路（savescreenshot 命令与调试钩子共享；public 供 ONSEngine.debugShot）
    void TakeScreenshot(int w, int h, const std::string& file);

    // 调试：F12 全屏合成快照 -> /tmp/ons_dbg_*.bmp（验证渲染，发行前删除）
    void DebugFullShot();

    // ---- gloval.sav（上游 saveGlovalData/loadGlovalData + globalon）----
    // 随时保存：变量变更置脏，FlushGlovalState 去抖落盘；stop 时强制落盘。
    void MarkGlovalDirty() { m_glovalDirty = true; }
    void FlushGlovalState(bool force = false);
    bool IsGlobalon() const { return m_globalon; }

private:
    // ---- 脚本数据（UTF-8 行）----
    std::vector<std::string> m_lines;
    std::map<std::string, int> m_labels;
    int m_pc = 0; // 当前行号
    std::vector<int> m_callStack;
    bool m_loaded = false;
    bool m_finished = false;
    int m_screenW = 640, m_screenH = 480;

    // ---- 变量（数字变量 %n% 与命名变量共用一张表）----
    std::map<std::string, int> m_vars;
    std::map<std::string, std::vector<int>> m_arrays;
    // defsub 子例程名（DEFINE 段；行首名字单独出现 = gosub）
    std::set<std::string> m_defsubs;
    // 子例程调用参数栈（getparam 读调用行参数；每层 gosub push、return pop）
    std::vector<std::vector<std::string>> m_subParams;
    // numalias 数字别名（DEFINE 段；%名 引用 -> %数字）
    std::map<std::string, std::string> m_numAliases;
    // 变量名规范化（%名 -> 数字/别名；统一去 % 前缀，读写一致：
    // 表达式解析器剥 % 读 "911"，命令写 "%911" —— 必须映射到同一键）
    std::string ResolveVarName(const std::string& name) const
    {
        std::string n = name;
        if (!n.empty() && n[0] == '$')
            return n; // 字符串变量不经 numalias
        if (!n.empty() && n[0] == '%')
            n = n.substr(1);
        auto it = m_numAliases.find(n);
        if (it != m_numAliases.end())
            n = it->second;
        return n;
    }
    std::map<std::string, std::string> m_strVars; // 字符串变量（$name，itoa2 等写入）

    // ---- 文本窗口 ----
    int m_textX = 0, m_textY = 0, m_textW = 0, m_textH = 0;
    int m_fontSize = 24, m_lineSpacing = 24;
    int m_textCursorX = 0, m_textCursorY = 0;
    // ---- 文本显示模式（上游 display_mode DISPLAY_MODE_TEXT/NORMAL，
    //      ONScripter_text.cpp enterTextDisplayMode/leaveTextDisplayMode）----
    bool m_textMode = false; // 当前是否处于文本显示模式
    bool m_saveon = true;    // 上游 saveon_flag（初始 true，ONScripter.cpp:771）
    int m_windowEffect = 1;  // window_effect（windoweffect 命令；默认 1=即时）
    int m_windowEffectDur = 0;
    bool m_windowbackFlag = false; // windowback 命令标志
    // 特效注册表（DEFINE 段 effect no,effect[,duration]；M3.2 转场系统使用）
    std::map<int, std::pair<int, int>> m_effects;
    int m_effectBlank = 1;   // effectblank：bg 缺省特效号
    std::string m_transMode; // transmode：默认透明模式（alpha 等）
    void EnterTextDisplayMode();
    void LeaveTextDisplayMode(bool force);
    // 窗口背景（setwindow 第 12 参数：sentence_font_info 语义）
    std::string m_windowImage; // 背景图（空 = 无图）
    int m_winX = 0, m_winY = 0; // 背景 blit 位置
    bool m_windowColorMode = false; // #RRGGBB 纯色窗口模式
    uint32_t m_windowColor = 0xffffff;
    bool m_textShadow = false; // setwindow is_shadow（白字黑阴影）

    // ---- 文本模型（NScr）----
    std::string m_textGosubLabel; // textgosub 目标标签（DEFINE 段设置；行尾点击 -> gosub）
    std::string m_pretextLabel;   // pretextgosub 目标标签（DEFINE；行首 [tag] 文本显示前 gosub）
    std::string m_currentTag;     // 当前行 [tag]（gettag 读取；上游 current_page->tag）
    bool m_pretextConsumed = false; // 本行 tag 已剥、正文待显示（例程 return 后置）
    bool m_textPageFresh = true;    // 下个画页需重载窗背/清旧文字（页首）；
                                    // false = 同页续行（保留已画文字叠加）
    std::string m_pretextBody;    // 剥离 [tag] 后的正文
    int m_pretextReturnLine = -1; // pretext 例程返回后应回到的行
    static constexpr int PRETEXT_RET_MARK = -9999;
    bool m_textClickClear = false; // WM_TEXTCLICK 点击后是否清文本（页断）
    bool m_pageBreakPending = false; // textgosub 页断：例程返回后清文本（newPage 语义）
    int m_eraseTextWindow = 1;   // erasetextwindow 模式（上游 erase_text_window_mode：
                                 // 0=非 force 的 leave 保留窗口，!=0 擦除；默认 1）
    // 裸 @（点击停顿）剩余段：画到 @ 前等待点击，点击后 Step 续画
    std::vector<std::string> m_pendingTextSegs;
    std::vector<bool> m_pendingAtWaits;
    bool m_pendingTextEol = false;
    int m_textLineAdvance = 0; // 当前行已画宽度（@ 接续段用）
    bool FinishTextLine(bool eol); // 文本行尾处理（textgosub / 默认点击等待）
    // 字符串表达式求值（$a+"lit"+$b 拼接）
    std::string EvalStringExpr(const std::string& s);
    std::string GetRawArg(const std::vector<std::string>& args, size_t i);
    // exbtn 悬停：控制串执行 + 悬停更新（上游 decodeExbtnControl / mouseOverCheck）
    void ExecExbtnControl(const std::string& ctl);
    void UpdateHover(int x, int y);

    // ---- 按钮（btn/spbtn/btnwait/textbtnwait）----
    struct OnsButton
    {
        int id, x, y, w, h;
        int spriteNo = -1; // 关联精灵号（spbtn/exbtn；-1=btn 矩形按钮）
        std::string ctl;   // exbtn 控制串（悬停效果：Cxx隐藏/Pxx显示/sN音效）
    };
    std::vector<OnsButton> m_buttons;
    // btn 命令绘制到的精灵层号（BTN_BASE+id）：btnwait 系列返回/btndef 重建时
    // 隐藏（上游 show_flag=0 —— 按钮只在 btnwait 等待期间显示，不常驻）
    std::vector<int> m_btnSpriteIds;
    void HideBtnSprites(); // 隐藏全部 btn 按钮精灵层（不清表）
    void ResetToStart(); // 回标题画面重置（goto *start / reset 命令）
    // lookback 回看（SYSTEM_LOOKBACK）
    void DrawLookback();
    void LookbackPage(int dir);
    void ExitLookback();
    // 逗号粘连语句展开（NScr 词法：命令参数后逗号跟命令名 = 新语句）
    void ExpandCommaStatements(const std::string& stmt, std::vector<std::string>& out);
    std::string m_btnwaitVar; // btnwait 结果变量名（% 已剥）
    bool m_btnwaitDelete = false; // btnwait 结束后清除按钮表
    std::string m_exbtnD; // exbtn_d 控制串（悬停离开恢复全部普通态）
    int m_hoverButton = -1; // 当前悬停按钮 id（-1=无）

    // ---- for/next 循环 ----
    struct LoopFrame
    {
        std::string var;
        int endVal = 0;
        int step = 1;
        int bodyLine = 0; // 循环体起始行（for 的下一行）
    };
    std::vector<LoopFrame> m_loops;

    // ---- rmenu（右键菜单；语义按 Onscripter）----
    struct RMenuAction
    {
        std::string label;
        std::string action;
    };
    std::vector<RMenuAction> m_rmenuDefItems; // rmenu 命令定义（DEFINE 段，主菜单内容）
    std::vector<RMenuAction> m_rmenuItems; // 当前菜单项（主菜单/槽列表共用）
    // 菜单字体/颜色（menusetwindow / menuselectcolor，DEFINE 段）
    int m_menuFontSize = 18;   // 字号
    int m_menuPitch = 22;      // 行距 = 字号 + 增量
    uint32_t m_menuOnColor = 0xffffff;  // 悬停/当前项颜色
    uint32_t m_menuOffColor = 0xb1b1b1; // 普通项颜色
    // 菜单项矩形（居中绘制；命中测试用）
    std::vector<tjs_int> m_rmenuRects; // 每项 [x,y,w,h]
    int m_rmenuHover = -1;     // 当前悬停项（-1=无）
    bool m_rmenuVisible = false;
    // 槽位子菜单（rmenu save/load 对齐上游 executeSystemSave/Load 的选槽步）
    std::vector<RMenuAction> m_rmenuMainItems; // 主菜单项备份（槽列表 ESC 返回）
    bool m_rmenuSlotMode = false;              // 当前处于槽列表
    bool m_rmenuSlotSaving = false;            // 槽列表为保存(否则读取)
    int m_rmenuPrevWait = 0; // rmenu 打开前的等待态（关闭/取消后恢复，WaitMode 值）
    // 菜单文字精灵槽基址（DrawRMenu 用；遮罩 = 菜单层半透明黑）
    static const int RMENU_TEXT_BASE = 800;
    static const int RMENU_TEXT_BASE_MAX = 24; // 主菜单/槽列表最大项数上限
    static const int RMENU_SLOTS = 10;

    // ---- 等待 ----
    enum WaitMode
    {
        WM_NONE,
        WM_CLICK,
        WM_TIMER,
        WM_SELECT,
        WM_TEXTCLICK,
        WM_BTNWAIT,
        WM_RMENU,
        WM_MOVIE,
        WM_EFFECT, // 转场阻塞（上游 while(doEffect) 的 tick 驱动等价）
        WM_ERASE,  // windowerase：文本框已隐藏，等任意点击恢复
        WM_LOOKBACK // 回想回看（systemcall lookback）
    };
    WaitMode m_wait = WM_NONE;
    uint32_t m_waitUntil = 0;
    bool m_waitInput = false; // delay 语义：WM_TIMER 等待可被输入跳过

    // ---- 行内语句暂停-恢复（NScr：if 行内 / ':' 语句链中的 wait 等）----
    // 一条脚本行拆成多条语句（SplitStatements）逐条执行；某条语句遇到等待点
    // （wait/click/btnwait/select/end）时，把剩余语句保存在这里，等待结束后
    // 继续执行，而不是把 pc 推进到下一行（否则 wait 之后的 goto 等会丢失）。
    std::vector<std::string> m_pendingStmts;
    size_t m_pendingIdx = 0; // 下一条待执行语句下标
    // 执行 stmts[startIdx..]：false=遇等待点（剩余语句已存入 m_pendingStmts）
    bool ExecStatementList(const std::vector<std::string>& stmts, size_t startIdx);
    // 等待输入事件（带陈旧过滤）：丢弃早于当前等待开始（m_waitStart）超过
    // STALE_INPUT_MS 的点击/按键 —— 上下文切换（菜单/读档/页面）前堆积的
    // 连点残留不得在新等待中集中消费（防御"点一下跳多句/菜单误触残留"；
    // 上游 SDL_WaitEvent 即时消费无堆积窗口，本 VM 逐 tick 消费会积压）。
    static const uint32_t STALE_INPUT_MS = 80;
    bool PollWaitEvent(OnsEvent* ev);

    // ---- systemcall 系统功能（CLANNAD r_menu；上游 SYSTEM_*/executeSystemCall）----
    // automode：文本等待（textbtnwait/WM_TEXTCLICK）定时自动推进；
    // 任意真实点击/按键先关闭标志并吞掉该次输入（上游 mousePressEvent
    // automode_flag 分支：点一下只停不推进）
    bool m_autoMode = false;
    bool m_skipMode = false;    // skip：文本等待立即通过（select/普通菜单不自动）
    // Ctrl 按住快进（上游 ctrl_pressed_status：按住 = 文本立即显示 + 点击
    // 等待/textbtnwait 自动通过；每按一次同时作为一次点击）
    bool m_ctrlHeld = false;
    int m_autoTime = 1000;      // automode 延迟 ms（上游 DEFAULT_AUTOMODE_TIME）
    uint32_t m_waitStart = 0;   // 当前等待开始时刻（automode/skip 计时）
    // windowerase（隐藏文本框）：文本层隐藏，点一下恢复
    bool m_erasePending = false;
    int m_erasePrevWait = 0; // windowerase 恢复后回到的等待（rmenu 打开前）
    // lookback（回想）：文本历史（每次 DrawTextPage 的整句文本，UTF-8）
    std::vector<std::string> m_textHistory;
    int m_lookbackIdx = 0; // 回看当前页起始句下标（0=最早）
    // 音量（defmp3vol/defsevol/defvoicevol；0-100）
    int m_mp3vol = 100, m_sevol = 100, m_voicevol = 100;
    // 精灵 z 分界（humanz；上游默认 499）：>z 精灵在窗背文字之下
    int m_zOrder = 499;

    // ---- select 选择支 ----
    struct SelectItem
    {
        std::string text;  // 菜单项文本（UTF-8）
        std::string label; // 目标标签（select/goto 模式）
    };
    std::vector<SelectItem> m_selectItems;
    int m_selectCursor = 0;  // 当前高亮项
    int m_selectMode = 0;    // 0=select(goto) 1=selnum(写变量)
    std::string m_selectVar; // selnum 模式的目标变量
    int m_selectMenuY = 0;   // 菜单在文本层内的起始 Y（绘制/点击命中用）

    // ---- 屏幕 ----
    OnsScreen* m_screen = nullptr;
    OnsAudio* m_audio = nullptr; // 音频（M3；bgm/se/dwave）
    class OnsMovie* m_movie = nullptr; // 视频（mov/mpegplay 命令）
    bool m_movieClick = false;         // mov click 参数（点击跳过）
    std::string m_fontFile;
    std::string m_saveDir; // 存档目录（引擎 setSaveDir；缺省 savedata/）
    std::string m_error;
    // gloval.sav 状态（上游 saveGlovalData/loadGlovalData；globalon 时全部变量持久化）
    bool m_globalon = false;    // globalon 命令（DEFINE 段）
    bool m_glovalDirty = false; // 变量变更置脏
    uint32_t m_glovalLastSave = 0;

    // ---- 画面状态（存档 v2 恢复用；上游 saveAll/loadSaveFile 保存 bg/精灵表）----
    std::string m_bgSpec; // 当前 bg 原始参数（"file"/white/black/矩形规格）
    struct SpriteState
    {
        std::string file;    // 图片=已解析存储名；文本精灵=文本内容
        bool textSprite = false;
        int fontSize = 18;
        uint32_t color = 0xffffff;
        int mode = -1;       // 图片透明模式（OnsScreen::IMG_*；-1=按 transmode 推断）
        int x = 0, y = 0;
        int alpha = -1;
        bool visible = false;
    };
    std::map<int, SpriteState> m_spriteState; // 精灵状态表（lsp/lsph/vsp/csp 维护）
    bool m_savedTextMode = false; // 读档时的文本模式（RestoreVisualState 落地）
    void RestoreVisualState(); // 读档后重建画面（bg + 精灵 + 文本窗）
    void RecordSpriteState(int n, const SpriteState& st) { m_spriteState[n] = st; }

    // ---- 截图（getscreenshot/savescreenshot/deletescreenshot；上游语义）----
    int m_shotW = 160, m_shotH = 120; // getscreenshot 目标尺寸
    int m_shotFullW = 0, m_shotFullH = 0; // 快照全幅尺寸
    std::vector<uint32_t> m_shotBuffer; // 快照（BGRA）
    bool WriteShotBMP(const std::string& path, int w, int h,
                      const std::vector<uint32_t>& bgra);

    // ---- 内部 ----
    static const int TACHI_BASE = 1000; // 立绘槽位映射到的精灵层编号（与脚本精灵隔离）
    bool LoadScriptFile(const std::string& name, int encoding);
    bool PreScan();             // 标签表 + 首屏分辨率
    void LoadDefineSection();   // DEFINE 段设置（textgosub 等）+ 入口行（*start）
    bool ExecuteLine(int line); // 执行一行；true=继续，false=停止（等待/结束）
    bool ExecuteStatement(const std::string& stmt); // 单条语句（已按 ':' 拆分）
    bool ExecuteIf(const std::string& args, bool negate); // if/notif（含行内命令）
    bool ExecuteFor(const std::string& args);
    bool ExecuteNext();
    bool ExecuteCommand(const std::string& cmd, const std::vector<std::string>& args);
    bool DrawTextPage(const std::string& text); // 文本行（\ 标签/EOL/页断/textgosub）
    void DrawTextLine(const std::string& text, bool appendToCurrent = false);
    void ClearText();
    void WaitForClick();
    void WaitForTicks(uint32_t ms);
    int ClassifyButton(tjs_int tjsButton, int x, int y);
    void DrawRMenu();               // 绘制右键菜单（居中文本项，菜单浮层）
    void UpdateRMenuHover(int x, int y); // 悬停高亮更新
    void CloseRMenu();              // 隐藏菜单浮层
    int ExecRMenuAction(const std::string& action); // 0完成 1立即继续 2子菜单
    void OpenRMenuSlots(bool saving); // 槽位子菜单（rmenu 储存/读取）
    std::string SlotLabel(int slot, bool withTime); // 槽位行标签
    void DrawSelectMenu();       // 绘制/重绘菜单（高亮当前项）
    void ConfirmSelect();        // 确认当前项：goto 标签 / selnum 写变量
    static bool IsKnownCommand(const std::string& cmd);
    bool EvalExpression(const std::string& expr, int& result); // 失败返回 false
    std::string SubstituteVars(const std::string& text) const;
    // 解析指令尾随特效参数（上游 parseEffect/readEffect）：
    // 无参 -> effect 0（即时）；单个数字 -> 特效注册表查找（m_effects）；
    // effect,duration -> 行内特效。返回 (effectNo, durationMs)。
    std::pair<int, int> ResolveEffect(const std::vector<std::string>& args, size_t idx);
};

#endif // ONS_VM_H
