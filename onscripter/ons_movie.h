/* -*- C++ -*-
 *  onscripter/ons_movie.h
 *
 *  ONS 视频播放（NScr mov 命令）：复用 krkrsdl3 原生 TJS VideoOverlay
 *  （tjsNativeVideoOverlay）—— Open/Play/Stop/loop/visible 由原生管理。
 *  结束检测：轮询 GetTimePosition/GetTotalTime（上游 playMPEG 播完继续脚本）。
 */

#ifndef ONS_MOVIE_H
#define ONS_MOVIE_H

#include <string>

namespace TJS
{
class iTJSDispatch2;
}

class OnsMovie
{
public:
    OnsMovie() = default;
    ~OnsMovie() { Stop(); }

    // 窗口对象（TJS VideoOverlay 构造参数；start 后设置）
    void SetWindow(TJS::iTJSDispatch2* win) { m_win = win; }

    // 播放（name 为已解析存储名；click = 点击跳过；loop = 循环）
    bool Play(const std::string& name, bool click, bool loop);
    void Stop();
    bool IsPlaying() const { return m_playing; }
    // 播放结束（轮询；未在播返回 true）
    bool IsDone() const;

private:
    TJS::iTJSDispatch2* m_win = nullptr;   // 窗口对象（借用）
    TJS::iTJSDispatch2* m_obj = nullptr;   // VideoOverlay TJS 对象（持有）
    bool m_playing = false;
    bool m_click = false;
    bool m_loop = false;

    bool EnsureOverlay();
    bool CallMethod(const char* name, int nparams, void** pp);
    tjs_int GetInt(const char* prop) const;
};

#endif // ONS_MOVIE_H
