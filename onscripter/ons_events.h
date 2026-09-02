/* -*- C++ -*-
 *  onscripter/ons_events.h
 *
 *  事件队列与时间/定时器（替代 SDL_Event / SDL_GetTicks / SDL_AddTimer 等）。
 *  事件结构字段与引擎原 SDL 用法一一对应，移植文件机械改名即可。
 */

#ifndef ONS_EVENTS_H
#define ONS_EVENTS_H

#include <cstdint>

// ---------------------------------------------------------------------------
// 事件类型
// ---------------------------------------------------------------------------
#define ONS_EVENT_FIRSTEVENT 0
#define ONS_EVENT_QUIT 0x100
#define ONS_EVENT_WINDOWEVENT 0x200
#define ONS_EVENT_KEYDOWN 0x300
#define ONS_EVENT_KEYUP 0x301
#define ONS_EVENT_MOUSEMOTION 0x400
#define ONS_EVENT_MOUSEBUTTONDOWN 0x401
#define ONS_EVENT_MOUSEBUTTONUP 0x402
#define ONS_EVENT_MOUSEWHEEL 0x403
#define ONS_EVENT_CONTROLLERDEVICEADDED 0x650
#define ONS_EVENT_CONTROLLERDEVICEREMOVED 0x651
#define ONS_EVENT_CONTROLLERBUTTONDOWN 0x652
#define ONS_EVENT_CONTROLLERBUTTONUP 0x653
#define ONS_EVENT_FINGERDOWN 0x700
#define ONS_EVENT_FINGERUP 0x701
#define ONS_EVENT_FINGERMOTION 0x702
#define ONS_EVENT_USEREVENT 0x8000
#define ONS_EVENT_LASTEVENT 0xFFFF

#define ONS_BUTTON_LEFT 1
#define ONS_BUTTON_MIDDLE 2
#define ONS_BUTTON_RIGHT 3

#define ONS_CONTROLLER_BUTTON_MAX 21

// ---------------------------------------------------------------------------
// 事件结构（与引擎用法对齐）
// ---------------------------------------------------------------------------

struct OnsKeysym
{
    int scancode;
    int sym;
    uint16_t mod;
    uint32_t unused;
};

struct OnsKeyboardEvent
{
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t state;
    uint8_t repeat;
    uint8_t padding2, padding3;
    OnsKeysym keysym;
};

struct OnsMouseMotionEvent
{
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint32_t state;
    int x, y;
    int xrel, yrel;
};

struct OnsMouseButtonEvent
{
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    uint8_t button;
    uint8_t state;
    uint8_t clicks;
    uint8_t padding1;
    int x, y;
};

struct OnsMouseWheelEvent
{
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint32_t which;
    int x, y;
    uint32_t direction;
};

struct OnsTouchFingerEvent
{
    uint32_t type;
    uint32_t timestamp;
    int64_t touchId;
    int64_t fingerId;
    float x, y;
    float dx, dy;
    float pressure;
};

struct OnsControllerButtonEvent
{
    uint32_t type;
    uint32_t timestamp;
    int32_t which;
    uint8_t button;
    uint8_t state;
    uint8_t padding1, padding2;
};

struct OnsControllerDeviceEvent
{
    uint32_t type;
    uint32_t timestamp;
    int32_t which;
};

struct OnsUserEvent
{
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    int32_t code;
    void* data1;
    void* data2;
};

struct OnsEvent
{
    uint32_t type;
    uint32_t timestamp;
    union
    {
        struct
        {
            uint32_t windowID;
        } common;
        OnsKeyboardEvent key;
        OnsMouseMotionEvent motion;
        OnsMouseButtonEvent button;
        OnsMouseWheelEvent wheel;
        OnsTouchFingerEvent tfinger;
        OnsControllerButtonEvent cbutton;
        OnsControllerDeviceEvent cdevice;
        OnsUserEvent user;
    } u;
};

// ---------------------------------------------------------------------------
// 队列 API（VM 在插件 tick 中消费；主线程由 TJS 层事件转发喂入，
// 见 demo/startup.tjs 的 ONSLayer 事件与 onscripter.cpp 的 ONSEngine::on*）
// ---------------------------------------------------------------------------

typedef enum
{
    ONS_GETEVENT = 0,
    ONS_PEEKEVENT = 1
} OnsEventAction;

int OnsPollEvent(OnsEvent* event);
int OnsWaitEvent(OnsEvent* event);
int OnsPushEvent(OnsEvent* event);
int OnsPeepEvents(OnsEvent* events, int numevents, OnsEventAction action, uint32_t minType,
                  uint32_t maxType);

// 主线程喂入（ONSEngine 的 TJS 事件转发调用）
namespace ons_events
{
void FeedMouseButton(int down, int button, int x, int y);
void FeedMouseMotion(int x, int y);
void FeedMouseWheel(int dx, int dy);
void FeedKey(int down, int keycode);
void FeedQuit();
void Reset(); // 清空队列（引擎重启时）
} // namespace ons_events

// TJS 层事件键码（krkrsdl3 传 Windows VK 码）-> ONS 键码（SDL1.2 风格 ONSK_*）
int ONS_MapTJSKeyToOns(int vk);

// ---------------------------------------------------------------------------
// 时间 / 定时器
// ---------------------------------------------------------------------------

typedef uint32_t(*OnsTimerCallback)(uint32_t interval, void* param);
typedef int OnsTimerID;

uint32_t OnsGetTicks(void);
void OnsDelay(uint32_t ms);
OnsTimerID OnsAddTimer(uint32_t interval, OnsTimerCallback callback, void* param);
int OnsRemoveTimer(OnsTimerID id);

// 键盘码常量（与移植后引擎一致）
#define ONSK_UNKNOWN 0
#define ONSK_RETURN 13
#define ONSK_ESCAPE 27
#define ONSK_BACKSPACE 8
#define ONSK_TAB 9
#define ONSK_SPACE 32
#define ONSK_DELETE 127
#define ONSK_UP 273
#define ONSK_DOWN 274
#define ONSK_RIGHT 275
#define ONSK_LEFT 276
#define ONSK_INSERT 277
#define ONSK_HOME 278
#define ONSK_END 279
#define ONSK_PAGEUP 280
#define ONSK_PAGEDOWN 281
#define ONSK_F1 282
#define ONSK_F2 283
#define ONSK_F3 284
#define ONSK_F4 285
#define ONSK_F5 286
#define ONSK_F6 287
#define ONSK_F7 288
#define ONSK_F8 289
#define ONSK_F9 290
#define ONSK_F10 291
#define ONSK_F11 292
#define ONSK_F12 293
#define ONSK_LCTRL 224
#define ONSK_LSHIFT 225
#define ONSK_LALT 226
#define ONSK_RCTRL 227
#define ONSK_RSHIFT 228
#define ONSK_RALT 229
#define ONSK_KP_ENTER 271
#define ONSK_KP_0 256
#define ONSK_KP_1 257
#define ONSK_KP_2 258
#define ONSK_KP_3 259
#define ONSK_KP_4 260
#define ONSK_KP_5 261
#define ONSK_KP_6 262
#define ONSK_KP_7 263
#define ONSK_KP_8 264
#define ONSK_KP_9 265
#define ONSK_MENU 319

#endif // ONS_EVENTS_H
