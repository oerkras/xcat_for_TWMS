// Classic TWMS — Inject* 门面 → unity_kbd（InputSystem KeyboardState）。
// 旧 OnKey 解析代码仍留在本文件（灯/诊断），Inject 路径不再调用。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "input_port.h"
#include "unity_kbd_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"

#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"

#include <Psapi.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::input {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaOnKey = 0x1021520;  // remounted 2026-08-06 · UserLocal.OnKey (+0x1E70)
constexpr uint32_t kRvaIsFocusedInputField = 0x166CFC0;  // remounted 2026-08-06 · IM IsFocusedInputField
constexpr int32_t kKeyInputDown = 0;
constexpr int32_t kKeyInputUp = 1;

// Game InputManager（非 UnityEngine）。Remount 2026-08-06：TDI 2303 ACS 重哈希。
constexpr char kInputManagerClass[] =
    "ab1dbbc390881790e19a5b1557c84a32d8c2261a26e7e4eaaf8f02e6be32a99";
// remounted UserLocal class hash（与 il2cpp_shape::kHashUserLocal 同）
constexpr char kUserLocalClass[] =
    "d81db6fbb1dc9506e153d6ee92c803ded0eef9dd0bf5c0e2334f2a98cabf4b0";  // remounted 2026-08-06 UL
constexpr char kHashOnKey[] =
    "c9267edb5f4326ea327c145d59260ac5b26a09697b8e7ca18757436ed63ff52";
constexpr char kHashIsFocused[] =
    "a36c99b43bc935805103a390ef12a527cf5d2a22830caa6501177adf2cd8601";
// TargetUser 字段防漂移：hash → field_get_offset；仍 @0x20
constexpr char kHashTargetUser[] =
    "<a5f06ffcd14e4475088ea9ac03212af97c74fd7aa80d2839f35c33b2bc91f8d>k__BackingField";
constexpr size_t kFbTargetUser = 0x20;

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

size_t gOffTargetUser = kFbTargetUser;
bool gTargetUserOffTried = false;
const char* gTargetUserPath = "fallback";  // meta | fallback

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

bool PlausibleInstanceOff(size_t off) { return off >= 0x10 && off < 0x1000; }

void EnsureTargetUserOffset() {
    if (gTargetUserOffTried) return;
    gTargetUserOffTried = true;
    gOffTargetUser = kFbTargetUser;
    gTargetUserPath = "fallback";

    if (!x::runtime::il2cpp::Ensure()) {
        x::runtime::LogW("InputPort", "TargetUser off: bind miss — fallback 0x%zX", kFbTargetUser);
        return;
    }
    if (!gImKlass) gImKlass = x::runtime::il2cpp::FindClass("", kInputManagerClass);
    if (!gImKlass) {
        x::runtime::LogW("InputPort", "TargetUser off: IM klass miss — fallback 0x%zX", kFbTargetUser);
        return;
    }

    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) {
        x::runtime::LogW("InputPort", "TargetUser off: field exports miss — fallback 0x%zX",
                         kFbTargetUser);
        return;
    }

    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(gImKlass, kHashTargetUser);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        field = nullptr;
    }
    if (!field) {
        x::runtime::LogW("InputPort", "TargetUser off: field hash miss — fallback 0x%zX",
                         kFbTargetUser);
        return;
    }
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        off = 0;
    }
    if (!PlausibleInstanceOff(off)) {
        x::runtime::LogW("InputPort", "TargetUser off: implausible 0x%zX — fallback 0x%zX", off,
                         kFbTargetUser);
        return;
    }
    gOffTargetUser = off;
    gTargetUserPath = "meta";
    x::runtime::LogI("InputPort", "TargetUser off path=%s 0x%zX (fb=0x%zX)", gTargetUserPath,
                     gOffTargetUser, kFbTargetUser);
}

size_t OffTargetUser() {
    EnsureTargetUserOffset();
    return gOffTargetUser;
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    // channel_hop 同形：同时认 methodPointer / virtualMethodPointer。
    if (!klass || !rva || !gGA) return nullptr;
    const auto& ex = x::runtime::il2cpp::Get();
    if (!ex.classGetMethods) return nullptr;
    void* target = AtRva<void*>(rva);
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* miRaw = ex.classGetMethods(cur, &iter);
                if (!miRaw) break;
                auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
                void* mp = nullptr;
                void* vp = nullptr;
                __try {
                    mp = mi->methodPointer;
                    vp = mi->virtualMethodPointer;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    continue;
                }
                if (mp == target || vp == target) return mi;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        if (!ex.classParent) break;
        void* parent = nullptr;
        __try {
            parent = ex.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        const int tryArgc[] = {argc, -1};
        for (int ac : tryArgc) {
            __try {
                mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, ac));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mi = nullptr;
            }
            if (mi && mi->methodPointer) return mi;
        }
    }
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
                          const char* plain, const char* hash,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    // SSOT：hash → plain → RVA/kind（OnKey 无明文，靠 hash）
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (outPath) *outPath = mr.path;
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("InputPort", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plain ? plain : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    // 最后：mp/vp 双指针 RVA（不依赖 GaBase 算术）
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

void EnsureMethodInfos() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    // dump: UserLocal$$df22d8fb… @0x10181E0 · TypeSignature viiii；UL 上同形约 9 个 → unique=false
    void* ul = x::runtime::il2cpp::FindClass("", kUserLocalClass);
    if (!ul) ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    ResolvePath pOn = ResolvePath::Miss, pFo = ResolvePath::Miss;
    if (ul && !gMiOnKey) {
        constexpr MethodShape kOn{2, TypeKind::Void, false, true, {TypeKind::Any, TypeKind::Any}};
        gMiOnKey = ResolveMi(ul, kRvaOnKey, kOn, nullptr, kHashOnKey, &pOn);
        if (gMiOnKey) {
            x::runtime::LogI("InputPort", "OnKey MI bind ok mi=%p rva=0x%X", gMiOnKey, kRvaOnKey);
        }
    }
    // 急切绑也要补 ImKlass：IsFocused MI 不依赖 singleton 实例，FindClass 即可。
    if (!gImKlass) gImKlass = x::runtime::il2cpp::FindClass("", kInputManagerClass);
    if (gImKlass && !gMiIsFocused) {
        constexpr MethodShape kFo{0, TypeKind::Bool, false, true, {}};
        gMiIsFocused = ResolveMi(gImKlass, kRvaIsFocusedInputField, kFo, "IsFocusedInputField",
                                 kHashIsFocused, &pFo);
        if (gMiIsFocused) {
            x::runtime::LogI("InputPort", "IsFocused MI bind ok mi=%p rva=0x%X", gMiIsFocused,
                             kRvaIsFocusedInputField);
        }
    }
    static bool sMethodHitsLogged = false;
    if (!sMethodHitsLogged && (gMiOnKey || gMiIsFocused)) {
        sMethodHitsLogged = true;
        int hits = 0;
        if (gMiOnKey) ++hits;
        if (gMiIsFocused) ++hits;
        const bool meta = (gMiOnKey && pOn == ResolvePath::Hash) &&
                          (gMiIsFocused && (pFo == ResolvePath::Hash || pFo == ResolvePath::Plain));
        x::runtime::LogI("InputPort", "methods path=%s hits=%d/2",
                         meta ? "meta" : (hits ? "meta-partial" : "fallback"), hits);
    }
    if (gMiOnKey) {
        x::runtime::anchor_lamps::Set("InputOnKey", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                     "OnKey MI");
    } else if (ul) {
        x::runtime::anchor_lamps::Set("InputOnKey",
                                     x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                     "RVA+pump");
    } else {
        // UL 未齐：先别橙
        x::runtime::anchor_lamps::Set("InputOnKey",
                                     x::runtime::anchor_lamps::AnchorLampCode::Unknown, "pending");
    }
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

    if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gImKlass);

    void* staticsKlass = gImKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gImKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
            staticsKlass = parent;
        }
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gImKlass);
    if (!statics) return nullptr;

    // 只收带 targetUser 的候选；无 user 的实例留给 FindAll，避免 bind 后 Down 全 fail。
    for (size_t s = 0; s < 4; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeHeapPtr(cand)) continue;
        if (LooksLikeHeapPtr(ReadPtr(cand, OffTargetUser()))) return cand;
    }
    return nullptr;
}

void* TryResolveFindAll() {
    if (!world::IsPlayReady()) return nullptr;
    if (!gImType) gImType = FindClassTypeObject(kInputManagerClass);
    if (!gImType || !gFindAll) return nullptr;
    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gImType, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    const uintptr_t n = ArrayLen(arr);
    for (uintptr_t i = 0; i < n && i < 8; ++i) {
        void* im = ArrayAt(arr, i);
        if (!LooksLikeHeapPtr(im)) continue;
        if (LooksLikeHeapPtr(ReadPtr(im, OffTargetUser()))) return im;
    }
    return nullptr;
}

bool InputManagerHasTargetUser() {
    if (!LooksLikeHeapPtr(gInputManager)) return false;
    __try {
        return LooksLikeHeapPtr(ReadPtr(gInputManager, OffTargetUser()));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResolveInputManager(DWORD now) {
    if (static_cast<int>(now - gSehCooldownUntil.load(std::memory_order_acquire)) < 0) {
        return false;
    }
    // 缓存必须连 targetUser 一起有效；换图后 user 变 null 时立刻丢掉重绑。
    if (InputManagerHasTargetUser()) return true;
    if (gInputManager) {
        gInputManager = nullptr;
        gLastRebind = 0;
    }
    if (gLastRebind && now - gLastRebind < kRebindMs) return false;
    gLastRebind = now;
    if (!BindApis()) return false;

    // InterStage / 卸图：禁 FindAll（targetUser 本就空，扫了只会拖黑屏）。
    // Singleton 仍可试；无 targetUser 则下面 ok=false。
    const bool allowFindAll = world::IsPlayReady();

    // RuntimeClassInit / Singleton 读托管静态字段必须在主线程，否则 GC unknown thread。
    struct Ctx {
        bool ok = false;
        bool allowFindAll = false;
        const char* how = "?";
    } ctx;
    ctx.allowFindAll = allowFindAll;
    auto job = [](void* user) {
        auto* c = reinterpret_cast<Ctx*>(user);
        if (!gImKlass) gImKlass = FindClass("", kInputManagerClass);
        void* best = TryResolveSingleton();
        c->how = "singleton";
        if (!best && c->allowFindAll) {
            // FindAll 直调（已在主线程）；禁止再套 managed_main::FindAll
            if (x::runtime::managed_main::IsLoginFrozen() ||
                x::runtime::managed_main::IsMapTransitBlocked()) {
                c->ok = false;
                return;
            }
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
                    if (!LooksLikeHeapPtr(ReadPtr(im, OffTargetUser()))) continue;
                    pick = im;
                    break;
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
        EnsureTargetUserOffset();
        x::runtime::LogI("InputPort",
                         "InputManager bind %p via %s targetUser=%d off=%s/0x%zX",
                         gInputManager, ctx.how,
                         LooksLikeHeapPtr(ReadPtr(gInputManager, OffTargetUser())) ? 1 : 0,
                         gTargetUserPath, gOffTargetUser);
        EnsureMethodInfos();
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

bool HasTargetUser() { return InputManagerHasTargetUser(); }

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
        void* lu = HasTargetUser() ? ReadPtr(gInputManager, OffTargetUser()) : nullptr;
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
                    // e2a28 探针：仅 Down（Up 不进位移钉点）；pre 需 XCAT_PROBE_ONKEY=1
                    player_combat::VisualSnap pre{};
                    char note[48]{};
                    if (kind == JobKind::Down) {
                        (void)player_combat::QueryVisualSnap(pre);
                        std::snprintf(note, sizeof(note), "key=%d", (int)key);
                        player_combat::LogE2a28KeyProbe("pre", note, nullptr);
                    }
                    fn(lu, inputType, key, nullptr);
                    if (kind == JobKind::Down) {
                        player_combat::LogE2a28KeyProbe("post", note, pre.ok ? &pre : nullptr);
                    }
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
    gOffTargetUser = kFbTargetUser;
    gTargetUserOffTried = false;
    gTargetUserPath = "fallback";
    gSubmitSeq.store(0);
    gDoneSeq.store(0);
    gJobOk.store(false);
    gJobFocusedSkip.store(false);
    EnterCriticalSection(&gRelCs);
    for (auto& s : gReleases) s = {};
    LeaveCriticalSection(&gRelCs);
    x::runtime::LogI("InputPort", "ready (Inject* → unity_kbd KeyboardState; OnKey path idle)");
    // 急切：unity_kbd + 旧 OnKey 灯（诊断用，Inject 不走 OnKey）
    (void)unity_kbd::EnsureBound();
    if (BindApis()) {
        EnsureTargetUserOffset();
        EnsureMethodInfos();
    }
}

void Shutdown() {
    gInputManager = nullptr;
    // unity_kbd::Shutdown 由 probe 统一调用；此处不重复摘钩。
}

bool EnsureBound() { return unity_kbd::EnsureBound() || unity_kbd::Ready(); }

bool Ready() { return unity_kbd::Ready() || unity_kbd::EnsureBound(); }

namespace {

constexpr DWORD kPulsePollMs = 16;
constexpr DWORD kKbdJobWaitMs = 800;

struct KbdHeldJob {
    int32_t key = 0;
    bool down = false;
    bool ok = false;
};

void KbdHeldJobOnMain(void* user) {
    auto* j = static_cast<KbdHeldJob*>(user);
    if (!j || j->key <= 0) return;
    j->ok = unity_kbd::SetKeyHeldOnMain(j->key, j->down);
    if (j->down) (void)unity_kbd::RepushOnMain();
}

// 已在泵线程则内联，禁止嵌套 InvokeAndWait（travel BIN 死锁）。
bool RunKbdHeld(int32_t unityKey, bool down, bool* outOk) {
    if (outOk) *outOk = false;
    KbdHeldJob job{};
    job.key = unityKey;
    job.down = down;
    if (x::runtime::main_thread::IsOnPumpThread()) {
        KbdHeldJobOnMain(&job);
    } else {
        if (!x::runtime::main_thread::Ensure()) return false;
        if (!x::runtime::main_thread::InvokeAndWait(&KbdHeldJobOnMain, &job, kKbdJobWaitMs,
                                                     x::runtime::main_thread::JobPrio::High)) {
            return false;
        }
    }
    if (outOk) *outOk = job.ok;
    return true;
}

bool TryKeyUp(int32_t unityKey) {
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        if (RunKbdHeld(unityKey, false, &ok) && ok) return true;
        Sleep(kPulsePollMs);
    }
    return false;
}

// Worker 优先；若误在主线程调用则内联执行（不嵌套等泵）。
bool PulseUnityKeySync(int32_t unityKey, DWORD holdMs) {
    if (unityKey <= 0) return false;
    if (!x::runtime::main_thread::Ensure() && !x::runtime::main_thread::IsOnPumpThread()) {
        x::runtime::LogWThrottled(40, 5000, "InputPort", "unity_kbd pulse: main pump missing");
        return false;
    }
    (void)unity_kbd::EnsureBound();
    const DWORD hold = holdMs < 50 ? 50 : holdMs;
    char detail[96]{};
    // 固定时长：until 恒 false，min=max=hold（与 Travel HoldUntil 同路）。
    const bool ok =
        unity_kbd::HoldUntil(unityKey, hold, hold, nullptr, nullptr, detail, sizeof(detail));
    if (!ok) {
        x::runtime::LogWThrottled(40, 5000, "InputPort",
                                  "unity_kbd HoldUntil fail key=%d detail=%s why=%s", (int)unityKey,
                                  detail, unity_kbd::LastFail());
    }
    return ok;
}

bool ForceUnityKeyUp(int32_t unityKey) {
    if (unityKey <= 0) return false;
    // SetKeyHeld(false) 对未持有位 no-op，不会空推全零态。
    bool ok = false;
    if (!RunKbdHeld(unityKey, false, &ok)) return false;
    return ok;
}

}  // namespace

bool InjectKeyHold(WORD vk, DWORD holdMs) {
    const int32_t key = VkToUnityKey(vk);
    if (key <= 0) {
        x::runtime::LogW("InputPort", "no Unity Key for VK=0x%02X", (unsigned)vk);
        return false;
    }
    return PulseUnityKeySync(key, holdMs);
}

bool InjectUnityKeyHold(int32_t unityKey, DWORD holdMs) {
    if (unityKey <= 0) return false;
    return PulseUnityKeySync(unityKey, holdMs);
}

void TickReleases(DWORD /*nowMs*/) {
    // 同步脉冲无异步松键队列。
}

void ForceReleaseVk(WORD vk) {
    const int32_t key = VkToUnityKey(vk);
    if (key <= 0) return;
    (void)ForceUnityKeyUp(key);
}

void ForceReleaseUnityKey(int32_t unityKey) {
    if (unityKey <= 0) return;
    (void)ForceUnityKeyUp(unityKey);
}

}  // namespace x::features::ports::input

