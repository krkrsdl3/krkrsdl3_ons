/* -*- C++ -*-
 *  onscripter/ons_audio.h
 *
 *  ONS 音频：复用 krkrsdl3 原生 tTJSNI_WaveSoundBuffer（TJS WaveSoundBuffer 的
 *  原生实现）的完整管理机制 —— 解码线程、L1/L2 双缓冲、无缝循环（LoopManager）、
 *  音量 —— 与 Onscripter 的 SDL_mixer 模型对齐：
 *  - BGM：SetLooping(true)（上游 Mix_PlayMusic -1 循环，内部无缝）；
 *  - SE / 语音：SetLooping(false) 一次性（上游 Mix_PlayChannel loop 参数）；
 *  - 切换：Stop 旧（停解码线程 + 清缓冲 + 注销）再 Open 新 —— 无残留。
 */

#ifndef ONS_AUDIO_H
#define ONS_AUDIO_H

#include <string>

class tTJSNI_WaveSoundBuffer;

class OnsAudio
{
public:
    OnsAudio() = default;
    ~OnsAudio() { StopAll(); }

    void Init(); // TVPInitDirectSound（幂等）

    // 每 tick 调用：解码线程自管理，Poll 仅做播放结束的清理兜底
    void Poll();

    void PlayBgm(const std::string& name); // 循环
    void StopBgm();
    // 当前 BGM 名（存档/读档恢复用；空 = 无）
    const std::string& GetCurrentBgm() const { return m_bgmName; }
    void PlaySe(const std::string& name); // 一次性
    void PlayVoice(int ch, const std::string& name); // 一次性
    void StopVoice(int ch);
    void StopAll();

    // 音量（0-100；上游 music_volume/se_volume/voice_volume，
    // Mix_Volume(n*MIX_MAX_VOLUME/100) 语义；krkrsdl3 量程 0-100000）
    void SetBgmVolume(int v);
    void SetSeVolume(int v);
    void SetVoiceVolume(int v);
    int GetBgmVolume() const { return m_bgmVol; }
    int GetSeVolume() const { return m_seVol; }
    int GetVoiceVolume() const { return m_voiceVol; }

private:
    enum
    {
        VOICE_CHANNELS = 4,
        SE_CHANNELS = 8
    };

    tTJSNI_WaveSoundBuffer* m_bgm = nullptr;
    tTJSNI_WaveSoundBuffer* m_voices[VOICE_CHANNELS] = {};
    tTJSNI_WaveSoundBuffer* m_ses[SE_CHANNELS] = {};
    int m_seNext = 0;
    bool m_inited = false;
    std::string m_bgmName;
    int m_bgmVol = 100, m_seVol = 100, m_voiceVol = 100;

    void Start(tTJSNI_WaveSoundBuffer*& slot, const std::string& name, bool loop,
               int vol100);
    void Stop(tTJSNI_WaveSoundBuffer*& slot);
    static void ApplyVolume(tTJSNI_WaveSoundBuffer* sb, int vol100);
};

#endif // ONS_AUDIO_H
