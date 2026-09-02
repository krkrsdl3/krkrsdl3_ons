/* -*- C++ -*-
 *  onscripter/ons_screen.cpp
 *
 *  ONS 屏幕实现（Layer 生命周期核心，见 ons_screen.h）。
 */

#include "ons_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "tjsNativeLayer.h"
#include "tjsNativeWindow.h"
#include "LayerManager.h"
#include "LayerBitmap.h" // tTVPBaseTexture（精灵图层尺寸对齐用）
#include "TVPScript.h" // TVPGetScriptDispatch
#include "ons_events.h" // OnsGetTicks

// 小写化（文件内辅助；ons_vm.cpp 另有同名列）
static std::string OnsLower(const std::string& in)
{
    std::string r = in;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return r;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef uint8_t Uint8;
typedef uint32_t Uint32;

void OnsScreen::Init(iTJSDispatch2* win, iTJSDispatch2* baseLayer)
{
    // 注意：ncbind 构造参数为借用引用（AsObjectNoAddRef），这里不得 Release；
    // tTJSVariant(obj,obj) 只是“存活期 pin”（+2/-2 净零），销毁时随 Clear 释放。
    if (!baseLayer || !win)
        return;

    m_layerObj = tTJSVariant(baseLayer, baseLayer);
    m_wndObj = tTJSVariant(win, win);
    m_layer = tTJSNI_Layer::FromObject(m_layerObj.AsObjectNoAddRef());
}

bool OnsScreen::SetSize(int width, int height)
{
    if (!m_layer)
        return false;
    if (width <= 0 || height <= 0)
        return false;
    
    tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(m_layerObj.AsObjectNoAddRef());
    if (!layNI)
        return false;

    // 尺寸 -> 分配图像 -> 可见（顺序重要：先定尺寸再 hasImage）
    layNI->SetSize((tjs_uint)width, (tjs_uint)height);
    layNI->SetHasImage(true);
    layNI->SetVisible(true);

    // 缓存像素缓冲（Layer 存活期内稳定）
    m_buffer = layNI->GetMainImagePixelBufferForWrite();
    m_pitch = layNI->GetMainImagePixelBufferPitch();
    m_width = (int)layNI->GetWidth();
    m_height = (int)layNI->GetHeight();

    return true;
}

bool OnsScreen::CreateTextLayer(int x, int y, int w, int h)
{
    if (!m_layer || m_textLayer)
        return false;
    try
    {
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (!global)
            return false;

        tTJSVariant layClsVar;
        if (TJS_FAILED(global->PropGet(0, TJS_N("Layer"), NULL, &layClsVar, global)))
            return false;
        iTJSDispatch2* layCls = layClsVar.AsObjectNoAddRef();

        // 1) 文本框背景层（btndef ":a;file" 窗背图；先创建 = 在文本层之下）
        tTJSVariant params[2];
        params[0] = m_wndObj;
        params[1] = m_layerObj;
        tTJSVariant* paramPtrs[2] = {&params[0], &params[1]};
        iTJSDispatch2* winObjRaw = NULL;
        if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &winObjRaw, 2, paramPtrs, layCls)))
            return false;
        m_windowLayerObj = tTJSVariant(winObjRaw, winObjRaw);
        winObjRaw->Release();
        tTJSNI_Layer* winNI = tTJSNI_Layer::FromObject(winObjRaw);
        if (!winNI)
            return false;
        tTJSNI_BaseLayer* winBase = winNI;
        winBase->SetPosition(x, y);
        winBase->SetSize((tjs_uint)w, (tjs_uint)h);
        winBase->SetHasImage(true);
        winBase->SetVisible(false); // 与精灵层同构：隐藏创建，SetTextWindowImage 载图+显示
        winBase->SetHitType(htProvince);
        m_windowLayer = winBase;

        // 2) 文本层（窗背层之上）
        iTJSDispatch2* layObjRaw2 = NULL;
        if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &layObjRaw2, 2, paramPtrs, layCls)))
            return false;
        m_textLayerObj = tTJSVariant(layObjRaw2, layObjRaw2);
        layObjRaw2->Release();

        tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(layObjRaw2);
        if (!layNI)
            return false;
        tTJSNI_BaseLayer* base = layNI;

        base->SetPosition(x, y);
        base->SetSize((tjs_uint)w, (tjs_uint)h);
        base->SetHasImage(true);
        base->SetVisible(true);
        base->SetHitType(htProvince);
        m_textLayer = base;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool OnsScreen::SetTextLayerWindowImage(const std::string& file, int x, int y)
{
    if (!m_textLayer)
        return false;
    try
    {
        if (file.empty())
        {
            // 无窗背：清文本层（透明）
            void* buf = m_textLayer->GetMainImagePixelBufferForWrite();
            int pitch = m_textLayer->GetMainImagePixelBufferPitch();
            int w = (int)m_textLayer->GetWidth(), h = (int)m_textLayer->GetHeight();
            if (buf && pitch > 0)
                for (int yy = 0; yy < h; yy++)
                    memset((char*)buf + (size_t)yy * pitch, 0, (size_t)w * 4);
            m_textLayer->Update();
            return true;
        }
        m_textLayer->LoadImages(ttstr(file.c_str()), 0x1FFFFFFF);
        // LoadImages 可能重置层位置：加载后再设置（否则文字/窗背画到 (0,0)）
        m_textLayer->SetPosition(x, y);
        tTVPBaseTexture* img = m_textLayer->GetMainImage();
        if (img && img->GetWidth() > 0 && img->GetHeight() > 0)
        {
            // NScr 图对格式：宽 ≈ 2x 屏宽时左半 RGB + 右半 alpha（如 form2
            // 1280x120 -> 640x120 半透明框）
            tjs_uint w = img->GetWidth();
            tjs_uint h = img->GetHeight();
            if (m_width > 0 && (int)w > m_width && (int)w <= m_width * 2)
            {
                tjs_uint half = w / 2;
                void* buf = m_textLayer->GetMainImagePixelBufferForWrite();
                int pitch = m_textLayer->GetMainImagePixelBufferPitch();
                if (buf && pitch >= (int)(w * 4))
                {
                    for (tjs_uint yy = 0; yy < h; yy++)
                    {
                        unsigned char* row = (unsigned char*)buf + (size_t)yy * pitch;
                        for (tjs_uint xx = 0; xx < half; xx++)
                            row[xx * 4 + 3] = row[(half + xx) * 4];
                    }
                    w = half;
                }
            }
            m_textLayer->SetSize(w, h);
        }
        // SetSize/LoadImages 都可能重置位置：最后再设一次
        m_textLayer->SetPosition(x, y);
        m_textLayer->Update();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool OnsScreen::SetTextWindowImage(const std::string& file, int x, int y)
{
    if (!m_windowLayer)
        return false;
    try
    {
        if (file.empty())
        {
            ClearTextWindow();
            return true;
        }
        m_windowImageFile = file;
        m_windowImageX = x;
        m_windowImageY = y;
        m_windowColorMode2 = false;
        m_windowLayer->SetPosition(x, y);
        m_windowLayer->LoadImages(ttstr(file.c_str()), 0x1FFFFFFF);
        tTVPBaseTexture* img = m_windowLayer->GetMainImage();
        if (img && img->GetWidth() > 0 && img->GetHeight() > 0)
        {
            // NScr 图对格式（TRANS_ALPHA + 无 alpha 的图）：宽 ≈ 2x 屏宽时，
            // 左半为 RGB 画面、右半为 alpha 图 —— 合成后宽度减半（如 form2
            // 1280x120 -> 640x120，alpha ≈ 右半灰度，半透明框）
            tjs_uint w = img->GetWidth();
            tjs_uint h = img->GetHeight();
            if (m_width > 0 && (int)w > m_width && (int)w <= m_width * 2)
            {
                tjs_uint half = w / 2;
                void* buf = m_windowLayer->GetMainImagePixelBufferForWrite();
                int pitch = m_windowLayer->GetMainImagePixelBufferPitch();
                if (buf && pitch >= (int)(w * 4))
                {
                    // 原地合并：右半灰度 -> 左半 alpha（区域不重叠，直接拷贝）
                    for (tjs_uint yy = 0; yy < h && yy < (tjs_uint)m_height; yy++)
                    {
                        unsigned char* row = (unsigned char*)buf + (size_t)yy * pitch;
                        for (tjs_uint xx = 0; xx < half; xx++)
                        {
                            unsigned char alpha = row[(half + xx) * 4]; // 灰度 R
                            row[xx * 4 + 3] = alpha;
                        }
                    }
                    w = half;
                }
                else
                {
                    fprintf(stderr, "[ONS] -- window img pair merge failed (buf=%p pitch=%d)\n",
                            buf, pitch);
                }
            }
            m_windowLayer->SetSize(w, h);
        }
        // 与精灵同构：LoadImages + SetVisible(true) 同函数（krkrsdl3 层在
        // visible 切换后内容不渲染，需同帧载图）
        m_windowLayer->SetVisible(true);
        m_windowLayer->Update();
        fprintf(stderr, "[ONS] -- window img '%s' at (%d,%d) ok\n", file.c_str(), x, y);
        return true;
    }
    catch (...)
    {
        fprintf(stderr, "[ONS] -- window img '%s' FAILED\n", file.c_str());
        return false;
    }
}

bool OnsScreen::SetTextWindowColor(tjs_uint32 color, int x, int y)
{
    if (!m_windowLayer)
        return false;
    try
    {
        m_windowImageFile.clear();
        m_windowColorMode2 = true;
        m_windowColor2 = color;
        m_windowImageX = x;
        m_windowImageY = y;
        m_windowLayer->SetPosition(x, y);
        // 背景层尺寸取文本窗口尺寸（当前 Rect 未变时先保证非零）
        if (m_windowLayer->GetWidth() == 0 || m_windowLayer->GetHeight() == 0)
            m_windowLayer->SetSize(640, 120);
        m_windowLayer->FillRect(tTVPRect(0, 0, (tjs_int)m_windowLayer->GetWidth(),
                                         (tjs_int)m_windowLayer->GetHeight()),
                                color);
        m_windowLayer->SetHasImage(true);
        m_windowLayer->SetVisible(true); // 同帧显示（与 SetTextWindowImage 一致）
        m_windowLayer->Update();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void OnsScreen::ClearTextWindow()
{
    if (!m_windowLayer)
        return;
    m_windowLayer->SetVisible(false);
    m_windowLayer->Update();
}

void OnsScreen::ShowTextWindow()
{
    if (!m_windowLayer)
        return;
    if (m_windowLayer->GetVisible())
        return; // 已显示（页内多行不重载）
    // krkrsdl3 层 visible 切换后内容不渲染：不可见 -> 重载内容 + 显示（同帧）
    if (!m_windowImageFile.empty())
    {
        SetTextWindowImage(m_windowImageFile, m_windowImageX, m_windowImageY);
    }
    else if (m_windowColorMode2)
    {
        SetTextWindowColor(m_windowColor2, m_windowImageX, m_windowImageY);
    }
    else
    {
        m_windowLayer->SetVisible(true);
        m_windowLayer->Update();
    }
}

// ---------------------------------------------------------------------------
// 精灵层（主层子层；懒创建，编号越大越靠上）
// ---------------------------------------------------------------------------

tTJSNI_BaseLayer* OnsScreen::GetMenuLayer()
{
    if (m_menuLayer)
        return m_menuLayer;
    if (!m_layer)
        return nullptr;
    try
    {
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (!global)
            return nullptr;
        tTJSVariant layClsVar;
        if (TJS_FAILED(global->PropGet(0, TJS_N("Layer"), NULL, &layClsVar, global)))
            return nullptr;
        iTJSDispatch2* layCls = layClsVar.AsObjectNoAddRef();
        tTJSVariant params[2];
        params[0] = m_wndObj;
        params[1] = m_layerObj;
        tTJSVariant* paramPtrs[2] = {&params[0], &params[1]};
        iTJSDispatch2* layObjRaw = NULL;
        if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &layObjRaw, 2, paramPtrs, layCls)))
            return nullptr;
        m_menuLayerObj = tTJSVariant(layObjRaw, layObjRaw);
        layObjRaw->Release();
        tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(layObjRaw);
        if (!layNI)
            return nullptr;
        tTJSNI_BaseLayer* base = layNI;
        base->SetSize(m_width > 0 ? (tjs_uint)m_width : 640,
                      m_height > 0 ? (tjs_uint)m_height : 480);
        base->SetHasImage(true);
        base->SetVisible(false); // 默认隐藏
        base->SetHitType(htProvince);
        m_menuLayer = base;
        return base;
    }
    catch (...)
    {
        return nullptr;
    }
}

bool OnsScreen::EnsureSpriteLayer(int n)
{
    if (n < 0 || !m_layer)
        return false;
    if ((int)m_sprites.size() <= n)
    {
        m_spriteObjs.resize((size_t)n + 1);
        m_sprites.resize((size_t)n + 1, nullptr);
    }
    if (m_sprites[n])
        return true;

    try
    {
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (!global)
            return false;
        tTJSVariant layClsVar;
        if (TJS_FAILED(global->PropGet(0, TJS_N("Layer"), NULL, &layClsVar, global)))
            return false;
        iTJSDispatch2* layCls = layClsVar.AsObjectNoAddRef();

        // new Layer(wnd, primaryLayer) —— 精灵作为背景层子层
        tTJSVariant params[2];
        params[0] = m_wndObj;
        params[1] = m_layerObj;
        tTJSVariant* paramPtrs[2] = {&params[0], &params[1]};
        iTJSDispatch2* layObjRaw = NULL;
        if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &layObjRaw, 2, paramPtrs, layCls)))
            return false;
        m_spriteObjs[n] = tTJSVariant(layObjRaw, layObjRaw);
        layObjRaw->Release();

        tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(layObjRaw);
        if (!layNI)
            return false;
        tTJSNI_BaseLayer* base = layNI;
        base->SetVisible(false); // 默认隐藏，首次载图时显示
        base->SetHitType(htProvince);
        m_sprites[n] = base;
        // 精灵层序对齐上游 refreshSurface：同区段编号从大到小绘制（编号小在上），
        // 且按 z 分界（humanz/z_order）分层 —— 编号 > z_order 的精灵画在
        // 窗背文字之下（背景层，如演出大图 201 Snap1），<= z_order 在其上
        // （前景层：按钮/槽条/文字精灵）。布局（底->顶）：
        //   [window, bgSprites(降序), textLayer, fgSprites(降序), 其它]
        try
        {
            RelayoutSprites();
        }
        catch (...)
        {
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void OnsScreen::RelayoutSprites()
{
    // 目标（底->顶，index 0 起）：window(不动) -> 背景精灵(编号>z 降序) ->
    // 文本层 -> 前景精灵(编号<=z 降序) -> 按钮精灵(BTN_BASE+，恒最上)。
    // 非精灵层（菜单/转场快照等保持在尾部最上 —— 快照层在按钮上）。
    // 上游 refreshSurface：按钮(ButtonLink show_flag)在整帧最后绘制，
    // 恒在所有精灵/窗背文字之上 —— BTN_BASE(300+) 大于 z_order(199) 会被
    // 误分进背景组被窗背盖，故单独最上组。
    if (!m_layer)
        return;
    std::vector<int> bg, fg, btn;
    for (size_t k = 0; k < m_sprites.size(); k++)
        if (m_sprites[k])
        {
            if (m_btnLayers.count((int)k))
                btn.push_back((int)k);
            else if ((int)k > m_zOrder)
                bg.push_back((int)k);
            else
                fg.push_back((int)k);
        }
    std::sort(bg.begin(), bg.end(), std::greater<int>());
    std::sort(fg.begin(), fg.end(), std::greater<int>());
    std::sort(btn.begin(), btn.end(), std::greater<int>());
    tjs_int idx = 1; // window(0) 保持最底
    for (int id : bg)
        m_sprites[id]->SetOrderIndex(idx++);
    // 文本层（窗背+文字）在背景精灵之上、前景精灵之下
    tjs_int textIdx = -1;
    if (m_textLayer)
    {
        m_textLayer->SetOrderIndex(idx);
        textIdx = idx;
        idx++;
    }
    for (int id : fg)
        m_sprites[id]->SetOrderIndex(idx++);
    for (int id : btn)
        m_sprites[id]->SetOrderIndex(idx++);
    if (textIdx >= 0)
        fprintf(stderr, "[ONS] -- relayout z=%d bg=%zu fg=%zu btn=%zu text@%d\n", m_zOrder,
                bg.size(), fg.size(), btn.size(), (int)textIdx);
}

bool OnsScreen::SetSpriteImage(int n, const std::string& file, int mode)
{
    if (!EnsureSpriteLayer(n))
    {
        return false;
    }
    tTJSNI_BaseLayer* l = m_sprites[n];
    try
    {
        fprintf(stderr, "[ONS] -- sprite %d load '%s' mode=%d\n", n, file.c_str(), mode);
        // 统一先无 key 载入（clNone），模式处理在像素层做
        l->LoadImages(ttstr(file.c_str()), 0x1FFFFFFF);
        tTVPBaseTexture* tex = l->GetMainImage();
        if (tex && tex->GetWidth() > 0 && tex->GetHeight() > 0)
        {
            int w = (int)tex->GetWidth(), h = (int)tex->GetHeight();
            void* buf = l->GetMainImagePixelBufferForWrite();
            int pitch = l->GetMainImagePixelBufferPitch();
            std::string low = OnsLower(file);
            bool noAlphaFmt = low.size() > 4 &&
                              (low.substr(low.size() - 4) == ".jpg" ||
                               low.substr(low.size() - 4) == ".jpeg" ||
                               low.substr(low.size() - 4) == ".bmp");
            if (mode == IMG_TOPLEFT || mode == IMG_TOPRIGHT)
            {
                // 上游 TRANS_TOPLEFT/TOPRIGHT（:l/:r 前缀或 transmode 默认）：
                // 以左上/右上角第一个像素为 colorkey 挖透明（素材透明区涂该色）。
                uint32_t ref = tex->GetPoint(mode == IMG_TOPLEFT ? 0 : w - 1, 0) & 0xffffff;
                if (buf && pitch >= w * 4)
                {
                    for (int yy = 0; yy < h; yy++)
                    {
                        unsigned char* row = (unsigned char*)buf + (size_t)yy * pitch;
                        for (int xx = 0; xx < w; xx++)
                        {
                            uint32_t px;
                            std::memcpy(&px, row + (size_t)xx * 4, 4);
                            if ((px & 0xffffff) == ref)
                                row[(size_t)xx * 4 + 3] = 0;
                        }
                    }
                }
            }
            else if (mode == IMG_ALPHA && noAlphaFmt)
            {
                // 上游 setupImageAlpha TRANS_ALPHA + 无 alpha 通道（jpg/bmp）：
                // 一律按图对处理 —— 右半 = 反相 alpha 掩码（灰度：白=透明、
                // 黑=实体；alpha = 255-右半灰度 ^0xff 语义），左半 = RGB
                // （透明区涂色由合成 alpha 覆盖，无需挖色）。结果真裁半宽、
                // 右半数据整体丢弃。上游无任何尺寸/灰度启发式 —— 类型即语义。
                int half = w / 2;
                fprintf(stderr, "[ONS] -- sprite %d alpha-pair: w %d -> %d\n", n, w, half);
                for (int yy = 0; yy < h; yy++)
                {
                    unsigned char* row = (unsigned char*)buf + (size_t)yy * pitch;
                    for (int xx = 0; xx < half; xx++)
                    {
                        unsigned char a = row[(size_t)(half + xx) * 4]; // 掩码 R 通道
                        row[(size_t)xx * 4 + 3] = (unsigned char)(255 - a);
                    }
                }
                tex->SetSize((tjs_uint)half, (tjs_uint)h, true); // 真裁右半
                w = half;
            }
            l->SetSize((tjs_uint)w, (tjs_uint)h);
        }
    }
    catch (...)
    {
        return false;
    }
    // 图层尺寸对齐图片（LoadImages 只在图片更小时收缩 Rect，不会扩大；
    // 否则大图只显示图层 Rect 区域，按钮命中/显示都不对）
    tTVPBaseTexture* img = l->GetMainImage();
    if (img && img->GetWidth() > 0 && img->GetHeight() > 0)
        l->SetSize(img->GetWidth(), img->GetHeight());
    l->SetVisible(true);
    l->Update();
    return true;
}

bool OnsScreen::SetSpriteText(int n, const std::string& text, tjs_int fontSize,
                              tjs_uint32 color)
{
    std::vector<tjs_uint32> colors;
    colors.push_back(color);
    return SetSpriteTextCells(n, text, fontSize, colors);
}

bool OnsScreen::SetSpriteTextCells(int n, const std::string& text, tjs_int fontSize,
                                   const std::vector<tjs_uint32>& colors)
{
    if (!EnsureSpriteLayer(n))
        return false;
    tTJSNI_BaseLayer* l = m_sprites[n];
    TextSpriteInfo info;
    info.text = text;
    info.fontSize = fontSize > 0 ? fontSize : 18;
    info.colors = colors;
    if (info.colors.empty())
        info.colors.push_back(0xffffff);
    info.cell = 0;
    m_textSprites[n] = info;
    try
    {
        if (!m_fontFile.empty())
        {
            l->SetFontFace(ttstr(m_fontFile.c_str()));
            l->SetFontFaceIsFileName(true);
        }
        l->SetFontHeight(info.fontSize);
        // 先按文本估算尺寸并分配图像，再绘制 —— 新精灵层无 MainImage，
        // 直接 FillRect/DrawText 会抛 TVPNotDrawableLayerType（静默失败，
        // 字符串精灵"画不出来"的根因）。
        // 宽估算：ASCII/半角按 0.55 字号、CJK/全角按 1.0 字号（估大不估小，
        // 宁多留白不裁剪）；高 = 字号 * 1.4。
        double wEst = 8;
        for (size_t i = 0; i < text.size();)
            if ((unsigned char)text[i] < 0x80)
            {
                wEst += info.fontSize * 0.55;
                i += 1;
            }
            else
            {
                unsigned char c = (unsigned char)text[i];
                i += (c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4);
                wEst += info.fontSize;
            }
        l->SetSize((tjs_uint)std::max(1, (int)wEst), (tjs_uint)(info.fontSize * 14 / 10));
        l->SetHasImage(true);
        RedrawTextSpriteCell(n, 0);
        l->SetVisible(true);
        l->Update();
        fprintf(stderr, "[ONS] -- SetSpriteText %d ok text='%s' size=%dx%d cells=%zu\n",
                n, text.c_str(), (int)l->GetWidth(), (int)l->GetHeight(), info.colors.size());
        return true;
    }
    catch (eTJS& e)
    {
        fprintf(stderr, "[ONS] -- SetSpriteText %d EXC: %s\n", n,
                e.GetMessage().AsStdString().c_str());
        return false;
    }
    catch (...)
    {
        fprintf(stderr, "[ONS] -- SetSpriteText %d EXC unknown\n", n);
        return false;
    }
}

void OnsScreen::SetSpriteCell(int n, int cell)
{
    if (cell < 0)
        cell = 0;
    auto it = m_textSprites.find(n);
    if (it == m_textSprites.end() || it->second.colors.size() <= 1)
        return; // 非字符串精灵 / 单色：无帧可切
    if (cell >= (int)it->second.colors.size())
        cell = (int)it->second.colors.size() - 1;
    if (it->second.cell == cell)
        return;
    it->second.cell = cell;
    RedrawTextSpriteCell(n, cell);
}

// 用 info.colors[cell] 重画字符串精灵层（层尺寸不变；透明清底）
void OnsScreen::RedrawTextSpriteCell(int n, int cell)
{
    tTJSNI_BaseLayer* l = GetSpriteLayer(n);
    auto it = m_textSprites.find(n);
    if (!l || it == m_textSprites.end())
        return;
    const TextSpriteInfo& info = it->second;
    if (cell < 0)
        cell = 0;
    if (cell >= (int)info.colors.size())
        cell = (int)info.colors.size() - 1;
    try
    {
        l->FillRect(tTVPRect(0, 0, l->GetWidth(), l->GetHeight()), 0);
        l->DrawText(0, 0, ttstr(info.text.c_str()), info.colors[cell], 255, true, 0, 0, 0,
                    0, 0);
        l->Update();
    }
    catch (eTJS& e)
    {
        fprintf(stderr, "[ONS] -- SetSpriteText redraw %d EXC: %s\n", n,
                e.GetMessage().AsStdString().c_str());
    }
    catch (...)
    {
    }
}

void OnsScreen::SetSpritePos(int n, int x, int y)
{
    tTJSNI_BaseLayer* l = GetSpriteLayer(n);
    if (!l)
        return;
    l->SetPosition(x, y);
    l->Update();
}

void OnsScreen::SetSpriteVisible(int n, bool visible)
{
    tTJSNI_BaseLayer* l = GetSpriteLayer(n);
    if (!l)
        return;
    l->SetVisible(visible);
    l->Update();
}

void OnsScreen::SetSpriteOpacity(int n, tjs_int alpha)
{
    tTJSNI_BaseLayer* l = GetSpriteLayer(n);
    if (!l)
        return;
    l->SetOpacity(alpha < 0 ? 255 : (alpha > 255 ? 255 : alpha));
    l->Update();
}

void OnsScreen::MarkSpriteAsBtn(int n)
{
    if (n < 0)
        return;
    if ((int)m_btnLayers.size() <= 0)
        m_btnLayers.clear();
    m_btnLayers.insert(n);
}

void OnsScreen::ClearSprite(int n)
{
    if (n < 0)
    {
        for (auto* l : m_sprites)
        {
            if (l)
            {
                l->SetVisible(false);
                l->Update();
            }
        }
        return;
    }
    SetSpriteVisible(n, false);
}

bool OnsScreen::SetBtnSource(const std::string& file)
{
    if (file.empty())
        return false;
    // btndef 图集：上游 btndefCommand 解析前缀后强制 TRANS_COPY（原样，
    // 不挖色不图对合成 —— form3.jpg 16x102 ▲▼ 竖排图集等）
    if (!SetSpriteImage(BTN_SRC_LAYER, file, IMG_COPY))
    {
        // 载入失败即清空图源：上游 btndef_info.remove()（command.cpp 3549），
        // 否则 blt 会误贴上一轮图集（ever17 specialEffectImage 曾因此贴旧图）
        ClearSprite(BTN_SRC_LAYER);
        return false;
    }
    // 图集源层隐藏（仅作裁剪源）
    SetSpriteVisible(BTN_SRC_LAYER, false);
    return true;
}

bool OnsScreen::LoadBgScaled(const std::string& file)
{
    if (!m_layer)
        return false;
    try
    {
        // 隐藏源层（懒创建）：载图用 —— 图多大层多大，无妨（仅作拉伸源）。
        if (!m_bgSrcLayer)
        {
            iTJSDispatch2* global = TVPGetScriptDispatch();
            if (!global)
                return false;
            tTJSVariant layClsVar;
            if (TJS_FAILED(global->PropGet(0, TJS_N("Layer"), NULL, &layClsVar, global)))
                return false;
            iTJSDispatch2* layCls = layClsVar.AsObjectNoAddRef();
            tTJSVariant params[2];
            params[0] = m_wndObj;
            params[1] = m_layerObj;
            tTJSVariant* paramPtrs[2] = {&params[0], &params[1]};
            iTJSDispatch2* layObjRaw = NULL;
            if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &layObjRaw, 2, paramPtrs, layCls)))
                return false;
            m_bgSrcObj = tTJSVariant(layObjRaw, layObjRaw);
            layObjRaw->Release();
            tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(layObjRaw);
            if (!layNI)
                return false;
            tTJSNI_BaseLayer* base = layNI;
            base->SetSize(1, 1);
            base->SetHasImage(true);
            base->SetVisible(false); // 仅作拉伸源，从不显示
            base->SetHitType(htProvince);
            m_bgSrcLayer = base;
        }
        tTJSNI_BaseLayer* src = m_bgSrcLayer;
        tTJSNI_BaseLayer* dst = GetPrimaryLayer();
        if (!src || !dst)
            return false;
        src->LoadImages(ttstr(file.c_str()), 0x1FFFFFFF); // clNone：不挖透明
        tTVPBaseTexture* simg = src->GetMainImage();
        if (!simg || simg->GetWidth() <= 0 || simg->GetHeight() <= 0)
            return false;
        int iw = (int)simg->GetWidth(), ih = (int)simg->GetHeight();
        // 主层尺寸恒定（=窗口）。先恢复（若曾被 LoadImages 类路径缩小），
        // 再整图缩放铺满 —— 图比窗口小/大都被拉伸，窗口绝不改尺寸。
        dst->SetSize((tjs_uint)m_width, (tjs_uint)m_height);
        dst->StretchCopy(tTVPRect(0, 0, m_width, m_height), simg,
                         tTVPRect(0, 0, iw, ih), stNearest, 0);
        dst->Update();
        fprintf(stderr, "[ONS] -- bg scaled %dx%d -> %dx%d\n", iw, ih, m_width, m_height);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool OnsScreen::DrawBtnImage(int slot, int sx, int sy, int w, int h, int x, int y)
{
    tTJSNI_BaseLayer* src = GetSpriteLayer(BTN_SRC_LAYER);
    if (!src)
        return false;
    if (!EnsureSpriteLayer(slot))
        return false;
    m_btnLayers.insert(slot); // 按钮显示层：Relayout 恒最上（上游按钮最后绘制）
    try
    {
        tTJSNI_BaseLayer* l = m_sprites[slot];
        l->SetSize((tjs_uint)std::max(1, w), (tjs_uint)std::max(1, h));
        l->SetPosition(x, y);
        // 从图集裁剪按钮图像（上游 btnCommand：copySurface(btndef_info, src_rect)）
        l->PiledCopy(0, 0, src, tTVPRect(sx, sy, sx + std::max(0, w), sy + std::max(0, h)));
        l->SetVisible(true);
        l->Update();
        return true;
}
    catch (...)
    {
        return false;
    }
}

void OnsScreen::Destroy()
{
    // 引擎线程必须已 join（由 ONSScreen::stop 保证）
    if (m_textLayerObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_textLayerObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o); // Part/释放图像
            }
            catch (...)
            {
            }
        }
        m_textLayerObj.Clear();
    }
    m_textLayer = nullptr;
    if (m_windowLayerObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_windowLayerObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o); // Part/释放图像
            }
            catch (...)
            {
            }
        }
        m_windowLayerObj.Clear();
    }
    m_windowLayer = nullptr;
    if (m_menuLayerObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_menuLayerObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o);
            }
            catch (...)
            {
            }
        }
        m_menuLayerObj.Clear();
    }
    m_menuLayer = nullptr;
    m_fxActive = false;
    m_quakeActive = false;
    if (m_effectLayerObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_effectLayerObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o);
            }
            catch (...)
            {
            }
        }
        m_effectLayerObj.Clear();
    }
    m_effectLayer = nullptr;
    if (m_quakeLayerObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_quakeLayerObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o);
            }
            catch (...)
            {
            }
        }
        m_quakeLayerObj.Clear();
    }
    m_quakeLayer = nullptr;
    for (auto& vo : m_spriteObjs)
    {
        if (vo.Type() == tvtObject)
        {
            iTJSDispatch2* o = vo.AsObjectNoAddRef();
            if (o)
            {
                try
                {
                    o->Invalidate(0, NULL, NULL, o); // Part/释放图像
                }
                catch (...)
                {
                }
            }
            vo.Clear();
        }
    }
    m_spriteObjs.clear();
    m_sprites.clear();
    m_textSprites.clear(); // 字符串精灵状态随层销毁
    if (m_layerObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_layerObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o); // DetachPrimary/Unregister/Part/释放图像
            }
            catch (...)
            {
            }
        }
        m_layerObj.Clear();
    }
    if (m_wndObj.Type() == tvtObject)
    {
        iTJSDispatch2* o = m_wndObj.AsObjectNoAddRef();
        if (o)
        {
            try
            {
                o->Invalidate(0, NULL, NULL, o);
            }
            catch (...)
            {
            }
        }
        m_wndObj.Clear();
    }
    m_layer = nullptr;
    m_buffer = nullptr;
    m_pitch = 0;
    m_width = m_height = 0;
    m_dirty = false;
}

void OnsScreen::ForceFullRefresh()
{
    if (m_layer)
    {
        tTVPLayerManager* mgr = m_layer->GetManager();
        if (mgr)
            mgr->RequestInvalidation(tTVPRect(0, 0, m_width, m_height));
    }
}

void OnsScreen::Tick()
{
    // 全屏强制重绘开关（实验：每帧全屏 invalidate 时 CPU 高；按钮残留根因
    // 已按上游 show_flag 语义修复 —— btnwait 返回即隐藏按钮精灵，且所有
    // 层修改路径（SetVisible/SetPosition/LoadImages/CPU 直写后 Update）都会
    // 自动标脏，增量合成即可正确刷新）
    if (m_dirty.exchange(false))
    {
        if (m_layer)
        {
            tTVPLayerManager* mgr = m_layer->GetManager();
            if (mgr)
                mgr->RequestInvalidation(tTVPRect(0, 0, m_width, m_height));
        }
    }

    // 转场推进（crossfade：快照层 opacity 255 -> 0）
    if (m_fxActive)
    {
        uint32_t now = OnsGetTicks();
        if (now >= m_fxEnd)
        {
            m_fxActive = false;
            if (m_effectLayer)
            {
                m_effectLayer->SetVisible(false);
                m_effectLayer->Update();
                fprintf(stderr, "[ONS] fx end @%u\n", (unsigned)now);
            }
        }
        else if (m_effectLayer)
        {
            uint32_t el = now - (m_fxEnd - m_fxDur);
            int opa = (int)(255 * (m_fxDur - el) / m_fxDur);
            m_effectLayer->SetOpacity(opa < 0 ? 0 : (opa > 255 ? 255 : opa));
        }
    }

    // quake 推进（上游 quakeCommand/doEffect MAX_EFFECT+0..2）
    if (m_quakeActive)
    {
        uint32_t now = OnsGetTicks();
        if (now >= m_quakeEnd)
        {
            m_quakeActive = false;
            if (m_effectLayer)
            {
                m_effectLayer->SetPosition(0, 0);
                m_effectLayer->SetVisible(false);
                m_effectLayer->Update();
            }
            if (m_quakeLayer)
            {
                m_quakeLayer->SetVisible(false);
                m_quakeLayer->Update();
            }
        }
        else if (m_effectLayer)
        {
            uint32_t el = now - (m_quakeEnd - m_quakeDur);
            int dx = 0, dy = 0;
            if (m_quakeType == 2)
            {
                // quake：随机抖动 ±2*no（上游 doEffect case MAX+2）
                dx = m_quakeNo * ((rand() % 3) - 1) * 2;
                dy = m_quakeNo * ((rand() % 3) - 1) * 2;
            }
            else
            {
                // quakex/quakey：正弦衰减（上游 case MAX+0/+1；
                // EFFECT_QUAKE_AMP = 12，非 24）
                double amp = sin(M_PI * 2.0 * m_quakeNo * el / m_quakeDur) *
                             12.0 * m_quakeNo * (m_quakeDur - el) / m_quakeDur;
                if (m_quakeType == 1)
                    dx = (int)amp;
                else
                    dy = (int)amp;
            }
            m_effectLayer->SetPosition(dx, dy);
        }
    }
}

// 转场快照层（懒创建；位于主层之上、精灵层之下）
void OnsScreen::EnsureEffectLayer()
{
    if (m_effectLayer || !m_layer)
        return;
    try
    {
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (!global)
            return;
        tTJSVariant layClsVar;
        if (TJS_FAILED(global->PropGet(0, TJS_N("Layer"), NULL, &layClsVar, global)))
            return;
        iTJSDispatch2* layCls = layClsVar.AsObjectNoAddRef();
        tTJSVariant params[2];
        params[0] = m_wndObj;
        params[1] = m_layerObj;
        tTJSVariant* paramPtrs[2] = {&params[0], &params[1]};
        iTJSDispatch2* layObjRaw = NULL;
        if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &layObjRaw, 2, paramPtrs, layCls)))
            return;
        m_effectLayerObj = tTJSVariant(layObjRaw, layObjRaw);
        layObjRaw->Release();
        tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(layObjRaw);
        if (!layNI)
            return;
        tTJSNI_BaseLayer* base = layNI;
        base->SetSize(m_width > 0 ? (tjs_uint)m_width : 640,
                      m_height > 0 ? (tjs_uint)m_height : 480);
        base->SetHasImage(true);
        base->SetVisible(false); // 默认隐藏
        base->SetHitType(htProvince);
        m_effectLayer = base;
    }
    catch (...)
    {
    }
}

// 整帧合成快照写入指定层（bg+可见精灵+文本层 -> 层像素缓冲）。
// 供转场（旧画面淡出）与 quake（抖动画面）共用 —— 上游两者的内容源都是
// 整帧快照（effect_src/effect_dst = accumulation_surface 整帧）。
bool OnsScreen::CaptureFrameToLayer(tTJSNI_BaseLayer* lay)
{
    if (!lay)
        return false;
    try
    {
        std::vector<uint32_t> frame;
        int fw = 0, fh = 0;
        if (!CompositeSnapshot(frame, fw, fh))
            return false;
        tTVPBaseTexture* tex = lay->GetMainImage();
        if (!tex)
            return false;
        lay->SetSize((tjs_uint)fw, (tjs_uint)fh);
        lay->SetHasImage(true);
        void* buf = lay->GetMainImagePixelBufferForWrite();
        int pitch = lay->GetMainImagePixelBufferPitch();
        if (buf && pitch >= fw * 4 && (int)frame.size() >= fw * fh)
        {
            const uint8_t* src = (const uint8_t*)frame.data();
            for (int yy = 0; yy < fh; yy++)
                std::memcpy((uint8_t*)buf + (size_t)yy * pitch, src + (size_t)yy * fw * 4,
                            (size_t)fw * 4);
        }
        lay->Update();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool OnsScreen::StartBgTransition(int effectNo, int durationMs)
{
    if (!m_layer || effectNo <= 1 || durationMs <= 0)
        return false; // 即时：无转场
    EnsureEffectLayer();
    if (!m_effectLayer)
        return false;
    try
    {
        // 快照旧画面 = 整帧合成（bg + 可见精灵 + 文本层；上游 effect_src =
        // 旧整帧，ONScripter_effect.cpp:38-58 —— 只拷主层会让精灵/窗背文字
        // 在淡出期间实时跳动/露出残余）。合成帧写入转场层像素缓冲。
        if (!CaptureFrameToLayer(m_effectLayer))
            return false;
        m_effectLayer->SetVisible(true);
        m_effectLayer->SetOpacity(255);
        m_effectLayer->Update();
    }
    catch (...)
    {
        return false;
    }
    m_fxActive = true;
    m_fxDur = (uint32_t)durationMs;
    m_fxEnd = OnsGetTicks() + m_fxDur;
    fprintf(stderr, "[ONS] fx start @%u dur=%u effectNo=%d\n", (unsigned)(m_fxEnd - m_fxDur),
            (unsigned)m_fxDur, effectNo);
    return true;
}

// quake 黑色遮底层（懒创建；盖住真实画面，快照层在其上做偏移）
void OnsScreen::EnsureQuakeLayer()
{
    if (m_quakeLayer || !m_layer)
        return;
    try
    {
        iTJSDispatch2* global = TVPGetScriptDispatch();
        if (!global)
            return;
        tTJSVariant layClsVar;
        if (TJS_FAILED(global->PropGet(0, TJS_N("Layer"), NULL, &layClsVar, global)))
            return;
        iTJSDispatch2* layCls = layClsVar.AsObjectNoAddRef();
        tTJSVariant params[2];
        params[0] = m_wndObj;
        params[1] = m_layerObj;
        tTJSVariant* paramPtrs[2] = {&params[0], &params[1]};
        iTJSDispatch2* layObjRaw = NULL;
        if (TJS_FAILED(layCls->CreateNew(0, NULL, NULL, &layObjRaw, 2, paramPtrs, layCls)))
            return;
        m_quakeLayerObj = tTJSVariant(layObjRaw, layObjRaw);
        layObjRaw->Release();
        tTJSNI_Layer* layNI = tTJSNI_Layer::FromObject(layObjRaw);
        if (!layNI)
            return;
        tTJSNI_BaseLayer* base = layNI;
        base->SetSize(m_width > 0 ? (tjs_uint)m_width : 640,
                      m_height > 0 ? (tjs_uint)m_height : 480);
        base->SetHasImage(true);
        base->SetVisible(false); // 默认隐藏
        base->SetHitType(htProvince);
        m_quakeLayer = base;
    }
    catch (...)
    {
    }
}

bool OnsScreen::StartQuake(int type, int no, int durationMs)
{
    if (!m_layer || no <= 0 || durationMs <= 0)
        return false;
    if (m_quakeActive || m_fxActive)
        return false; // quake 阻塞执行，正常不会与转场/另一抖动重叠
    // 主层（bg 载体）不可移动（krkrsdl3 TVPCannotMovePrimary —— 本次闪退根因：
    // quake 抖动把 SetPosition 打在 primary 上）。改为与上游一致的做法：
    // quakeCommand 先快照整帧（accumulation_surface -> effect_dst_surface），
    // doEffect MAX+0..2 每帧整屏填黑 + 按偏移重画快照 —— 这里用整帧快照层
    // 做偏移，其下黑色遮底层盖住真实画面，露出的边缘即黑色。
    EnsureEffectLayer();
    EnsureQuakeLayer();
    if (!m_effectLayer || !m_quakeLayer)
        return false;
    try
    {
        if (!CaptureFrameToLayer(m_effectLayer))
            return false;
        m_quakeLayer->FillRect(tTVPRect(0, 0, m_width, m_height), 0x000000);
        m_effectLayer->SetPosition(0, 0);
        m_effectLayer->SetOpacity(255);
        // 层级：黑色遮底 -> 快照层（各自置顶；先遮底后快照，快照在最上）
        m_quakeLayer->SetVisible(true);
        m_quakeLayer->SetOrderIndex(1 << 20);
        m_quakeLayer->Update();
        m_effectLayer->SetVisible(true);
        m_effectLayer->SetOrderIndex(1 << 20);
        m_effectLayer->Update();
    }
    catch (...)
    {
        return false;
    }
    m_quakeActive = true;
    m_quakeType = type;
    m_quakeNo = no;
    m_quakeDur = (uint32_t)durationMs;
    m_quakeEnd = OnsGetTicks() + m_quakeDur;
    fprintf(stderr, "[ONS] quake start type=%d no=%d dur=%u\n", type, no,
            (unsigned)m_quakeDur);
    return true;
}

// 层像素拷贝 + alpha 混合（层内布局 BGRA8888；目标同为 BGRA8888）。
static void BlitLayerInto(uint32_t* dst, int dstW, int dstH,
                          const uint32_t* src, int srcW, int srcH, int pitch,
                          int dx, int dy, int opacity)
{
    if (!dst || !src || opacity <= 0)
        return;
    int x0 = dx < 0 ? 0 : dx;
    int y0 = dy < 0 ? 0 : dy;
    int x1 = (dx + srcW) < dstW ? (dx + srcW) : dstW;
    int y1 = (dy + srcH) < dstH ? (dy + srcH) : dstH;
    for (int y = y0; y < y1; y++)
    {
        const uint32_t* srow = src + (size_t)(y - dy) * (pitch / 4);
        uint32_t* drow = dst + (size_t)y * dstW;
        for (int x = x0; x < x1; x++)
        {
            uint32_t s = srow[x - dx];
            int a = ((int)(s >> 24) & 0xff) * opacity / 255;
            if (a == 0)
                continue;
            if (a >= 255)
            {
                drow[x] = s;
                continue;
            }
            // BGRA 通道混合（straight alpha）
            uint32_t d = drow[x];
            int na = 255 - a;
            drow[x] = ((uint32_t)((((s & 0xff) * a + (d & 0xff) * na) / 255) & 0xff)) |
                      ((uint32_t)(((((s >> 8) & 0xff) * a + ((d >> 8) & 0xff) * na) / 255) & 0xff)
                       << 8) |
                      ((uint32_t)(((((s >> 16) & 0xff) * a + ((d >> 16) & 0xff) * na) / 255) & 0xff)
                       << 16) |
                      0xff000000u;
        }
    }
}

bool OnsScreen::CompositeSnapshot(std::vector<uint32_t>& bgra, int& w, int& h)
{
    if (!m_layer || m_width <= 0 || m_height <= 0)
        return false;
    try
    {
        w = m_width;
        h = m_height;
        bgra.assign((size_t)w * h, 0xff000000u); // 黑底
        // 1) bg
        tTVPBaseTexture* img = m_layer->GetMainImage();
        if (img)
            BlitLayerInto(bgra.data(), w, h, (const uint32_t*)img->GetScanLine(0),
                          (int)img->GetWidth(), (int)img->GetHeight(), img->GetPitchBytes(),
                          0, 0, 255);
        // 2) 可见精灵（编号顺序 = 绘制顺序）
        for (tTJSNI_BaseLayer* sp : m_sprites)
        {
            if (!sp || !sp->GetVisible())
                continue;
            tTVPBaseTexture* simg = sp->GetMainImage();
            if (!simg)
                continue;
            BlitLayerInto(bgra.data(), w, h, (const uint32_t*)simg->GetScanLine(0),
                          (int)simg->GetWidth(), (int)simg->GetHeight(),
                          simg->GetPitchBytes(), (int)sp->GetLeft(), (int)sp->GetTop(),
                          sp->GetOpacity());
        }
        // 3) 文本层（窗背+文字）
        if (m_textLayer && m_textLayer->GetVisible())
        {
            tTVPBaseTexture* timg = m_textLayer->GetMainImage();
            if (timg)
                BlitLayerInto(bgra.data(), w, h, (const uint32_t*)timg->GetScanLine(0),
                              (int)timg->GetWidth(), (int)timg->GetHeight(),
                              timg->GetPitchBytes(), (int)m_textLayer->GetLeft(),
                              (int)m_textLayer->GetTop(), m_textLayer->GetOpacity());
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void OnsScreen::Present(const void* pixels, int w, int h, int pitch)
{
    if (!m_buffer || !pixels || w <= 0 || h <= 0)
        return;

    if (w == m_width && h == m_height)
    {
        // ARGB8888 -> RGBA8888（与 krkrsdl3 Layer 缓冲布局一致）
        const Uint32* sp = (const Uint32*)pixels;
        Uint8* dp = (Uint8*)m_buffer;
        for (int y = 0; y < m_height; y++)
        {
            const Uint32* srow = sp + (size_t)y * (pitch / 4);
            Uint8* drow = dp + (size_t)y * m_pitch;
            for (int x = 0; x < m_width; x++)
            {
                Uint32 c = srow[x];
                drow[x * 4 + 0] = (Uint8)((c >> 16) & 0xff);
                drow[x * 4 + 1] = (Uint8)((c >> 8) & 0xff);
                drow[x * 4 + 2] = (Uint8)(c & 0xff);
                drow[x * 4 + 3] = (Uint8)((c >> 24) & 0xff);
            }
        }
    }
    else
    {
        // 运行期分辨率变化：最近邻缩放
        const Uint32* sp = (const Uint32*)pixels;
        for (int y = 0; y < m_height; y++)
        {
            int sy = std::min(h - 1, y * h / m_height);
            const Uint32* srow = sp + (size_t)sy * (pitch / 4);
            Uint8* drow = (Uint8*)m_buffer + (size_t)y * m_pitch;
            for (int x = 0; x < m_width; x++)
            {
                int sx = std::min(w - 1, x * w / m_width);
                Uint32 c = srow[sx];
                drow[x * 4 + 0] = (Uint8)((c >> 16) & 0xff);
                drow[x * 4 + 1] = (Uint8)((c >> 8) & 0xff);
                drow[x * 4 + 2] = (Uint8)(c & 0xff);
                drow[x * 4 + 3] = (Uint8)((c >> 24) & 0xff);
            }
        }
    }
    m_dirty = true;
}
