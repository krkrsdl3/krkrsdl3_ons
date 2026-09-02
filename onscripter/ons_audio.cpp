/* -*- C++ -*-
 *  onscripter/ons_audio.cpp
 *
 *  ONS 音频实现（复用 tTJSNI_WaveSoundBuffer，见 ons_audio.h）。
 *  Open/Play/Stop/SetLooping 由 krkrsdl3 原生管理：全局解码线程
 *  （TVPWakeDecodeSoundBuffer）自动填 L2 缓冲，LoopManager 做无缝循环。
 */

#include "tjsCommHead.h"

#include "ons_audio.h"

#include "PlatformAudio.h" // TVPInitDirectSound
#include "tjsNativeWaveSoundBuffer.h"

#include <cstdio>

void OnsAudio::Init()
{
    if (m_inited)
        return;
    TVPInitDirectSound(48000);
    m_inited = true;
}

void OnsAudio::ApplyVolume(tTJSNI_WaveSoundBuffer* sb, int vol100)
{
    if (!sb)
        return;
    if (vol100 < 0)
        vol100 = 0;
    if (vol100 > 100)
        vol100 = 100;
    sb->SetVolume(vol100 * 1000); // krkrsdl3 量程 0-100000
}

void OnsAudio::Start(tTJSNI_WaveSoundBuffer*& slot, const std::string& name, bool loop,
                     int vol100)
{
    Stop(slot); // 切换：先停旧（上游 Mix_PlayMusic/Mix_PlayChannel 替换语义）
    if (name.empty())
        return;
    Init();
    tTJSNI_WaveSoundBuffer* w = new tTJSNI_WaveSoundBuffer();
    slot = w;
    try
    {
        w->SetLooping(loop);
        w->Open(ttstr(name.c_str()));
        ApplyVolume(w, vol100); // 上游 playSound 按当前 se_volume/voice_volume 起播
        w->Play();
        if (loop)
            m_bgmName = name;
        fprintf(stderr, "[AUDIO] start '%s' loop=%d vol=%d\n", name.c_str(),
                loop ? 1 : 0, vol100);
    }
    catch (...)
    {
        fprintf(stderr, "[AUDIO] start FAILED '%s'\n", name.c_str());
        Stop(slot);
    }
}

void OnsAudio::Stop(tTJSNI_WaveSoundBuffer*& slot)
{
    if (!slot)
        return;
    try
    {
        slot->Stop();      // 停解码线程 + 缓冲 + rewind
        slot->Invalidate(); // 从全局解码管理注销 + 释放资源
    }
    catch (...)
    {
    }
    delete slot;
    slot = nullptr;
}

void OnsAudio::Poll()
{
    // 解码线程自管理：无每帧补缓冲需求。播放结束的通道由下次 Start 覆盖；
    // 空转无害（WaveSoundBuffer 解码线程在 EOF 后自动休眠）。
}

void OnsAudio::PlayBgm(const std::string& name)
{
    Start(m_bgm, name, true, m_bgmVol);
}

void OnsAudio::StopBgm()
{
    Stop(m_bgm);
    m_bgmName.clear();
}

void OnsAudio::PlaySe(const std::string& name)
{
    // SE 通道轮转（上游 wave_sample 通道：新 SE 覆盖最旧）
    Start(m_ses[m_seNext], name, false, m_seVol);
    m_seNext = (m_seNext + 1) % SE_CHANNELS;
}

void OnsAudio::PlayVoice(int ch, const std::string& name)
{
    if (ch < 0 || ch >= VOICE_CHANNELS)
        ch = 0;
    Start(m_voices[ch], name, false, m_voiceVol);
}

void OnsAudio::StopVoice(int ch)
{
    if (ch < 0 || ch >= VOICE_CHANNELS)
        ch = 0;
    Stop(m_voices[ch]);
}

// 音量（0-100）：立即应用到在播缓冲 + 记录供后续起播使用
void OnsAudio::SetBgmVolume(int v)
{
    if (v < 0)
        v = 0;
    if (v > 100)
        v = 100;
    m_bgmVol = v;
    ApplyVolume(m_bgm, v);
}

void OnsAudio::SetSeVolume(int v)
{
    if (v < 0)
        v = 0;
    if (v > 100)
        v = 100;
    m_seVol = v;
    for (auto& s : m_ses)
        ApplyVolume(s, v);
}

void OnsAudio::SetVoiceVolume(int v)
{
    if (v < 0)
        v = 0;
    if (v > 100)
        v = 100;
    m_voiceVol = v;
    for (auto& vc : m_voices)
        ApplyVolume(vc, v);
}

void OnsAudio::StopAll()
{
    StopBgm();
    for (auto& v : m_voices)
        Stop(v);
    for (auto& s : m_ses)
        Stop(s);
}
