/* -*- C++ -*-
 *  onscripter/ons_events.cpp
 *
 *  浜嬩欢闃熷垪锛堜簰鏂?鏉′欢鍙橀噺锛夈€佹椂闂淬€佸畾鏃跺櫒绾跨▼瀹炵幇銆? */

#include "ons_events.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>

// TJS 层事件键码（krkrsdl3 传入的是 Windows VK 码，见 sdl3_app.cpp
// TVPConvertKeyCodeToVKCode）-> ONS 键码（SDL1.2 风格，见 ons_events.h ONSK_*）。
// VK_RETURN/ESCAPE/SPACE/BACK/TAB 与 ONS 键码数值相同，可透传；方向键/F 键/修饰键不同。
int ONS_MapTJSKeyToOns(int vk)
{
    switch (vk)
    {
        case 0x0d: return ONSK_RETURN;     // VK_RETURN
        case 0x1b: return ONSK_ESCAPE;     // VK_ESCAPE
        case 0x08: return ONSK_BACKSPACE;  // VK_BACK
        case 0x09: return ONSK_TAB;        // VK_TAB
        case 0x20: return ONSK_SPACE;      // VK_SPACE
        case 0x2e: return ONSK_DELETE;     // VK_DELETE -> 127
        case 0x25: return ONSK_LEFT;       // VK_LEFT 37 -> 276
        case 0x26: return ONSK_UP;         // 38 -> 273
        case 0x27: return ONSK_RIGHT;      // 39 -> 275
        case 0x28: return ONSK_DOWN;       // 40 -> 274
        case 0x2d: return ONSK_INSERT;     // 45 -> 277
        case 0x24: return ONSK_HOME;       // 36 -> 278
        case 0x23: return ONSK_END;        // 35 -> 279
        case 0x21: return ONSK_PAGEUP;     // 33 -> 280
        case 0x22: return ONSK_PAGEDOWN;   // 34 -> 281
        case 0x10: return ONSK_LSHIFT;     // VK_SHIFT（不分左右，取左）
        case 0x11: return ONSK_LCTRL;      // VK_CONTROL
        case 0x12: return ONSK_LALT;       // VK_MENU
        case 0x5d: return ONSK_MENU;       // VK_APPS -> 319
        case 0x60: return ONSK_KP_0;       // 小键盘 0..9 -> 256..265
        case 0x61: return ONSK_KP_1;
        case 0x62: return ONSK_KP_2;
        case 0x63: return ONSK_KP_3;
        case 0x64: return ONSK_KP_4;
        case 0x65: return ONSK_KP_5;
        case 0x66: return ONSK_KP_6;
        case 0x67: return ONSK_KP_7;
        case 0x68: return ONSK_KP_8;
        case 0x69: return ONSK_KP_9;
        default:
            if (vk >= 0x30 && vk <= 0x39) // VK_0..VK_9 == '0'..'9'，与 ONS 相同
                return vk;
            if (vk >= 0x41 && vk <= 0x5a) // VK_A..VK_Z(65..90) -> 'a'..'z'(97..122)
                return 'a' + (vk - 0x41);
            if (vk >= 0x70 && vk <= 0x7b) // VK_F1..F12(112..123) -> 282..293
                return ONSK_F1 + (vk - 0x70);
            return vk; // 兜底透传
    }
}

namespace
{
std::mutex g_EventMutex;
std::condition_variable g_EventCV;
std::deque<OnsEvent> g_EventQueue;
// 当前处于按下态的键（key repeat 过滤用）。OS/桌面会在按住期间重复送
// KEYDOWN（X11 ~30ms/次），若每个 repeat 都入队，按住几秒可堆积上百个
// 事件：VM 消费慢时队列越积越长，松开后的 KEYUP 排在队尾迟迟轮不到，
// Ctrl 快进"停不下来/延迟太久"。与上游一致（keydownEvent 对 repeat 只是
// 幂等重设状态位 + 状态覆盖式点击，无事件累积）：同键 down 未 up 时丢弃
// 后续 down，只保留首个 down 与随后的 up。
std::set<int> g_KeysDown;

bool OnsEventInRange(uint32_t type, uint32_t minType, uint32_t maxType)
{
    if (minType == ONS_EVENT_FIRSTEVENT && maxType == ONS_EVENT_LASTEVENT)
        return true;
    return type >= minType && type <= maxType;
}

struct OnsTimer
{
    int id;
    uint32_t interval;
    OnsTimerCallback cb;
    void* param;
    std::atomic<bool> cancel{false};
    std::thread th;
};

std::mutex g_TimerMutex;
std::map<int, OnsTimer*> g_Timers;
int g_NextTimerId = 1;

void OnsTimerThreadProc(OnsTimer* t)
{
    while (!t->cancel.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(t->interval));
        if (t->cancel.load())
            break;
        uint32_t next = t->cb(t->interval, t->param);
        if (next == 0)
            break;
        t->interval = next;
    }
}
} // namespace

int OnsPollEvent(OnsEvent* event)
{
    std::lock_guard<std::mutex> lk(g_EventMutex);
    if (g_EventQueue.empty())
        return 0;
    if (event)
        *event = g_EventQueue.front();
    g_EventQueue.pop_front();
    return 1;
}

int OnsWaitEvent(OnsEvent* event)
{
    std::unique_lock<std::mutex> lk(g_EventMutex);
    g_EventCV.wait(lk, [] { return !g_EventQueue.empty(); });
    if (event)
        *event = g_EventQueue.front();
    g_EventQueue.pop_front();
    return 1;
}

int OnsPushEvent(OnsEvent* event)
{
    if (!event)
        return -1;
    std::lock_guard<std::mutex> lk(g_EventMutex);
    g_EventQueue.push_back(*event);
    g_EventCV.notify_all();
    return 1;
}

int OnsPeepEvents(OnsEvent* events, int numevents, OnsEventAction action, uint32_t minType,
                  uint32_t maxType)
{
    std::lock_guard<std::mutex> lk(g_EventMutex);
    int n = 0;
    for (auto it = g_EventQueue.begin(); it != g_EventQueue.end() && n < numevents;)
    {
        if (!OnsEventInRange(it->type, minType, maxType))
        {
            ++it;
            continue;
        }
        if (action == ONS_GETEVENT)
        {
            if (events)
                events[n] = *it;
            it = g_EventQueue.erase(it);
        }
        else
        {
            if (events)
                events[n] = *it;
            ++it;
        }
        n++;
    }
    return n;
}

namespace ons_events
{

void FeedMouseButton(int down, int button, int x, int y)
{
    OnsEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = down ? ONS_EVENT_MOUSEBUTTONDOWN : ONS_EVENT_MOUSEBUTTONUP;
    ev.timestamp = OnsGetTicks();
    ev.u.button.button = (uint8_t)button;
    ev.u.button.state = (uint8_t)(down ? 1 : 0);
    ev.u.button.clicks = 1;
    ev.u.button.x = x;
    ev.u.button.y = y;
    OnsPushEvent(&ev);
}

void FeedMouseMotion(int x, int y)
{
    OnsEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = ONS_EVENT_MOUSEMOTION;
    ev.timestamp = OnsGetTicks();
    ev.u.motion.x = x;
    ev.u.motion.y = y;
    OnsPushEvent(&ev);
}

void FeedMouseWheel(int dx, int dy)
{
    OnsEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = ONS_EVENT_MOUSEWHEEL;
    ev.timestamp = OnsGetTicks();
    ev.u.wheel.x = dx;
    ev.u.wheel.y = dy;
    OnsPushEvent(&ev);
}

void FeedKey(int down, int keycode)
{
    {
        std::lock_guard<std::mutex> lk(g_EventMutex);
        if (down)
        {
            if (keycode == 0 || g_KeysDown.count(keycode))
                return; // unknown 或 key repeat：丢弃（首个 down 已入队）
            g_KeysDown.insert(keycode);
        }
        else
        {
            if (!g_KeysDown.erase(keycode))
                return; // 无对应 down 的 up：丢弃
        }
    }
    OnsEvent ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = down ? ONS_EVENT_KEYDOWN : ONS_EVENT_KEYUP;
    ev.timestamp = OnsGetTicks();
    ev.u.key.state = (uint8_t)(down ? 1 : 0);
    ev.u.key.keysym.sym = keycode;
    ev.u.key.keysym.scancode = keycode;
    OnsPushEvent(&ev);
}

void FeedQuit()
{
    OnsEvent ev;
    ev.timestamp = OnsGetTicks();
    std::memset(&ev, 0, sizeof(ev));
    ev.type = ONS_EVENT_QUIT;
    OnsPushEvent(&ev);
}

void Reset()
{
    std::lock_guard<std::mutex> lk(g_EventMutex);
    g_EventQueue.clear();
    g_KeysDown.clear(); // 键状态随队列作废（引擎重启）
}

} // namespace ons_events

// ---------------------------------------------------------------------------
// 鏃堕棿 / 瀹氭椂鍣?// ---------------------------------------------------------------------------

uint32_t OnsGetTicks(void)
{
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void OnsDelay(uint32_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

OnsTimerID OnsAddTimer(uint32_t interval, OnsTimerCallback callback, void* param)
{
    OnsTimer* t = new OnsTimer;
    t->id = g_NextTimerId++;
    t->interval = interval;
    t->cb = callback;
    t->param = param;
    t->th = std::thread(OnsTimerThreadProc, t);
    {
        std::lock_guard<std::mutex> lk(g_TimerMutex);
        g_Timers[t->id] = t;
    }
    return t->id;
}

int OnsRemoveTimer(OnsTimerID id)
{
    OnsTimer* t = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_TimerMutex);
        auto it = g_Timers.find(id);
        if (it == g_Timers.end())
            return 0;
        t = it->second;
        g_Timers.erase(it);
    }
    t->cancel.store(true);
    if (t->th.joinable())
        t->th.join();
    delete t;
    return 1;
}

