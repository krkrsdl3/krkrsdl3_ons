/* -*- C++ -*-
 *  onscripter/onscripter.cpp
 *
 *  ONScripter 插件模块（ncbind, NCB_MODULE_NAME = "onscripter.dll"）。
 *
 *  职责（指令转发架构，见 docs/06）：
 *   1. 模块注册时：注册 ONS 封包格式（NSA/NS2/SAR）与图像格式（PNG/JPEG/WEBP；
 *      BMP/TLG 由 krkrsdl3 core 自带 handler 提供，无需注册）；
 *   2. TJS 类 ONSEngine —— ONS 运行宿主（demo/startup.tjs 的 ONSLayer 持有）：
 *        - 构造：OnsScreen::Init 接收 TJS 侧传入的窗口与基础层（new ONSLayer(wnd,null)
 *          即窗口 primary layer），不自行创建窗口；
 *        - 配置：setArchivePath / setFontFile / setEncoding / setSaveDir
 *        - start()：主线程解析脚本 + SetSize 定尺寸（先尺寸后 hasImage，见 docs/04），
 *          启动 TVPTimer tick 驱动 VM（无执行线程，全部主线程）；
 *        - stop()：先停 tick 再销毁 Layer 引用；
 *        - 事件转发：onMouseDown/onMouseUp/onKeyDown/onKeyUp 由 TJS 层事件调用，
 *          转换键码/键值后喂入 ons_events 队列（VM 等待点消费）。
 */

#include "tjsCommHead.h"
#include "ncbind/ncbind.hpp"
#include "TVPGraphicsLoader.h"
#include "TVPStorage.h"
#include "TVPTimer.h"
#include "tjsNativeLayer.h"
#include "ons_screen.h"
#include "ons_events.h"
#include "ons_vm.h"
#include "ons_audio.h"
#include "ons_movie.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

#define NCB_MODULE_NAME TJS_N("onscripter.dll")

extern tTVPRegisterGraphicInfo _jpegGraphicInfo;
extern tTVPRegisterGraphicInfo _pngGraphicInfo;
extern tTVPRegisterGraphicInfo _webpGraphicInfo;
extern void OnsRegisterArchiveFormats();
extern void OnsUnregisterArchiveFormats();

// ===========================================================================
// ONSScreen
// ===========================================================================

class ONSEngine
{
    std::string m_archivePath;
    std::string m_fontFile;
    std::string m_saveDir;
    std::string m_scriptFile;
    int m_overrideW = 0, m_overrideH = 0; // setWindowSize 覆盖默认 640x480
    int m_encoding = 0; // 0=gbk 1=sjis 2=utf8

    OnsScreen m_screen; // Layer 生命周期（主线程，docs/04）
    OnsVM m_vm;         // 指令转发 VM（主线程，docs/06）
    OnsAudio m_audio;
    OnsMovie m_movie;   // 音频（M3：bgm/se/dwave -> krkrsdl3 声音栈）
    TVPTimer m_timer;
    std::atomic<bool> m_started{false};

public:
    ONSEngine(iTJSDispatch2* win, iTJSDispatch2* parentLayer)
    {
        m_screen.Init(win, parentLayer);
        m_vm.SetAudio(&m_audio);
        m_movie.SetWindow(win);
        m_vm.SetMovie(&m_movie);
        m_timer.SetOnTimerHandler(this, &ONSEngine::frameStep);
        m_timer.SetInterval(16);
    }

    ~ONSEngine() { stop(); }

    // ---- 配置 ----

    void setArchivePath(ttstr path)
    {
        m_archivePath = path.AsStdString();
        if (!m_archivePath.empty() && m_archivePath.back() != '/' && m_archivePath.back() != '\\')
            m_archivePath += '/';
    }

    void setFontFile(ttstr path) { m_fontFile = path.AsStdString(); }

    void setSaveDir(ttstr path)
    {
        m_saveDir = path.AsStdString();
        if (!m_saveDir.empty() && m_saveDir.back() != '/' && m_saveDir.back() != '\\')
            m_saveDir += '/';
    }

    void setEncoding(ttstr enc)
    {
        if (enc == TJS_N("sjis") || enc == TJS_N("shift_jis"))
            m_encoding = 1;
        else if (enc == TJS_N("utf8") || enc == TJS_N("utf-8"))
            m_encoding = 2;
        else
            m_encoding = 0; // gbk
    }

    void setScriptFile(ttstr path) { m_scriptFile = path.AsStdString(); }

    // 覆盖默认分辨率（NScr 游戏无 $mode 时在 startup.tjs 配置，如 ever17 800x600）
    void setWindowSize(tjs_int w, tjs_int h)
    {
        m_overrideW = (int)w;
        m_overrideH = (int)h;
    }

    // ---- 启停 ----

    void start()
    {
        if (m_started)
            return;
        m_started = true;

        // 默认存档目录 = krkrsdl3 的 savedata 路径（相对游戏目录 argv[1]，
        // 不依赖启动时 cwd —— 否则从别的目录启动会找不到存档、重放 30 秒片头）
        if (m_saveDir.empty())
        {
            m_saveDir = TVPNativeDataPath.AsStdString();
            if (m_saveDir.empty())
                m_saveDir = "savedata/";
            std::error_code ec;
            std::filesystem::create_directories(m_saveDir, ec);
        }
        m_vm.SetSaveDir(m_saveDir);

        // 1) 解析脚本（主线程，纯逻辑）：解密 + 编码转换 + 标签表 + 首屏分辨率
        m_vm.SetFontFile(m_fontFile);
        if (!m_vm.Load(m_scriptFile, m_encoding))
        {
            m_started = false;
            TVPThrowExceptionMessage(TJS_N("ONSScreen: script load failed"));
        }

        m_screen.SetFontFile(m_fontFile);

        // 2) 创建 ONS 屏幕（主线程）：TVPWindow + 主 Layer（背景层）+ 文本层
        int scrW = m_overrideW > 0 ? m_overrideW : m_vm.GetScreenWidth();
        int scrH = m_overrideH > 0 ? m_overrideH : m_vm.GetScreenHeight();
        if (m_overrideW > 0)
            m_vm.OverrideScreenSize(m_overrideW, m_overrideH);
        if (!m_screen.SetSize(scrW, scrH))
        {
            m_started = false;
            TVPThrowExceptionMessage(TJS_N("ONSScreen: failed to create screen layer"));
        }
        int W = m_screen.GetWidth(), H = m_screen.GetHeight();
        if (!m_screen.CreateTextLayer(20, H - 160, W - 40, 140))
        {
            m_started = false;
            TVPThrowExceptionMessage(TJS_N("ONSScreen: failed to create text layer"));
        }
        // 文本层字体（游戏自带字体，经 krkrsdl3 存储/字体栈加载）
        if (!m_fontFile.empty())
        {
            tTJSNI_BaseLayer* tl = m_screen.GetTextLayer();
            tl->SetFontFace(ttstr(m_fontFile.c_str()));
            tl->SetFontFaceIsFileName(true);
            tl->SetFontHeight(24);
        }

        m_vm.SetScreen(&m_screen);

        // DEFINE 段 setwindow 的窗口参数（文本层位置/字号）落地；
        // 窗背由 DrawTextPage 画到文本层（简单模型）
        m_vm.ApplyTextWindow();

        // 3) gloval.sav（启动即读：%911 等全局变量在首帧执行前就位；
        //    无 gloval.sav = 全新开局，脚本从 *start 走 30s 注意事项）
        m_vm.LoadGlovalState();

        // 4) 输入桥复位 + 启动主循环 tick（VM 由 tick 驱动）
        ons_events::Reset();
        m_timer.SetEnabled(true);
    }

    void frameStep()
    {
        m_audio.Poll(); // 音频解码补缓冲（主线程）
        m_screen.Tick();
        if (m_vm.IsLoaded() && !m_vm.IsFinished())
            m_vm.Step();
    }

    void stop()
    {
        m_timer.SetEnabled(false);
        if (m_started)
        {
            m_vm.FlushGlovalState(true); // gloval.sav 随时保存：停止时强制落盘
            m_screen.Destroy(); // 先停 VM（tick 已停）再销毁 Layer（docs/04）
            m_started = false;
        }
    }

    // ---- 事件转发（TJS Layer 事件桥，见 demo/startup.tjs 的 ONSLayer）----
    // krkrsdl3 鼠标键：0=左 1=右 2=中；ONS 键码：1=左 2=中 3=右（SDL1.2 风格）
    static tjs_int MapTjsButton(tjs_int b)
    {
        return b == 1 ? 3 : (b == 2 ? 2 : 1);
    }

    void onMouseDown(tjs_int x, tjs_int y, tjs_int buttons, tjs_uint32 flags)
    {
        //fprintf(stderr, "[ONS] -- mousedown (%d,%d) btn=%d\n", (int)x, (int)y, (int)buttons);
        ons_events::FeedMouseButton(1, MapTjsButton(buttons), x, y);
        m_screen.GetPrimaryLayer()->Update();
    }

    void onMouseUp(tjs_int x, tjs_int y, tjs_int buttons, tjs_uint32 flags)
    {
        ons_events::FeedMouseButton(0, MapTjsButton(buttons), x, y);
        m_screen.GetPrimaryLayer()->Update();
    }

    void onMouseMove(tjs_int x, tjs_int y, tjs_uint32 flags)
    {
        //fprintf(stderr, "[ONS] -- mousemove (%d,%d)\n", (int)x, (int)y);
        ons_events::FeedMouseMotion((int)x, (int)y);
    }

    void onKeyDown(tjs_uint key, tjs_uint32 shift)
    {
        ons_events::FeedKey(1, ONS_MapTJSKeyToOns((int)key));
    }

    void onKeyUp(tjs_uint key, tjs_uint32 shift)
    {
        ons_events::FeedKey(0, ONS_MapTJSKeyToOns((int)key));
    }

    // ---- 状态 ----

    // 存档/读档（槽位 n；测试与调试用，也可作游戏内简易存档）
    void saveState(tjs_int slot) { m_vm.SaveState((int)slot); }
    bool loadState(tjs_int slot) { return m_vm.LoadState((int)slot); }

    // 脚本执行追踪开关（打印每行执行与等待点，见 [ONS] 前缀日志）
    void setTrace(bool on) { m_vm.SetTrace(on); }

    // 调试：打印解码后脚本行 [from, from+count)
    void dumpLines(tjs_int from, tjs_int count) { m_vm.DumpLines((int)from, (int)count); }

    // 调试：把解码后的完整脚本写到文件（离线检索用）
    void dumpScript(ttstr path) { m_vm.DumpScript(path.AsStdString()); }

    // 调试：直接走截图链路（全屏快照 -> 缩放 -> save_dir/文件），
    // 验证保存菜单缩略图路径（savescreenshot 命令同路）
    void debugShot(tjs_int w, tjs_int h, ttstr file)
    {
        m_vm.TakeScreenshot((int)w, (int)h, file.AsStdString());
    }

    // 调试：把引擎图层（bg/文本/菜单浮层）以 BMP 转储到 dir（渲染验证用；
    // 复用 core 的 tTJSNI_BaseLayer::SaveLayerImage，即 TJS Layer.saveLayerImage 同一路径）
    void dumpLayers(ttstr dir)
    {
        ttstr prefix = dir;
        if (!prefix.IsEmpty() && prefix.GetLastChar() != TJS_N('/') &&
            prefix.GetLastChar() != TJS_N('\\'))
            prefix += TJS_N('/');
        fprintf(stderr, "[ONS] dumpLayers @%u ticks %s\n", (unsigned)OnsGetTicks(),
                prefix.AsStdString().c_str());
        auto dump = [&](const ttstr& name, tTJSNI_BaseLayer* l) {
            try
            {
                if (l && l->GetMainImage())
                {
                    l->SaveLayerImage(prefix + name, TJS_N("bmp"));
                    fprintf(stderr, "[ONS] dumpLayers -> %s%s\n", prefix.AsStdString().c_str(),
                            name.AsStdString().c_str());
                }
                else
                    fprintf(stderr, "[ONS] dumpLayers: %s has no image\n",
                            name.AsStdString().c_str());
            }
            catch (eTJSScriptException& e)
            {
                fprintf(stderr, "[ONS] dumpLayers %s: script error: %s\n",
                        name.AsStdString().c_str(), e.GetMessage().AsStdString().c_str());
            }
            catch (eTJS& e)
            {
                fprintf(stderr, "[ONS] dumpLayers %s: TJS error: %s\n",
                        name.AsStdString().c_str(), e.GetMessage().AsStdString().c_str());
            }
            catch (...)
            {
                fprintf(stderr, "[ONS] dumpLayers %s: unknown error\n",
                        name.AsStdString().c_str());
            }
        };
        dump(TJS_N("dbg_bg.bmp"), m_screen.GetPrimaryLayer());
        dump(TJS_N("dbg_text.bmp"), m_screen.GetTextLayer());
        dump(TJS_N("dbg_menu.bmp"), m_screen.GetMenuLayer());
        // 转场快照层（crossfade 期间为旧画面；仅活跃时存在内容）
        dump(TJS_N("dbg_fx.bmp"), m_screen.GetEffectLayer());
        // 精灵层（只转储已载图的层；限制数量避免拖慢验证节奏）
        for (int n = 0; n <= 14; n++)
        {
            tTJSNI_BaseLayer* sp = m_screen.GetSpriteLayer(n);
            if (sp && sp->GetMainImage())
                dump(ttstr(TJS_N("dbg_sp")) + ttstr(n) + ttstr(TJS_N(".bmp")), sp);
        }
    }

    bool isRunning() const { return m_started && !m_vm.IsFinished(); }
    tjs_int getWidth() const { return m_screen.GetWidth(); }
    tjs_int getHeight() const { return m_screen.GetHeight(); }
};

NCB_REGISTER_CLASS(ONSEngine)
{
    NCB_CONSTRUCTOR((iTJSDispatch2*, iTJSDispatch2*));
    NCB_METHOD(setArchivePath);
    NCB_METHOD(setFontFile);
    NCB_METHOD(setSaveDir);
    NCB_METHOD(setEncoding);
    NCB_METHOD(setScriptFile);
    NCB_METHOD(setWindowSize);
    NCB_METHOD(start);
    NCB_METHOD(frameStep);
    NCB_METHOD(stop);
    NCB_METHOD(onMouseDown);
    NCB_METHOD(onMouseUp);
    NCB_METHOD(onMouseMove);
    NCB_METHOD(onKeyDown);
    NCB_METHOD(onKeyUp);
    NCB_METHOD(saveState);
    NCB_METHOD(loadState);
    NCB_METHOD(setTrace);
    NCB_METHOD(dumpLines);
    NCB_METHOD(dumpScript);
    NCB_METHOD(dumpLayers);
    NCB_METHOD(debugShot);
    NCB_PROPERTY_RO(isRunning, isRunning);
    NCB_PROPERTY_RO(width, getWidth);
    NCB_PROPERTY_RO(height, getHeight);
}

// ===========================================================================
// 模块初始化 / 反初始化
// ===========================================================================

static void onscripter_init()
{
    // ONS 封包格式（>arc.nsa>... 路径可用）
    OnsRegisterArchiveFormats();

    // 图像格式：PNG/JPEG/WEBP（krkrsdl3 插件 handler；BMP/TLG 由 core 自带）
    TVPRegisterGraphicLoadingHandler(&_jpegGraphicInfo);
    TVPRegisterGraphicLoadingHandler(&_pngGraphicInfo);
    TVPRegisterGraphicLoadingHandler(&_webpGraphicInfo);
}

static void onscripter_done()
{
    TVPUnregisterGraphicLoadingHandler(&_jpegGraphicInfo);
    TVPUnregisterGraphicLoadingHandler(&_pngGraphicInfo);
    TVPUnregisterGraphicLoadingHandler(&_webpGraphicInfo);
    OnsUnregisterArchiveFormats();
}

NCB_PRE_REGIST_CALLBACK(onscripter_init);
NCB_POST_UNREGIST_CALLBACK(onscripter_done);
