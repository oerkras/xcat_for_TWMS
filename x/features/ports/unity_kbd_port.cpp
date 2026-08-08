// Classic TWMS — Keyboard 设备状态注入（内部输入真源）。
// RVA/布局取自运行期 dump（Dumps/runtime/out/dump.cs.restored · remount 2026-08-06）：
//   InputSystem.QueueEvent(InputEventPtr)      @0x46F9C30
//   Keyboard.get_current()                     @0x4755120
//   InputSystem.get_settings()                 @0x46FA2E0
//   InputSettings.set_backgroundBehavior(e)    @0x47786B0
//   InputSystem.EnableDevice(InputDevice)      @0x46F8920
//   StateEvent: baseEvent@0x00(20B) stateFormat@0x14 stateData@0x18
//   KeyboardState: 'KEYS' · 16B 位图 · leftArrow=bit61 rightArrow=bit62
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "unity_kbd_port.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace x::features::ports::unity_kbd {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;

constexpr uint32_t kRvaQueueEvent = 0x46F9C30;
constexpr uint32_t kRvaKeyboardGetCurrent = 0x4755120;
constexpr uint32_t kRvaGetSettings = 0x46FA2E0;
constexpr uint32_t kRvaSetBackgroundBehavior = 0x47786B0;
constexpr uint32_t kRvaEnableDevice = 0x46F8920;
// InputControl.get_currentStatePtr() → InputStateBuffers.GetFrontBufferForDevice(deviceIndex)@0x47CDC30
// 已在运行期 IDB（imagebase 0x7ff848c80000 → VA 0x7FF84D37D900）反汇编核实。
constexpr uint32_t kRvaGetCurrentStatePtr = 0x46FD900;
// Keyboard.IEventPreProcessor.PreProcessEvent(InputEventPtr) —— 每个键盘事件落到设备前的收口点。
// VA 0x7FF84D3D7020 反编译已核实：判 type=='STAT'（1398030676）→ 判 *(u32*)(ev+0x14)=='KEYS'
// → 拿 ev+0x18 当位图**原地改写**（Unity 自己把 bit111 挪到 bit127），恒返回 1。
// 这既确认了它在必经之路上，也确认了 +0x14/+0x18 与本文件 KeyboardStateEvent 的布局一致。
constexpr uint32_t kRvaKeyboardPreProcess = 0x4757020;
// InputManager 的事件循环按设备标志位 DeviceFlags.HasEventPreProcessor(0x4000) 决定要不要调
// pre-processor。这位是 0 时，钩子装得再对也永远不会被调到 —— 必须实读，必要时置上。
constexpr uint32_t kRvaGetHasPreProc = 0x4709030;
constexpr uint32_t kRvaSetHasPreProc = 0x4709040;

// InputDevice 字段（dump.cs · InputDevice : InputControl）
constexpr size_t kOffDeviceId = 0xE4;          // m_DeviceId
constexpr size_t kOffStateByteOffset = 0x14;   // m_StateBlock@0x10 + m_ByteOffset@0x04
constexpr size_t kOffLastUpdateTime = 0x130;   // m_LastUpdateTimeInternal

constexpr uint32_t kFourCcStateEvent = 0x53544154u;  // 'STAT'
constexpr uint32_t kFourCcKeyboard = 0x4B455953u;    // 'KEYS'
constexpr int kKeyboardStateBytes = 16;              // KeyboardState.kSizeInBytes
constexpr int kMaxKeyBit = kKeyboardStateBytes * 8;

// InputSettings.BackgroundBehavior.IgnoreFocus。默认失焦会 Reset+Disable 键盘设备，
// 那样注入的状态会被清掉、设备禁用后事件直接丢弃 —— 后台走位必须先关掉这个行为。
constexpr int32_t kBackgroundIgnoreFocus = 2;

#pragma pack(push, 1)
struct KeyboardStateEvent {
    uint32_t type;         // 0x00 'STAT'
    uint16_t sizeInBytes;  // 0x04
    uint16_t deviceId;     // 0x06
    double time;           // 0x08 internalTime
    int32_t eventId;       // 0x10
    uint32_t stateFormat;  // 0x14 'KEYS'
    uint8_t keys[kKeyboardStateBytes];  // 0x18
};
#pragma pack(pop)
static_assert(sizeof(KeyboardStateEvent) == 40, "StateEvent<KeyboardState> = 20 + 4 + 16");

using FnQueueEvent = void(__fastcall*)(void* eventPtr, const void* mi);
using FnGetCurrent = void*(__fastcall*)(const void* mi);
using FnGetSettings = void*(__fastcall*)(const void* mi);
using FnSetBgBehavior = void(__fastcall*)(void* self, int32_t value, const void* mi);
using FnEnableDevice = void(__fastcall*)(void* device, const void* mi);
using FnGetStatePtr = void*(__fastcall*)(void* control, const void* mi);
using FnPreProcess = char(__fastcall*)(void* self, void* eventPtr, const void* mi);
using FnGetBool = char(__fastcall*)(void* self, const void* mi);
using FnSetBool = void(__fastcall*)(void* self, char value, const void* mi);

// 历史：守位/直写曾只碰箭头。现按 gMask 全位（含 PageDown/技能键脉冲），玩家未持有的位不动。
uint8_t gPrevDirectMask[kKeyboardStateBytes]{};

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

bool gBindOk = false;
DWORD gBindNextTryMs = 0;
const char* gFail = "unbound";

void* gKeyboardKlass = nullptr;
void* gInputSystemKlass = nullptr;
void* gSettingsKlass = nullptr;
void* gControlKlass = nullptr;

MethodInfoHead* gMiQueueEvent = nullptr;
MethodInfoHead* gMiGetCurrent = nullptr;
MethodInfoHead* gMiGetSettings = nullptr;
MethodInfoHead* gMiSetBg = nullptr;
MethodInfoHead* gMiEnableDevice = nullptr;
MethodInfoHead* gMiGetStatePtr = nullptr;

uint8_t gMask[kKeyboardStateBytes]{};
double gLastSentTime = 0.0;
bool gBgApplied = false;
uint32_t gBgTries = 0;
uint32_t gQueued = 0;
uint32_t gForeign = 0;
uint32_t gDirect = 0;
uint32_t gGuarded = 0;
bool gDirectWrite = false;  // XCAT_KBD_DIRECT=1 才开；见 RepushOnMain 注释
bool gInputTickOwned = false;  // 本模块已挂 InputFrameTick（掩码非空自管）

std::atomic<FnPreProcess> gOrigPreProcess{nullptr};
void** gGuardSlots[4]{};
int gGuardSlotCount = 0;
uint32_t gHookCalls = 0;      // 钩子被调到的次数：与 guards 一起才能分开「没被调」和「被调但没改」
uint32_t gDiagLogged = 0;     // 事件结构诊断的一次性打印计数（最多 4 条）
int gEvIndirect = -1;         // -1 未定 / 0 直接是 InputEvent* / 1 需再解一层
bool gPreProcFlagDone = false;

// 事件时间必须严格递增：InputManager 处理状态事件时会丢弃早于
// device.m_LastUpdateTimeInternal 的乱序事件。取「设备上次更新时间 + ε」既不会被判乱序，
// 也不会落到未来被推迟到下一帧。
constexpr double kTimeEpsilon = 0.0009765625;  // 2^-10，二进制精确

template <typename Fn>
Fn FnOf(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return x::runtime::il2cpp::AtRva<Fn>(rva);
}

MethodInfoHead* MethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethodFromName) return nullptr;
    MethodInfoHead* mi = nullptr;
    __try {
        mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mi = nullptr;
    }
    return (mi && mi->methodPointer) ? mi : nullptr;
}

bool EnvOff(const char* name) {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return false;
    return buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' || buf[0] == 'f' || buf[0] == 'F';
}

bool EnvOn(const char* name) {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    if (!n || n >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

bool PatchVtableMethodPtr(void** slot, void* hook, void** outOrig) {
    if (!slot || !hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    void* prev = nullptr;
    __try {
        prev = *slot;
        *slot = hook;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(slot, sizeof(void*), old, &old);
        return false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    if (outOrig) *outOrig = prev;
    return true;
}

bool InstallEventGuard(void* dev);

bool Bind() {
    if (gBindOk) return true;
    // 首次可能早于 il2cpp domain 就绪；退避重试，别一次失败就永久熄火。
    const DWORD now = GetTickCount();
    if (gBindNextTryMs && static_cast<int>(now - gBindNextTryMs) < 0) return false;
    gBindNextTryMs = now + 2000;

    if (!x::runtime::il2cpp::Ensure()) {
        gFail = "il2cpp_unbound";
        return false;
    }
    gKeyboardKlass = x::runtime::il2cpp::FindClass("UnityEngine.InputSystem", "Keyboard");
    gInputSystemKlass = x::runtime::il2cpp::FindClass("UnityEngine.InputSystem", "InputSystem");
    gSettingsKlass = x::runtime::il2cpp::FindClass("UnityEngine.InputSystem", "InputSettings");
    if (!gKeyboardKlass || !gInputSystemKlass) {
        gFail = "klass_miss";
        return false;
    }
    x::runtime::il2cpp::RuntimeClassInit(gKeyboardKlass);
    x::runtime::il2cpp::RuntimeClassInit(gInputSystemKlass);

    // Unity 程序集未混淆，明文名可直取；缺失时回落裸 RVA。
    gMiGetCurrent = MethodByName(gKeyboardKlass, "get_current", 0);
    gMiQueueEvent = MethodByName(gInputSystemKlass, "QueueEvent", 1);
    gMiGetSettings = MethodByName(gInputSystemKlass, "get_settings", 0);
    gMiEnableDevice = MethodByName(gInputSystemKlass, "EnableDevice", 1);
    if (gSettingsKlass) gMiSetBg = MethodByName(gSettingsKlass, "set_backgroundBehavior", 1);
    gControlKlass = x::runtime::il2cpp::FindClass("UnityEngine.InputSystem", "InputControl");
    if (gControlKlass) gMiGetStatePtr = MethodByName(gControlKlass, "get_currentStatePtr", 0);

    gDirectWrite = EnvOn("XCAT_KBD_DIRECT");
    gBindOk = true;
    gFail = "ok";
    x::runtime::LogI("UnityKbd",
                     "bind ok kb=%p is=%p mi(cur=%p q=%p set=%p bg=%p en=%p sp=%p) direct=%d",
                     gKeyboardKlass, gInputSystemKlass, (void*)gMiGetCurrent, (void*)gMiQueueEvent,
                     (void*)gMiGetSettings, (void*)gMiSetBg, (void*)gMiEnableDevice,
                     (void*)gMiGetStatePtr, gDirectWrite ? 1 : 0);
    return true;
}

void* CurrentKeyboard() {
    auto fn = FnOf<FnGetCurrent>(gMiGetCurrent, kRvaKeyboardGetCurrent);
    if (!fn) return nullptr;
    void* dev = nullptr;
    __try {
        dev = fn(gMiGetCurrent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dev = nullptr;
    }
    return LooksLikeHeapPtr(dev) ? dev : nullptr;
}

// 失焦不重置/不禁用键盘设备，否则后台注入会被整体丢弃。XCAT_KBD_BG=0 可关。
void EnsureBackgroundBehavior(void* device) {
    if (gBgApplied) return;
    if (EnvOff("XCAT_KBD_BG")) {
        gBgApplied = true;
        x::runtime::LogI("UnityKbd", "backgroundBehavior keep default (XCAT_KBD_BG=0)");
        return;
    }
    auto getSettings = FnOf<FnGetSettings>(gMiGetSettings, kRvaGetSettings);
    auto setBg = FnOf<FnSetBgBehavior>(gMiSetBg, kRvaSetBackgroundBehavior);
    bool ok = false;
    if (getSettings && setBg) {
        __try {
            void* settings = getSettings(gMiGetSettings);
            if (LooksLikeHeapPtr(settings)) {
                setBg(settings, kBackgroundIgnoreFocus, gMiSetBg);
                ok = true;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    auto enableDev = FnOf<FnEnableDevice>(gMiEnableDevice, kRvaEnableDevice);
    if (enableDev && device) {
        __try {
            enableDev(device, gMiEnableDevice);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // 本函数每帧都会被 Repush 路过；失败也要封顶重试次数，否则日志会被刷爆。
    if (ok || ++gBgTries >= 3) gBgApplied = true;
    x::runtime::LogI("UnityKbd", "backgroundBehavior=IgnoreFocus %s (try=%u)", ok ? "ok" : "FAIL",
                     gBgTries);
}

// PreProcessEvent 只有在设备的 DeviceFlags.HasEventPreProcessor(0x4000) 置位时才会被
// InputManager 调用。Keyboard 实现了 IEventPreProcessor，这位本应为 1；实读一次并在为 0 时置上，
// 免得钩子装对了却永远进不来。置位本身是安全的：调用目标就是 Keyboard 自己的实现。
void EnsurePreProcFlag(void* dev) {
    if (gPreProcFlagDone || !dev) return;
    gPreProcFlagDone = true;
    void* devKlass = x::runtime::il2cpp::FindClass("UnityEngine.InputSystem", "InputDevice");
    MethodInfoHead* miGet = MethodByName(devKlass, "get_hasEventPreProcessor", 0);
    MethodInfoHead* miSet = MethodByName(devKlass, "set_hasEventPreProcessor", 1);
    auto get = FnOf<FnGetBool>(miGet, kRvaGetHasPreProc);
    auto set = FnOf<FnSetBool>(miSet, kRvaSetHasPreProc);
    int before = -1;
    int after = -1;
    __try {
        if (get) before = get(dev, miGet) ? 1 : 0;
        if (before == 0 && set) {
            set(dev, 1, miSet);
            if (get) after = get(dev, miGet) ? 1 : 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    x::runtime::LogI("UnityKbd", "hasEventPreProcessor before=%d after=%d (get=%p set=%p)", before,
                     after, reinterpret_cast<void*>(get), reinterpret_cast<void*>(set));
}

bool WriteStateDirect(void* dev);

bool PushState() {
    if (!Bind()) return false;
    void* dev = CurrentKeyboard();
    if (!dev) {
        gFail = "no_keyboard";
        return false;
    }
    EnsureBackgroundBehavior(dev);
    EnsurePreProcFlag(dev);
    // 守位钩子必须拿到设备实例才能取到派发用的 klass，故装在这里而非 Bind()。
    // 只在手里有键时才动手（见 GuardOwnedBits），空手期零开销。
    (void)InstallEventGuard(dev);

    auto queue = FnOf<FnQueueEvent>(gMiQueueEvent, kRvaQueueEvent);
    if (!queue) {
        gFail = "no_queue_fn";
        return false;
    }

    int32_t deviceId = 0;
    double lastUpdate = 0.0;
    __try {
        deviceId = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(dev) + kOffDeviceId);
        lastUpdate = *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(dev) + kOffLastUpdateTime);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFail = "dev_read_seh";
        return false;
    }
    if (deviceId <= 0) {
        gFail = "bad_device_id";
        return false;
    }
    if (!(lastUpdate > 0.0)) lastUpdate = 0.0;

    // 覆盖计量（零成本）：设备上次更新时间是我们自己写进去的那个值；一旦它更新，
    // 说明中途有**外来**键盘事件（前台真实 Raw Input）把整块状态覆盖过。
    // 这正是「前台一顿一顿」的元凶，用它可以直接量化补写有没有压住覆盖。
    if (gLastSentTime > 0.0 && lastUpdate > gLastSentTime) ++gForeign;

    double t = lastUpdate + kTimeEpsilon;
    if (t <= gLastSentTime) t = gLastSentTime + kTimeEpsilon;
    gLastSentTime = t;

    __declspec(align(8)) KeyboardStateEvent ev{};
    ev.type = kFourCcStateEvent;
    ev.sizeInBytes = static_cast<uint16_t>(sizeof(ev));
    ev.deviceId = static_cast<uint16_t>(deviceId);
    ev.time = t;
    ev.eventId = 0;
    ev.stateFormat = kFourCcKeyboard;
    memcpy(ev.keys, gMask, sizeof(gMask));

    __try {
        queue(&ev, gMiQueueEvent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFail = "queue_seh";
        return false;
    }
    ++gQueued;
    gFail = "ok";
    return true;
}

// 直写设备前台状态缓冲。QueueEvent 那条路是「排队等下一次 InputSystem.Update 处理」，
// 而前台每帧都有一条真实键盘事件排在同一队里 —— 谁在队尾谁赢，实测打成 1:1
// （02:41：clob≈31/s vs tick≈34/s，占空比卡在 54%）。
// 直写发生在 WM.FixedUpdate orig 之前、本帧事件队列早已在 EarlyUpdate 抽干之后，
// 是这一帧的最后一次落笔，游戏随后读到的必然是我们的值 —— 不再赌硬币。
bool WriteStateDirect(void* dev) {
    auto getPtr = FnOf<FnGetStatePtr>(gMiGetStatePtr, kRvaGetCurrentStatePtr);
    if (!getPtr) {
        gFail = "no_stateptr_fn";
        return false;
    }
    uint8_t* keys = nullptr;
    __try {
        auto* base = static_cast<uint8_t*>(getPtr(dev, gMiGetStatePtr));
        if (!base) return false;
        const uint32_t off =
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(dev) + kOffStateByteOffset);
        // 设备是自身状态块的根，byteOffset 应当极小；异常值说明布局对不上，宁可不写。
        if (off > 4096u) {
            gFail = "bad_state_offset";
            return false;
        }
        keys = base + off;
        for (int i = 0; i < kKeyboardStateBytes; ++i) {
            const uint8_t clearBits =
                static_cast<uint8_t>(gPrevDirectMask[i] & static_cast<uint8_t>(~gMask[i]));
            keys[i] = static_cast<uint8_t>((keys[i] & static_cast<uint8_t>(~clearBits)) | gMask[i]);
        }
        memcpy(gPrevDirectMask, gMask, sizeof(gMask));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        gFail = "state_write_seh";
        return false;
    }
    ++gDirect;
    gFail = "ok";
    return true;
}

bool MaskEmpty() {
    for (uint8_t b : gMask) {
        if (b) return false;
    }
    return true;
}

void InputRepushFrameTick(void*) { (void)RepushOnMain(); }

// 掩码非空 → 挂 InputFrameTick；变空 → 摘掉。走路不再独占该槽。
void SyncInputFrameTick() {
    const bool want = !MaskEmpty();
    if (want == gInputTickOwned) return;
    if (!x::runtime::main_thread::Ensure() && !x::runtime::main_thread::IsOnPumpThread()) {
        // 泵未就绪：下次 Push/Hold 再试；勿永久卡死。
        return;
    }
    if (want) {
        x::runtime::main_thread::SetInputFrameTick(&InputRepushFrameTick, nullptr);
        gInputTickOwned = true;
    } else {
        x::runtime::main_thread::SetInputFrameTick(nullptr, nullptr);
        gInputTickOwned = false;
    }
}

bool BitIsSet(int32_t bit) {
    if (bit <= 0 || bit >= kMaxKeyBit) return false;
    const size_t byteIdx = static_cast<size_t>(bit) / 8u;
    const uint8_t bitMask = static_cast<uint8_t>(1u << (static_cast<uint32_t>(bit) & 7u));
    return (gMask[byteIdx] & bitMask) != 0;
}

bool SetBit(int32_t bit, bool on) {
    if (bit <= 0 || bit >= kMaxKeyBit) return false;
    const size_t byteIdx = static_cast<size_t>(bit) / 8u;
    const uint8_t bitMask = static_cast<uint8_t>(1u << (static_cast<uint32_t>(bit) & 7u));
    if (on) {
        gMask[byteIdx] |= bitMask;
    } else {
        gMask[byteIdx] = static_cast<uint8_t>(gMask[byteIdx] & ~bitMask);
    }
    SyncInputFrameTick();
    return true;
}

// 把本模块正持有的键位（gMask）OR 回事件位图。
// 前台 Unity 原生输入后端每帧都发一条「整块键盘状态」事件，注入位常被写成 0；游戏那道门闩吃的是
// **状态变化**（change monitor / InputAction 回调），于是每条这种事件都触发一次 canceled。
// 在这里把位补回去，取消信号根本不会产生。只碰 gMask 里按下的位，玩家真按的其他键一律不动。
// `InputEventPtr` 是只含一个指针字段的结构。il2cpp 对这种结构可能按值传（拿到的直接就是
// `InputEvent*`），也可能按引用传（拿到的是结构地址，要再解一层）。原函数的反编译看不出是哪种，
// 所以用 'STAT' 这个 FourCC 当判据实测一次并记住，两种 ABI 都能吃。
uint8_t* ResolveEvent(void* evRaw) {
    auto* p = static_cast<uint8_t*>(evRaw);
    if (gEvIndirect != 1 && *reinterpret_cast<uint32_t*>(p) == kFourCcStateEvent) {
        gEvIndirect = 0;
        return p;
    }
    if (gEvIndirect != 0) {
        auto* q = *reinterpret_cast<uint8_t**>(p);
        if (q && *reinterpret_cast<uint32_t*>(q) == kFourCcStateEvent) {
            gEvIndirect = 1;
            return q;
        }
    }
    return nullptr;  // 非 'STAT'（如 TEXT/IME/DLTA），本模块不管
}

void GuardOwnedBits(void* evRaw) {
    if (!evRaw || MaskEmpty()) return;
    bool touched = false;
    __try {
        uint8_t* p = ResolveEvent(evRaw);
        if (!p) return;
        if (*reinterpret_cast<uint16_t*>(p + 4) < sizeof(KeyboardStateEvent)) return;
        if (*reinterpret_cast<uint32_t*>(p + 0x14) != kFourCcKeyboard) return;
        uint8_t* keys = p + 0x18;
        for (int i = 0; i < kKeyboardStateBytes; ++i) {
            const uint8_t want = gMask[i];
            if (!want) continue;
            const uint8_t before = keys[i];
            const uint8_t after = static_cast<uint8_t>(before | want);
            if (after == before) continue;
            keys[i] = after;
            touched = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    // 只统计真正改写过的次数：它等于「被拦下的 canceled 数」，前台应≈外来事件率，失焦应≈0。
    if (touched) ++gGuarded;
}

char __fastcall PreProcessEventHook(void* self, void* eventPtr, const void* mi) {
    ++gHookCalls;
    // 头几次把事件头的原始 dword 打出来：'STAT'(0x54415453) 出现在第一层说明结构按值传，
    // 出现在解一层之后说明按引用传。二者之外则是别的事件型别（TEXT/IME 等）。
    if (gDiagLogged < 4 && eventPtr) {
        ++gDiagLogged;
        uint32_t d0 = 0, d1 = 0, r0 = 0;
        __try {
            d0 = *reinterpret_cast<uint32_t*>(eventPtr);
            d1 = *(reinterpret_cast<uint32_t*>(eventPtr) + 1);
            auto* q = *reinterpret_cast<uint8_t**>(eventPtr);
            if (q) r0 = *reinterpret_cast<uint32_t*>(q);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        x::runtime::LogI("UnityKbd", "preproc ev=%p d0=%08X d1=%08X deref0=%08X (STAT=%08X)",
                         eventPtr, d0, d1, r0, kFourCcStateEvent);
    }
    FnPreProcess orig = gOrigPreProcess.load(std::memory_order_acquire);
    char r = 1;  // 原函数恒返回 1（接受）；取不到原函数时按接受放行，绝不吞事件。
    if (orig) {
        __try {
            r = orig(self, eventPtr, mi);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            r = 1;
        }
    }
    // 只处理 'STAT'：原函数自身也只认 'STAT'，说明键盘走的是整块状态事件而非 'DLTA' 增量。
    // 若日后发现 guard/s 压不住卡顿，DeltaStateEvent（stateOffset@0x1C / data@0x20）是第一嫌疑。
    if (r) GuardOwnedBits(eventPtr);
    return r;
}

// Il2CppClass 中与接口派发相关的布局，全部由 InputManager.OnUpdate 的派发序列反推（详见
// docs/features/protocol/MoveElem字段.md）：
//   mov rax,[r12] / movzx ecx,[rax+12Eh] / mov r8,[rax+0B0h] / cmp [r8-8],rdx
//   movsxd rcx,[r8] / shl rcx,4 / add rax,rcx / add rax,138h / call qword ptr [rax]
constexpr uint32_t kOffKlassIfaceOffsets = 0x0B0;  // Il2CppRuntimeInterfaceOffsetPair*，每项 16B
constexpr uint32_t kOffKlassIfaceCount = 0x12E;    // uint16
constexpr uint32_t kOffKlassVtable = 0x138;        // VirtualInvokeData[]，每项 16B {methodPtr,method}

// 按派发器的同一公式算槽地址：klass + 0x138 + interfaceOffset*16。
// 先前版本改为「在类对象里逐 qword 找等于原函数地址的格子」，实测踩坑：能扫到一个、
// 地址校验也对，但那不是派发真正读的那一格，钩子永不进入（hc=0）。这里不再靠碰运气。
void** ResolveIfaceSlot(void* klass, void* ifaceKlass) {
    if (!klass || !ifaceKlass) return nullptr;
    auto* k = static_cast<uint8_t*>(klass);
    __try {
        const uint16_t n = *reinterpret_cast<uint16_t*>(k + kOffKlassIfaceCount);
        auto* pairs = *reinterpret_cast<uint8_t**>(k + kOffKlassIfaceOffsets);
        if (!n || !pairs) return nullptr;
        for (uint16_t i = 0; i < n; ++i) {
            uint8_t* e = pairs + static_cast<size_t>(i) * 16;
            if (*reinterpret_cast<void**>(e) != ifaceKlass) continue;
            const int32_t off = *reinterpret_cast<int32_t*>(e + 8);
            if (off < 0 || off > 0x4000) return nullptr;
            // IEventPreProcessor 只声明 PreProcessEvent 一个方法，接口内槽位恒为 0
            return reinterpret_cast<void**>(k + kOffKlassVtable + static_cast<size_t>(off) * 16);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

// 用**设备实例自己的** klass（`*(void**)dev`），与派发器 `mov rax,[r12]` 完全一致；
// 不用 FindClass 的结果，免得两者不是同一个类对象却无从察觉。
bool InstallEventGuard(void* dev) {
    if (gGuardSlotCount != 0) return gGuardSlotCount > 0;  // -1 = 已显式停用，不再重试
    if (!dev) return false;
    if (EnvOff("XCAT_KBD_GUARD")) {
        x::runtime::LogI("UnityKbd", "event guard off (XCAT_KBD_GUARD=0)");
        gGuardSlotCount = -1;  // 明确停用，不再重试
        return false;
    }
    void* want = x::runtime::il2cpp::AtRva<void*>(kRvaKeyboardPreProcess);
    if (!want) return false;

    void* klass = nullptr;
    __try {
        klass = *reinterpret_cast<void**>(dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!LooksLikeHeapPtr(klass)) return false;

    void* ifaceKlass =
        x::runtime::il2cpp::FindClass("UnityEngine.InputSystem.LowLevel", "IEventPreProcessor");
    void** slot = ResolveIfaceSlot(klass, ifaceKlass);
    void* cur = nullptr;
    if (slot) {
        __try {
            cur = *slot;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            slot = nullptr;
        }
    }
    // 槽里必须正躺着 PreProcessEvent 本尊，否则说明布局或接口对象认错了，宁可整体不装。
    const bool match = slot && cur == want;
    x::runtime::LogI("UnityKbd",
                     "guard resolve devKlass=%p findKlass=%p iface=%p slot=%p off=%lld cur=%p "
                     "want=%p match=%d",
                     klass, gKeyboardKlass, ifaceKlass, (void*)slot,
                     slot ? (long long)(reinterpret_cast<uint8_t*>(slot) -
                                        static_cast<uint8_t*>(klass))
                          : -1LL,
                     cur, want, match ? 1 : 0);
    if (!match) {
        x::runtime::LogI("UnityKbd", "event guard MISS — 卡顿修复未生效");
        return false;
    }

    void* prev = nullptr;
    if (!PatchVtableMethodPtr(slot, reinterpret_cast<void*>(&PreProcessEventHook), &prev)) {
        x::runtime::LogI("UnityKbd", "event guard patch FAIL slot=%p", (void*)slot);
        return false;
    }
    // 回读自证：只看 PatchVtableMethodPtr 的返回值不够，写没落地照样报成功。
    void* after = nullptr;
    __try {
        after = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (after != reinterpret_cast<void*>(&PreProcessEventHook)) {
        x::runtime::LogI("UnityKbd", "event guard writeback MISMATCH slot=%p after=%p", (void*)slot,
                         after);
        return false;
    }
    gOrigPreProcess.store(reinterpret_cast<FnPreProcess>(prev), std::memory_order_release);
    gGuardSlots[gGuardSlotCount++] = slot;
    x::runtime::LogI("UnityKbd", "event guard on slot=%p orig=%p", (void*)slot, prev);
    return true;
}

}  // namespace

bool SetKeyHeldOnMain(int32_t unityKey, bool down) {
    if (down) {
        if (!SetBit(unityKey, true)) return false;
        return PushState();
    }
    // Up：未持有则 no-op，禁止入队全零态抹掉走路/玩家键。
    if (!BitIsSet(unityKey)) {
        gFail = "ok";
        return true;
    }
    if (!SetBit(unityKey, false)) return false;
    return PushState();
}

bool BeginHoldOnMain(int32_t unityKey) { return SetKeyHeldOnMain(unityKey, true); }

bool EndHoldOnMain(int32_t unityKey) { return SetKeyHeldOnMain(unityKey, false); }

bool AnyHeld() { return !MaskEmpty(); }

uint32_t HeldMaskHash() {
    uint32_t h = 2166136261u;
    for (uint8_t b : gMask) {
        h ^= b;
        h *= 16777619u;
    }
    return h;
}

bool SetWalkDirOnMain(int inputX) {
    const bool wasEmpty = MaskEmpty();
    SetBit(kKeyLeftArrow, inputX < 0);
    SetBit(kKeyRightArrow, inputX > 0);
    // 状态事件是「整块覆盖」：本模块空手时入队等于把玩家真按住的键也抹掉一帧。
    if (wasEmpty && MaskEmpty()) return true;
    return PushState();
}

bool ReleaseAllOnMain() {
    // 只松走路左右；保留脉冲位（PageDown/技能/StickUp），避免与 Inject 互抹。
    const bool hadL = BitIsSet(kKeyLeftArrow);
    const bool hadR = BitIsSet(kKeyRightArrow);
    if (!hadL && !hadR) return true;
    SetBit(kKeyLeftArrow, false);
    SetBit(kKeyRightArrow, false);
    // gPrevDirectMask：清掉左右对应位，免直写路径误清脉冲。
    {
        const int32_t bits[2] = {kKeyLeftArrow, kKeyRightArrow};
        for (int n = 0; n < 2; ++n) {
            const int32_t bit = bits[n];
            const size_t i = static_cast<size_t>(bit) / 8u;
            const uint8_t m = static_cast<uint8_t>(1u << (static_cast<uint32_t>(bit) & 7u));
            gPrevDirectMask[i] = static_cast<uint8_t>(gPrevDirectMask[i] & ~m);
        }
    }
    return PushState();
}

bool RepushOnMain() {
    if (!gBindOk || MaskEmpty()) return false;
    // 默认走事件注入。直写（XCAT_KBD_DIRECT=1）虽然能稳赢事件队列的排队竞争，
    // 但 02:53 实测把纯内部注入的移动率从 71.4% 打到 10.2% —— 说明游戏那道门闩不是
    // 轮询设备位图，而是吃**状态变化**（事件路径才会触发的 change monitor / InputAction）；
    // 直写把位提前设成 1，随后到达的事件因「状态没变」而不再触发门闩，等于自断触发。
    // 保留开关是为了不重编就能 A/B 复验，默认必须关。
    if (gDirectWrite) {
        void* dev = CurrentKeyboard();
        if (dev && WriteStateDirect(dev)) return true;
    }
    return PushState();
}

namespace {

constexpr DWORD kHoldPollMs = 16;
constexpr DWORD kHoldJobWaitMs = 800;

struct HoldKeyJob {
    int32_t key = 0;
    bool down = false;
    bool ok = false;
};

void HoldKeyJobOnMain(void* user) {
    auto* j = static_cast<HoldKeyJob*>(user);
    if (!j || j->key <= 0) return;
    j->ok = j->down ? BeginHoldOnMain(j->key) : EndHoldOnMain(j->key);
}

bool RunHoldKey(int32_t unityKey, bool down) {
    HoldKeyJob job{};
    job.key = unityKey;
    job.down = down;
    if (x::runtime::main_thread::IsOnPumpThread()) {
        HoldKeyJobOnMain(&job);
        return job.ok;
    }
    if (!x::runtime::main_thread::Ensure()) return false;
    if (!x::runtime::main_thread::InvokeAndWait(&HoldKeyJobOnMain, &job, kHoldJobWaitMs,
                                                 x::runtime::main_thread::JobPrio::High)) {
        return false;
    }
    return job.ok;
}

bool ForceEndHold(int32_t unityKey) {
    for (int i = 0; i < 3; ++i) {
        if (RunHoldKey(unityKey, false)) return true;
        Sleep(kHoldPollMs);
    }
    return false;
}

}  // namespace

bool HoldUntil(int32_t unityKey, DWORD minHoldMs, DWORD maxHoldMs, HoldUntilFn until, void* user,
               char* detail, size_t detailCap, DWORD afterUntilDrainMs) {
    if (detail && detailCap) detail[0] = '\0';
    if (unityKey <= 0) {
        if (detail && detailCap) snprintf(detail, detailCap, "fail:bad_key");
        return false;
    }
    if (!EnsureBound()) {
        if (detail && detailCap) snprintf(detail, detailCap, "fail:unbound");
        return false;
    }
    if (!x::runtime::main_thread::Ensure() && !x::runtime::main_thread::IsOnPumpThread()) {
        if (detail && detailCap) snprintf(detail, detailCap, "fail:no_pump");
        return false;
    }

    DWORD minMs = minHoldMs;
    DWORD maxMs = maxHoldMs;
    if (maxMs < minMs) maxMs = minMs;
    if (minMs < 1) minMs = 1;

    uint32_t pushes0 = 0, guards0 = 0;
    Stats(&pushes0, nullptr, nullptr, &guards0, nullptr);

    if (!RunHoldKey(unityKey, true)) {
        if (detail && detailCap)
            snprintf(detail, detailCap, "fail:down:%s", LastFail() ? LastFail() : "?");
        x::runtime::LogW("UnityKbd", "HoldUntil down fail key=%d why=%s", unityKey, LastFail());
        return false;
    }

    const DWORD t0 = GetTickCount();
    bool untilHit = false;
    bool untilBeforeMin = false;
    const char* reason = "timeout";

    // 主线程上勿 Sleep 整段（卡游戏）；最短脉冲后立刻松。
    if (x::runtime::main_thread::IsOnPumpThread()) {
        reason = "pump_inline";
    } else {
        while (static_cast<int>(GetTickCount() - t0) < static_cast<int>(maxMs)) {
            const DWORD now = GetTickCount();
            const DWORD elapsed = now - t0;
            const bool stop = until && until(user);
            if (stop) {
                untilHit = true;
                untilBeforeMin = elapsed < minMs;
                // drain：至少撑满 minHold；until 后再额外 afterUntilDrainMs（Travel 进门）。
                DWORD drainUntil = t0 + minMs;
                const DWORD postUntil = now + afterUntilDrainMs;
                if (postUntil > drainUntil) drainUntil = postUntil;
                while (GetTickCount() < drainUntil) {
                    Sleep(kHoldPollMs);
                }
                reason = untilBeforeMin ? "until_drain" : "until";
                break;
            }
            if (elapsed >= maxMs) {
                reason = "timeout";
                break;
            }
            Sleep(kHoldPollMs);
        }
        if (!untilHit) reason = "timeout";
    }

    const DWORD heldMs = GetTickCount() - t0;
    const bool upOk = ForceEndHold(unityKey);

    uint32_t pushes1 = 0, guards1 = 0;
    Stats(&pushes1, nullptr, nullptr, &guards1, nullptr);
    x::runtime::LogI("UnityKbd",
                     "HoldUntil key=%d held=%ums reason=%s until=%d drain=%d up=%d "
                     "dPush=%u dGuard=%u guardOn=%d tick=%u host=%u",
                     unityKey, static_cast<unsigned>(heldMs), reason, untilHit ? 1 : 0,
                     untilBeforeMin ? 1 : 0, upOk ? 1 : 0, pushes1 - pushes0, guards1 - guards0,
                     GuardActive() ? 1 : 0,
                     static_cast<unsigned>(x::runtime::main_thread::InputFrameTickRuns()),
                     static_cast<unsigned>(x::runtime::main_thread::InputFrameTickHost()));

    if (detail && detailCap) {
        snprintf(detail, detailCap, "%s held=%ums up=%d", reason, static_cast<unsigned>(heldMs),
                 upOk ? 1 : 0);
    }
    return upOk;
}

void Stats(uint32_t* pushes, uint32_t* clobbers, uint32_t* directs, uint32_t* guards,
           uint32_t* hookCalls) {
    if (pushes) *pushes = gQueued;
    if (clobbers) *clobbers = gForeign;
    if (directs) *directs = gDirect;
    if (guards) *guards = gGuarded;
    if (hookCalls) *hookCalls = gHookCalls;
}

bool GuardActive() { return gGuardSlotCount > 0; }

bool Ready() { return gBindOk; }

bool EnsureBound() { return Bind(); }

const char* LastFail() { return gFail; }

void Shutdown() {
    // 必须还原：vtable 槽指向本 DLL 内的 PreProcessEventHook，卸载后游戏下一个键盘事件
    // 就会调进已释放内存。先摘钩再松键，顺序不能反。
    if (gInputTickOwned) {
        x::runtime::main_thread::SetInputFrameTick(nullptr, nullptr);
        gInputTickOwned = false;
    }
    FnPreProcess orig = gOrigPreProcess.exchange(nullptr, std::memory_order_acq_rel);
    if (orig) {
        for (int i = 0; i < gGuardSlotCount; ++i) {
            void** slot = gGuardSlots[i];
            if (!slot) continue;
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
            __try {
                // 只在槽里仍是我们的钩子时还原，避免踩掉别人后装的钩。
                if (*slot == reinterpret_cast<void*>(&PreProcessEventHook)) {
                    *slot = reinterpret_cast<void*>(orig);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
        x::runtime::LogI("UnityKbd", "event guard off (restored %d slot(s))", gGuardSlotCount);
    }
    for (auto*& s : gGuardSlots) s = nullptr;
    gGuardSlotCount = 0;
    memset(gMask, 0, sizeof(gMask));
    memset(gPrevDirectMask, 0, sizeof(gPrevDirectMask));
}

}  // namespace x::features::ports::unity_kbd
