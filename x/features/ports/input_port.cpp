// Classic TWMS — UserLocal.OnKey via shared main_thread_pump.
// KeyDownTouch/Up CFF 实机 SEH → 改走 OnKey @0x10181E0（MI=null）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "input_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"

#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"

#include <Psapi.h>
#include <atomic>
#include <cstring>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::input {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaOnKey = 0x10181E0;  // remapped 2026-08-03 · UserLocal.(KeyInputType,Key)
constexpr uint32_t kRvaIsFocusedInputField = 0x1663BE0;  // remapped 2026-08-03
constexpr int32_t kKeyInputDown = 0;
constexpr int32_t kKeyInputUp = 1;

// Game InputManager（非 UnityEngine）。旧哈希 a9487a00… 在 2026-08-03 dump 已不存在。
constexpr char kInputManagerClass[] =
    "c829ef06e8b802f06e5832fb7d75ff5299842cfec9b6600d4381a22c5a7219d";
constexpr char kHashOnKey[] =
    "df22d8fb6a481027c0a485d792f41d5c686ecf61662856f6994e18f8b19a309";
constexpr char kHashIsFocused[] =
    "cff33226e7716ffa85778a5ac60b94989da40969d23c8708d751798e7e1cc75";

constexpr size_t kOffTargetUser = 0x20;  // UserLocal backing field，仍 @0x20
constexpr DWORD kRebindMs = 3000;
constexpr DWORD kJobWaitMs = 1500;
constexpr DWORD kSehCooldownMs = 5000;
constexpr size_t kReleaseSlots = 16;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnClassStaticData = void* (*)(void* klass);
using FnOnKey = void (*)(void* self, int32_t inputType, int32_t key, const void* methodInfo);
using FnIsFocused = bool (*)(void* self, const void* methodInfo);

enum class JobKind : int { None = 0, Down = 1, Up = 2 };

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassParent gClassParent = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;
FnClassStaticData gClassStaticData = nullptr;

void* gImType = nullptr;
void* gImKlass = nullptr;
void* gInputManager = nullptr;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};
MethodInfoHead* gMiOnKey = nullptr;
MethodInfoHead* gMiIsFocused = nullptr;

DWORD gLastRebind = 0;
std::atomic<DWORD> gSehCooldownUntil{0};

std::atomic<uint64_t> gSubmitSeq{0};
std::atomic<uint64_t> gDoneSeq{0};
std::atomic<bool> gJobOk{false};
std::atomic<bool> gJobFocusedSkip{false};
JobKind gJobKind = JobKind::None;
int32_t gJobKey = 0;
uint64_t gJobSeq = 0;
CRITICAL_SECTION gJobCs{};
bool gJobCsInit = false;

struct ReleaseSlot {
    WORD vk = 0;
    int32_t key = 0;
    DWORD dueAt = 0;
    bool active = false;
};
ReleaseSlot gReleases[kReleaseSlots]{};
CRITICAL_SECTION gRelCs{};
bool gRelCsInit = false;

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
    }
    if (mi && mi->methodPointer) return mi;
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* raw = e.classGetMethods(cur, &iter);
                if (!raw) break;
                const char* nm = e.methodGetName(raw);
                if (nm && strcmp(nm, name) == 0) {
                    mi = reinterpret_cast<MethodInfoHead*>(raw);
                    if (mi && mi->methodPointer) return mi;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (plain) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plain, shape.arity)) return mi;
    }
    if (hash) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hash, shape.arity)) return mi;
    }
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("InputPort", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plain ? plain : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return nullptr;
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

void EnsureMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (ul && !gMiOnKey) {
        // void(KeyInputType,Key) — UL 上唯一-ish；哈希主
        constexpr MethodShape kOn{2, TypeKind::Void, true, true, {TypeKind::Any, TypeKind::Any}};
        gMiOnKey = ResolveMi(ul, kRvaOnKey, kOn, "OnKey", kHashOnKey);
    }
    if (gImKlass && !gMiIsFocused) {
        constexpr MethodShape kFo{0, TypeKind::Bool, false, true, {}};
        gMiIsFocused =
            ResolveMi(gImKlass, kRvaIsFocusedInputField, kFo, "IsFocusedInputField", kHashIsFocused);
    }
    x::runtime::anchor_lamps::Set(
        "InputOnKey",
        gMiOnKey ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                 : x::runtime::anchor_lamps::AnchorLampCode::Degraded,
        gMiOnKey ? "OnKey MI" : "RVA+pump");
}

void EnsureCs() {
    if (!gJobCsInit) {
        InitializeCriticalSection(&gJobCs);
        gJobCsInit = true;
    }
    if (!gRelCsInit) {
        InitializeCriticalSection(&gRelCs);
        gRelCsInit = true;
    }
}

void* FindClass(const char* ns, const char* name) {
    return x::runtime::il2cpp::FindClass(ns, name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

void* TryLazyValue(void* lazy) {
    if (!lazy || !LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    if (gClassStaticData) {
        __try {
            void* p = gClassStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

bool BindApis() {
    if (gGA && gFindAll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gClassParent = e.classParent;
    gRuntimeClassInit = e.runtimeClassInit;
    gClassStaticData = e.classStaticData;
    return gFindAll != nullptr;
}

void* TryResolveSingleton() {
    if (!gImKlass) gImKlass = FindClass("", kInputManagerClass);
    if (!gImKlass) return nullptr;

    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gImKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* staticsKlass = gImKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gImKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) {
                __try {
                    gRuntimeClassInit(parent);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            staticsKlass = parent;
        }
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gImKlass);
    if (!statics) return nullptr;

    for (size_t s = 0; s < 4; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeHeapPtr(cand)) continue;
        if (LooksLikeHeapPtr(ReadPtr(cand, kOffTargetUser))) return cand;
        if (!gInputManager) return cand;
    }
    return nullptr;
}

void* TryResolveFindAll() {
    if (!gImType) gImType = FindClassTypeObject(kInputManagerClass);
    if (!gImType || !gFindAll) return nullptr;
    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gImType, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    const uintptr_t n = ArrayLen(arr);
    void* best = nullptr;
    for (uintptr_t i = 0; i < n && i < 8; ++i) {
        void* im = ArrayAt(arr, i);
        if (!LooksLikeHeapPtr(im)) continue;
        if (LooksLikeHeapPtr(ReadPtr(im, kOffTargetUser))) return im;
        if (!best) best = im;
    }
    return best;
}

bool ResolveInputManager(DWORD now) {
    if (static_cast<int>(now - gSehCooldownUntil.load(std::memory_order_acquire)) < 0) {
        return false;
    }
    if (gInputManager && LooksLikeHeapPtr(gInputManager)) return true;
    if (gLastRebind && now - gLastRebind < kRebindMs) return gInputManager != nullptr;
    gLastRebind = now;
    if (!BindApis()) return false;

    // RuntimeClassInit / Singleton 读托管静态字段必须在主线程，否则 GC unknown thread。
    struct Ctx {
        bool ok = false;
        const char* how = "?";
    } ctx;
    auto job = [](void* user) {
        auto* c = reinterpret_cast<Ctx*>(user);
        if (!gImKlass) gImKlass = FindClass("", kInputManagerClass);
        void* best = TryResolveSingleton();
        c->how = "singleton";
        if (!best) {
            // FindAll 直调（已在主线程）；禁止再套 managed_main::FindAll
            if (!gImType) {
                void* klass = gImKlass ? gImKlass : FindClass("", kInputManagerClass);
                gImType = x::runtime::il2cpp::ClassTypeObjectOnMain(klass);
            }
            if (gImType && gFindAll) {
                void* arr = nullptr;
                __try {
                    arr = gFindAll(gImType, nullptr);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    arr = nullptr;
                }
                const uintptr_t n = ArrayLen(arr);
                void* pick = nullptr;
                for (uintptr_t i = 0; i < n && i < 8; ++i) {
                    void* im = ArrayAt(arr, i);
                    if (!LooksLikeHeapPtr(im)) continue;
                    if (LooksLikeHeapPtr(ReadPtr(im, kOffTargetUser))) {
                        pick = im;
                        break;
                    }
                    if (!pick) pick = im;
                }
                best = pick;
                c->how = "FindAll";
            }
        }
        gInputManager = best;
        c->ok = best != nullptr;
    };
    if (!x::runtime::managed_main::Call(+job, &ctx, 2000)) {
        x::runtime::LogWThrottled(42, 5000, "InputPort", "InputManager resolve main pump fail");
        return false;
    }
    if (gInputManager) {
        x::runtime::LogI("InputPort",
                         "InputManager bind %p via %s targetUser=%d (KeyTouch MI=null)",
                         gInputManager, ctx.how,
                         LooksLikeHeapPtr(ReadPtr(gInputManager, kOffTargetUser)) ? 1 : 0);
    }
    return ctx.ok;
}

bool IsFocusedOnMain() {
    if (!gInputManager || !gGA) return false;
    EnsureMethodInfos();
    auto fn = FnFromMi<FnIsFocused>(gMiIsFocused, kRvaIsFocusedInputField);
    if (!fn) return false;
    bool focused = false;
    __try {
        focused = fn(gInputManager, gMiIsFocused);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        focused = false;
    }
    return focused;
}

bool HasTargetUser() {
    return LooksLikeHeapPtr(ReadPtr(gInputManager, kOffTargetUser));
}

void RunJobOnMain(void* /*user*/) {
    EnsureCs();
    EnterCriticalSection(&gJobCs);
    const JobKind kind = gJobKind;
    const int32_t key = gJobKey;
    const uint64_t seq = gJobSeq;
    LeaveCriticalSection(&gJobCs);

    bool ok = false;
    bool focusedSkip = false;
    __try {
        EnsureMethodInfos();
        void* lu = HasTargetUser() ? ReadPtr(gInputManager, kOffTargetUser) : nullptr;
        if (!gInputManager || !LooksLikeHeapPtr(lu) || key <= 0) {
            ok = false;
        } else if (kind == JobKind::Down || kind == JobKind::Up) {
            if (kind == JobKind::Down && IsFocusedOnMain()) {
                focusedSkip = true;
                ok = false;
            } else {
                auto fn = FnFromMi<FnOnKey>(gMiOnKey, kRvaOnKey);
                if (fn) {
                    const int32_t inputType =
                        (kind == JobKind::Down) ? kKeyInputDown : kKeyInputUp;
                    fn(lu, inputType, key, gMiOnKey);
                    ok = true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        gInputManager = nullptr;
        gSehCooldownUntil.store(GetTickCount() + 800, std::memory_order_release);
        EnterCriticalSection(&gRelCs);
        for (auto& slot : gReleases) slot = {};
        LeaveCriticalSection(&gRelCs);
        x::runtime::LogW("InputPort", "OnKey SEH kind=%d key=%d → IM cleared cooldown=800ms",
                         (int)kind, (int)key);
    }

    if (gSubmitSeq.load(std::memory_order_acquire) == seq) {
        gJobOk.store(ok, std::memory_order_release);
        gJobFocusedSkip.store(focusedSkip, std::memory_order_release);
        gDoneSeq.store(seq, std::memory_order_release);
    }
}

bool EnqueueAndWait(JobKind kind, int32_t key, bool* outFocusedSkip) {
    if (outFocusedSkip) *outFocusedSkip = false;
    EnsureCs();
    const uint64_t seq = gSubmitSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    EnterCriticalSection(&gJobCs);
    gJobKind = kind;
    gJobKey = key;
    gJobSeq = seq;
    LeaveCriticalSection(&gJobCs);
    gJobOk.store(false, std::memory_order_release);
    gJobFocusedSkip.store(false, std::memory_order_release);

    if (!x::runtime::main_thread::InvokeAndWait(&RunJobOnMain, nullptr, kJobWaitMs)) {
        gSubmitSeq.fetch_add(1, std::memory_order_acq_rel);
        x::runtime::LogW("InputPort", "job pump fail/timeout kind=%d key=%d seq=%llu", (int)kind,
                         (int)key, (unsigned long long)seq);
        return false;
    }
    if (gDoneSeq.load(std::memory_order_acquire) != seq) {
        x::runtime::LogW("InputPort", "job seq mismatch kind=%d key=%d", (int)kind, (int)key);
        return false;
    }
    if (outFocusedSkip) *outFocusedSkip = gJobFocusedSkip.load(std::memory_order_acquire);
    return gJobOk.load(std::memory_order_acquire);
}

bool ExtendHoldIfActive(WORD vk, int32_t key, DWORD dueAt) {
    EnsureCs();
    EnterCriticalSection(&gRelCs);
    bool extended = false;
    for (auto& slot : gReleases) {
        if (!slot.active || slot.vk != vk) continue;
        slot.key = key;
        if (static_cast<int>(dueAt - slot.dueAt) > 0) slot.dueAt = dueAt;
        extended = true;
        break;
    }
    LeaveCriticalSection(&gRelCs);
    return extended;
}

bool ScheduleRelease(WORD vk, int32_t key, DWORD dueAt) {
    EnsureCs();
    EnterCriticalSection(&gRelCs);
    bool ok = false;
    for (auto& slot : gReleases) {
        if (slot.active && slot.vk == vk) {
            slot.key = key;
            slot.dueAt = dueAt;
            ok = true;
            break;
        }
    }
    if (!ok) {
        for (auto& slot : gReleases) {
            if (slot.active) continue;
            slot.vk = vk;
            slot.key = key;
            slot.dueAt = dueAt;
            slot.active = true;
            ok = true;
            break;
        }
    }
    LeaveCriticalSection(&gRelCs);
    if (!ok) x::runtime::LogW("InputPort", "release queue full vk=0x%02X", (unsigned)vk);
    return ok;
}

}  // namespace

int32_t VkToUnityKey(WORD vk) {
    if (vk >= '0' && vk <= '9') {
        if (vk == '0') return 50;
        return 40 + (vk - '0');
    }
    if (vk >= 'A' && vk <= 'Z') return 15 + (vk - 'A');
    if (vk >= 'a' && vk <= 'z') return 15 + (vk - 'a');
    if (vk == VK_OEM_MINUS) return 13;
    if (vk == VK_OEM_PLUS) return 14;
    if (vk == VK_SPACE) return 1;
    if (vk == VK_RETURN) return 2;
    if (vk == VK_TAB) return 3;
    if (vk == VK_ESCAPE) return 60;
    if (vk == VK_LEFT) return 61;
    if (vk == VK_RIGHT) return 62;
    if (vk == VK_UP) return 63;
    if (vk == VK_DOWN) return 64;
    if (vk == VK_PRIOR) return 67;
    if (vk == VK_NEXT) return 66;
    if (vk == VK_LMENU || vk == VK_MENU) return 53;
    if (vk == VK_RMENU) return 54;
    if (vk == VK_LCONTROL || vk == VK_CONTROL) return 55;
    if (vk == VK_RCONTROL) return 56;
    return 0;
}

void Init() {
    EnsureCs();
    gInputManager = nullptr;
    gLastRebind = 0;
    gSubmitSeq.store(0);
    gDoneSeq.store(0);
    gJobOk.store(false);
    gJobFocusedSkip.store(false);
    EnterCriticalSection(&gRelCs);
    for (auto& s : gReleases) s = {};
    LeaveCriticalSection(&gRelCs);
    x::runtime::LogI("InputPort", "ready (UserLocal.OnKey via shared MainPump)");
}

void Shutdown() { gInputManager = nullptr; }

bool EnsureBound() { return ResolveInputManager(GetTickCount()); }

bool Ready() { return gInputManager != nullptr && gGA != nullptr; }

bool InjectKeyHold(WORD vk, DWORD holdMs) {
    const int32_t key = VkToUnityKey(vk);
    if (key <= 0) {
        x::runtime::LogW("InputPort", "no Unity Key for VK=0x%02X", (unsigned)vk);
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD hold = holdMs < 50 ? 50 : holdMs;
    const DWORD dueAt = now + hold;

    if (ExtendHoldIfActive(vk, key, dueAt)) return true;

    if (!EnsureBound()) return false;

    bool focusedSkip = false;
    if (!EnqueueAndWait(JobKind::Down, key, &focusedSkip)) {
        if (focusedSkip) {
            x::runtime::LogWThrottled(41, 3000, "InputPort", "input field focused — skip fire");
        } else {
            x::runtime::LogWThrottled(40, 5000, "InputPort", "Down fail VK=0x%02X (no user / SEH / pump)",
                                     (unsigned)vk);
        }
        return false;
    }

    if (!ScheduleRelease(vk, key, dueAt)) {
        x::runtime::LogW("InputPort", "release queue full — immediate Up VK=0x%02X", (unsigned)vk);
        (void)EnqueueAndWait(JobKind::Up, key, nullptr);
        return false;
    }
    return true;
}

bool InjectUnityKeyHold(int32_t unityKey, DWORD holdMs) {
    if (unityKey <= 0) return false;
    // Synthetic VK slot: high bit marks Unity-key direct path (avoid VkToUnityKey).
    const WORD synthVk = static_cast<WORD>(0x8000u | (static_cast<uint16_t>(unityKey) & 0x7FFFu));

    const DWORD now = GetTickCount();
    const DWORD hold = holdMs < 50 ? 50 : holdMs;
    const DWORD dueAt = now + hold;

    if (ExtendHoldIfActive(synthVk, unityKey, dueAt)) return true;
    if (!EnsureBound()) return false;

    bool focusedSkip = false;
    if (!EnqueueAndWait(JobKind::Down, unityKey, &focusedSkip)) {
        if (focusedSkip) {
            x::runtime::LogWThrottled(41, 3000, "InputPort", "input field focused — skip fire");
        } else {
            x::runtime::LogWThrottled(42, 5000, "InputPort", "Down fail UnityKey=%d (no user / SEH / pump)",
                                     (int)unityKey);
        }
        return false;
    }
    if (!ScheduleRelease(synthVk, unityKey, dueAt)) {
        (void)EnqueueAndWait(JobKind::Up, unityKey, nullptr);
        return false;
    }
    return true;
}

void TickReleases(DWORD nowMs) {
    EnsureCs();
    ReleaseSlot due[kReleaseSlots]{};
    size_t dueCount = 0;
    EnterCriticalSection(&gRelCs);
    for (auto& slot : gReleases) {
        if (!slot.active) continue;
        if (static_cast<int>(nowMs - slot.dueAt) < 0) continue;
        if (dueCount < kReleaseSlots) due[dueCount++] = slot;
        slot = {};
    }
    LeaveCriticalSection(&gRelCs);

    for (size_t i = 0; i < dueCount; ++i) {
        (void)EnqueueAndWait(JobKind::Up, due[i].key, nullptr);
    }
}

void ForceReleaseVk(WORD vk) {
    const int32_t key = VkToUnityKey(vk);
    if (key <= 0) return;
    EnsureCs();
    bool wasActive = false;
    EnterCriticalSection(&gRelCs);
    for (auto& slot : gReleases) {
        if (!slot.active || slot.vk != vk) continue;
        slot = {};
        wasActive = true;
    }
    LeaveCriticalSection(&gRelCs);
    // Only Up if we actually held the key. Blind Up → KeyTouch SEH → MainPump crash.
    if (!wasActive) return;
    if (EnsureBound()) (void)EnqueueAndWait(JobKind::Up, key, nullptr);
}

void ForceReleaseUnityKey(int32_t unityKey) {
    if (unityKey <= 0) return;
    const WORD synthVk = static_cast<WORD>(0x8000u | (static_cast<uint16_t>(unityKey) & 0x7FFFu));
    EnsureCs();
    bool wasActive = false;
    EnterCriticalSection(&gRelCs);
    for (auto& slot : gReleases) {
        if (!slot.active || slot.vk != synthVk) continue;
        slot = {};
        wasActive = true;
    }
    LeaveCriticalSection(&gRelCs);
    if (!wasActive) return;
    if (EnsureBound()) (void)EnqueueAndWait(JobKind::Up, unityKey, nullptr);
}

}  // namespace x::features::ports::input
