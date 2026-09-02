/* -*- C++ -*-
 *  onscripter/ons_movie.cpp
 *
 *  ONS 视频播放实现（复用 TJS VideoOverlay，见 ons_movie.h）。
 */

#include "tjsCommHead.h"

#include "ons_movie.h"

#include "tjsInterface.h"
#include "tjsVariant.h"
#include "TVPScript.h" // TVPGetScriptDispatch

#include <cstdio>

// 脚本分发（全局对象）
static iTJSDispatch2* GetGlobal()
{
    return TVPGetScriptDispatch();
}

bool OnsMovie::EnsureOverlay()
{
    if (m_obj)
        return true;
    if (!m_win)
        return false;
    try
    {
        iTJSDispatch2* global = GetGlobal();
        if (!global)
            return false;
        tTJSVariant clsVar;
        if (TJS_FAILED(global->PropGet(0, TJS_N("VideoOverlay"), NULL, &clsVar, global)))
            return false;
        iTJSDispatch2* cls = clsVar.AsObjectNoAddRef();
        tTJSVariant wndVar(m_win);
        tTJSVariant* params[1] = {&wndVar};
        iTJSDispatch2* obj = NULL;
        if (TJS_FAILED(cls->CreateNew(0, NULL, NULL, &obj, 1, params, cls)))
            return false;
        m_obj = obj;
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool OnsMovie::CallMethod(const char* name, int nparams, void** pp)
{
    if (!m_obj)
        return false;
    try
    {
        tTJSVariant* vp = nparams > 0 ? *(tTJSVariant**)pp : NULL;
        tTJSVariant** vpp = NULL;
        tTJSVariant arr[4];
        if (nparams > 0 && pp)
        {
            for (int i = 0; i < nparams && i < 4; i++)
            {
                arr[i] = *((tTJSVariant**)pp)[i];
                vpp = NULL;
            }
            vp = arr;
        }
        tjs_error hr =
            m_obj->FuncCall(0, TJS_N(name), NULL, NULL, nparams, nparams > 0 ? &vp : NULL, m_obj);
        return TJS_SUCCEEDED(hr);
    }
    catch (...)
    {
        return false;
    }
}

tjs_int OnsMovie::GetInt(const char* prop) const
{
    if (!m_obj)
        return 0;
    try
    {
        tTJSVariant v;
        if (TJS_FAILED(m_obj->PropGet(0, TJS_N(prop), NULL, &v, m_obj)))
            return 0;
        return (tjs_int)v;
    }
    catch (...)
    {
        return 0;
    }
}

bool OnsMovie::Play(const std::string& name, bool click, bool loop)
{
    Stop();
    if (name.empty() || !EnsureOverlay())
        return false;
    try
    {
        tTJSVariant nameVar(ttstr(name.c_str()));
        tTJSVariant* p1[1] = {&nameVar};
        if (TJS_FAILED(m_obj->FuncCall(0, TJS_N("open"), NULL, NULL, 1, p1, m_obj)))
            return false;
        tTJSVariant bTrue(true);
        m_obj->PropSet(0, TJS_N("visible"), NULL, &bTrue, m_obj);
        tTJSVariant loopVar(loop);
        m_obj->PropSet(0, TJS_N("loop"), NULL, &loopVar, m_obj);
        // 全屏覆盖（640x480）
        tTJSVariant x0(0), y0(0), w(640), h(480);
        tTJSVariant* p4[4] = {&x0, &y0, &w, &h};
        m_obj->FuncCall(0, TJS_N("setBounds"), NULL, NULL, 4, p4, m_obj);
        if (TJS_FAILED(m_obj->FuncCall(0, TJS_N("play"), NULL, NULL, 0, NULL, m_obj)))
            return false;
        m_playing = true;
        m_click = click;
        m_loop = loop;
        fprintf(stderr, "[MOVIE] play '%s' click=%d loop=%d\n", name.c_str(), click ? 1 : 0,
                loop ? 1 : 0);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void OnsMovie::Stop()
{
    if (m_obj && m_playing)
    {
        try
        {
            m_obj->FuncCall(0, TJS_N("stop"), NULL, NULL, 0, NULL, m_obj);
            tTJSVariant bFalse(false);
            m_obj->PropSet(0, TJS_N("visible"), NULL, &bFalse, m_obj);
        }
        catch (...)
        {
        }
    }
    m_playing = false;
}

bool OnsMovie::IsDone() const
{
    if (!m_obj || !m_playing)
        return true;
    // 播放结束：帧位置 >= 总帧数（循环播放不结束）
    if (m_loop)
        return false;
    tjs_int nf = GetInt("numberOfFrame");
    tjs_int f = GetInt("frame");
    if (nf > 0)
        return f >= nf - 1;
    // 总帧数未知：用时间判断（totalTime 毫秒）
    tjs_int total = GetInt("totalTime");
    if (total > 0)
    {
        tjs_int pos = GetInt("timePosition");
        if (pos > 0)
            return pos >= total;
    }
    return false;
}
