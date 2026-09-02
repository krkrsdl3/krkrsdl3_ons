/* -*- C++ -*-
 *  onscripter/ons_screen.h
 *
 *  ONS 屏幕 —— krkrsdl3 Layer 生命周期核心（用户关注点，规则见 docs/04）。
 *
 *  指令转发架构（docs/06）下，ONS 画面由 Layer 树承载（窗口与基础层由 TJS 侧创建，
 *  demo/startup.tjs 的 ONSLayer 即 primary layer，经构造传入）：
 *   1. Init(win, baseLayer)（主线程）：接管 TJS 侧传入的窗口/基础层（借用引用，
 *      tTJSVariant(obj,obj) 存活期 pin）；
 *   2. SetSize(w,h)（主线程）：定尺寸 -> hasImage -> 可见（顺序重要），缓存像素缓冲；
 *   3. CreateTextLayer(x,y,w,h)（主线程）：原生创建文本子层（ONS 文本窗口）；
 *   4. Present(pixels,w,h,pitch)：整帧位图拷贝（ARGB→RGBA，必要时最近邻缩放）
 *      + 标脏（用于快照/整帧输出场景；常规绘制走 Layer 原生方法）；
 *   5. Tick()（主线程 TVPTimer）：脏 → LayerManager::RequestInvalidation → 合成上屏；
 *   6. Destroy()（主线程）：先 Invalidate 各 Layer（Part/释放图像），再 Invalidate
 *      Window；清空持有的引用。
 */

#ifndef ONS_SCREEN_H
#define ONS_SCREEN_H

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "tjsCommHead.h"
#include "tjsVariant.h"

class tTJSNI_BaseLayer;
class tTJSNI_Window;

class OnsScreen
{
public:
    OnsScreen() = default;
    ~OnsScreen() { Destroy(); }

    // 传入上下文窗口和基础层
    void Init(iTJSDispatch2* win, iTJSDispatch2* baseLayer);
    // 字体（字符串精灵/文本用；引擎 start 时设置）
    void SetFontFile(const std::string& f) { m_fontFile = f; }

    // 设置显示大小
    bool SetSize(int width, int height);

    // 主线程：创建文本子层（ONS 文本窗口；在 SetSize 之后调用）
    bool CreateTextLayer(int x, int y, int w, int h);

    // 文本框背景（setwindow 窗背图 / NScr sentence_font_info）：画到文本层
    // （LoadImages 同时清掉旧文字 —— 简单模型），位置 (x,y)，图对自动合成
    bool SetTextLayerWindowImage(const std::string& file, int x, int y);

    // 文本框背景层（独立层版本，保留但不再使用；显示问题由同层方案规避）
    bool SetTextWindowImage(const std::string& file, int x, int y);
    bool SetTextWindowColor(tjs_uint32 color, int x, int y);
    void ClearTextWindow();
    // 重新显示窗背层（上游 enterTextDisplayMode：每次进入文本模式重绘窗口背景；
    // 页断擦除后下一段文本重新出现）
    void ShowTextWindow();

    // 主线程：菜单浮层（rmenu 用；懒创建、默认隐藏）
    class tTJSNI_BaseLayer* GetMenuLayer();

    // 主线程：销毁（调用方需保证执行线程已停止）
    void Destroy();

    // 主线程：每帧 tick（TVPTimer 回调）
    void Tick();
    // 强制整屏重绘一次（读档/大画面切换后调用；等价上游
    // dirty_rect.fill + flush —— 不依赖各层增量标脏的完备性）
    void ForceFullRefresh();

    // ---- 转场（上游 setEffect/doEffect 模型：effect_src=旧画面 effect_dst=新画面）----
    // 先快照当前主层画面到转场层（可见），随后由调用方载入新 bg；Tick 按特效
    // 推进（crossfade=快照层淡出）。effectNo<=1 或 durationMs<=0 返回 false（即时，
    // 调用方直接载入新图）。其余编号按上游 doEffect 未实现分支替换为 crossfade
    // （ONScripter_effect.cpp:445 的既有语义）。
    bool StartBgTransition(int effectNo, int durationMs);
    bool IsTransitioning() const { return m_fxActive; }
    // quake/quakex/quakey（上游 quakeCommand：no=强度 1..3，时长 ms）
    bool StartQuake(int type, int no, int durationMs);
    bool IsQuaking() const { return m_quakeActive; }
    // 全屏快照：bg + 可见精灵 + 文本层（近似上游 accumulation_surface 快照；
    // getscreenshot/savescreenshot 用）。输出 BGRA8888（层内像素布局）。
    bool CompositeSnapshot(std::vector<uint32_t>& bgra, int& w, int& h);

    // 执行线程：整帧呈现（像素为 ARGB8888）
    void Present(const void* pixels, int w, int h, int pitch);

    bool IsReady() const { return m_layer != nullptr; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // 图层访问（供 VM 转发指令使用；仅在主线程使用）
    class tTJSNI_BaseLayer* GetPrimaryLayer() const { return m_layer; }
    class tTJSNI_BaseLayer* GetTextLayer() const { return m_textLayer; }
    class tTJSNI_BaseLayer* GetWindowLayer() const { return m_windowLayer; }
    // 转场快照层（调试转储/检查用）
    class tTJSNI_BaseLayer* GetEffectLayer() const { return m_effectLayer; }

    // 精灵层 z 分界（上游 humanzCommand -> z_order，默认 499）：
    // 编号 > z 的精灵 = 背景层（窗背/文字之下）；<= z = 前景层（其上）。
    void SetZOrder(int z) { m_zOrder = z; RelayoutSprites(); }
    int GetZOrder() const { return m_zOrder; }

    // ---- 精灵层（主层子层，懒创建）----
    // 创建/取得精灵层 n（失败返回 false）
    bool EnsureSpriteLayer(int n);
    // 图像透明模式（上游 AnimationInfo trans_mode，parseTaggedString 解析）：
    // 无前缀图按脚本 transmode 默认（CLANNAD DEFINE transmode alpha）。
    //   ALPHA   = 有 alpha 通道（png/webp）用自带 alpha；
    //             无 alpha 格式（jpg/jpeg/bmp）= 图对：左半 RGB + 右半反相
    //             alpha 掩码（alpha=255-右半灰度），合成后真裁半宽
    //             （对齐上游 setupImageAlpha TRANS_ALPHA —— 不启发式，
    //             无尺寸/灰度门槛）
    //   TOPLEFT/TOPRIGHT = 挖左上/右上角像素色为透明（TRANS_TOPLEFT/TOPRIGHT）
    //   COPY   = 原样显示（bg/btndef 图集强制 COPY，上游同）
    enum
    {
        IMG_ALPHA = 0,
        IMG_TOPLEFT = 1,
        IMG_TOPRIGHT = 2,
        IMG_COPY = 3
    };
    // 载入图像并显示（mode 见上；jpg 无自带 alpha 的判定按扩展名）
    bool SetSpriteImage(int n, const std::string& file, int mode);
    // 字符串精灵（:s/ 标签）：把文本画到精灵层（NScr TRANS_STRING 语义）
    bool SetSpriteText(int n, const std::string& text, tjs_int fontSize, tjs_uint32 color);
    // 多色字符串精灵（上游 :s/ 的 #RRGGBB 颜色列表 = 帧：每色各画一帧，显示帧
    // 由 cell 决定，如 CLANNAD 选项文字 #ffffff 普通 / #CCCCCC 悬停）。本项目
    // 无层 cell 机制，切帧 = 用目标色重画该层。
    bool SetSpriteTextCells(int n, const std::string& text, tjs_int fontSize,
                            const std::vector<tjs_uint32>& colors);
    // 切换字符串精灵显示帧（cell 越界取最后帧；非字符串精灵 no-op）
    void SetSpriteCell(int n, int cell);
    // 位置 / 可见性 / 透明度（改变后立即 Update 标脏）
    void SetSpritePos(int n, int x, int y);
    void SetSpriteVisible(int n, bool visible);
    // 把精灵层纳入按钮最上组（cselbtn 选项文字：下次 relayout 恒最上，
    // 不被立绘/文本层覆盖）
    void MarkSpriteAsBtn(int n);
    void SetSpriteOpacity(int n, tjs_int alpha); // alpha 0-255（255=不透明）
    // 清除精灵（隐藏；n < 0 表示全部）
    void ClearSprite(int n);
    class tTJSNI_BaseLayer* GetSpriteLayer(int n) const
    {
        return (n >= 0 && n < (int)m_sprites.size()) ? m_sprites[n] : nullptr;
    }

    // ---- btn 图像源（btndef ":a;file" 按钮图集，上游 btndef_info）----
    // 载入图集到隐藏精灵层（BTN_SRC_LAYER），btn 命令从其中裁剪按钮图像
    bool SetBtnSource(const std::string& file);
    // 从图集裁剪 (sx,sy,w,h) 画到精灵层 slot，位置 (x,y)
    bool DrawBtnImage(int slot, int sx, int sy, int w, int h, int x, int y);
    static const int BTN_SRC_LAYER = 250; // 图集源层（隐藏）
    void RelayoutSprites(); // 按 z 分界归位：bg(>z 降序) -> 文本层 -> fg(<=z 降序)
    static const int BTN_BASE = 300;      // 按钮显示层基址（BTN_BASE+id）

    // ---- bg 背景（上游 bgCommand 载 background -> 画到主画面）----
    // 载图到隐藏源层再 StretchCopy 缩放到主层铺满；主层尺寸恒定（=窗口），
    // 绝不被图片尺寸改动（krkrz LoadImages 的 InternalSetImageSize 会按图
    // 缩小主层 —— 曾致背景图小于窗口时画面只剩图片区域）。
    bool LoadBgScaled(const std::string& file);

private:
    tTJSVariant m_wndObj;       // 持有窗口对象（防 GC）
    tTJSVariant m_layerObj;     // 持有主层对象
    tTJSVariant m_textLayerObj; // 持有文本层对象
    tTJSVariant m_windowLayerObj; // 持有文本框背景层对象（文本层之下）
    tTJSVariant m_menuLayerObj; // 持有菜单浮层对象
    tTJSVariant m_effectLayerObj; // 持有转场快照层对象
    tTJSVariant m_quakeLayerObj; // 持有 quake 黑色遮底层对象
    tTJSVariant m_bgSrcObj;     // 持有 bg 源层对象（LoadBgScaled 用）
    tTJSNI_BaseLayer* m_layer = nullptr;
    tTJSNI_BaseLayer* m_textLayer = nullptr;
    tTJSNI_BaseLayer* m_windowLayer = nullptr;
    tTJSNI_BaseLayer* m_menuLayer = nullptr;
    tTJSNI_BaseLayer* m_effectLayer = nullptr;
    tTJSNI_BaseLayer* m_quakeLayer = nullptr;
    tTJSNI_BaseLayer* m_bgSrcLayer = nullptr;
    // 转场状态
    bool m_fxActive = false;
    uint32_t m_fxEnd = 0;
    uint32_t m_fxDur = 0;
    // quake 状态
    bool m_quakeActive = false;
    int m_quakeType = 2;
    int m_quakeNo = 1;
    uint32_t m_quakeEnd = 0;
    uint32_t m_quakeDur = 0;
    void EnsureEffectLayer();
    // quake 用黑色遮底层（盖住真实画面；快照层在其上做偏移，露出边缘为黑）
    void EnsureQuakeLayer();
    // 整帧合成快照写入指定层（bg+可见精灵+文本层 -> 层像素缓冲）
    bool CaptureFrameToLayer(class tTJSNI_BaseLayer* lay);
    // 窗背内容缓存（ShowTextWindow 重载用；krkrsdl3 层 visible 切换后内容不渲染，
    // 需 LoadImages/重绘 + SetVisible(true) 同帧）
    std::string m_windowImageFile;
    int m_windowImageX = 0, m_windowImageY = 0;
    bool m_windowColorMode2 = false;
    tjs_uint32 m_windowColor2 = 0;
    int m_zOrder = 499; // 上游默认（ScriptParser.cpp:157）
    // 显式按钮显示层集合（DrawBtnImage 绘制的层；Relayout 恒置最上 ——
    // 上游按钮链整帧最后绘制；不按编号 >=BTN_BASE 硬判，避免误顶普通精灵）
    std::set<int> m_btnLayers;
    // 字符串精灵状态（SetSpriteTextCells 记录；SetSpriteCell 重画用）
    struct TextSpriteInfo
    {
        std::string text;
        int fontSize = 18;
        std::vector<tjs_uint32> colors;
        int cell = 0;
    };
    std::map<int, TextSpriteInfo> m_textSprites;
    // 用 info.colors[cell] 重画精灵层 n（层尺寸不变；cell 越界 clamp）
    void RedrawTextSpriteCell(int n, int cell);
    std::vector<tTJSVariant> m_spriteObjs;   // 持有精灵层对象
    std::vector<tTJSNI_BaseLayer*> m_sprites;
    std::string m_fontFile;
    void* m_buffer = nullptr;
    int m_pitch = 0;
    int m_width = 0, m_height = 0;
    std::atomic<bool> m_dirty{false};
};

#endif // ONS_SCREEN_H
