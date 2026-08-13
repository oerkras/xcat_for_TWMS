// Classic TWMS — UserPool remote-user count / enum (encounter + player_hide).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "user_pool_port.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <atomic>
#include <cstring>

namespace x::features::ports::user_pool {
namespace {

using x::runtime::il2cpp::AtRva;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// remount 2026-08-04 · TypeDef 1582 UserPool : Singleton<UserPool>
// 旧 e0e036a9… 已不存在；字段布局未变（UL@0x10 List@0x18 Dict@0x20 Field@0x28）
constexpr char kUserPoolClass[] =
    "e81242592b38c5cf9e9ffb9adedaa1ce483632b75032a109420e6cc2e82d7df";
// 字段防漂移：哈希名 → field_get_offset；失败回退 Hint
constexpr char kHashRemoteDict[] =
    "bb787cb1e021b60e250a47d4e8abda535e8f8511598fe4a764523d706510252";
constexpr char kHashRemoteList[] =
    "ac4d630bde85acee99c58226996cb2a2a442c81914c2a2a2ac9a5d087a72d1e";

// GetRemoteUserCount CFA @ 0x1114BA0（旧 0x110E240）；采样优先读字段，避免调 CFA。
constexpr size_t kOffUserLocalHint = 0x10;  // get_UserLocal → *(this+0x10)
constexpr size_t kOffRemoteDictHint = 0x20;
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
constexpr size_t kOffRemoteListHint = 0x18;
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kEntryStride (x::runtime::il2cpp_container::DictEntryStrideIntPtr())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())
#define kOffEntryValue (x::runtime::il2cpp_container::OffDictEntryValuePtr())

size_t gOffRemoteDict = kOffRemoteDictHint;
size_t gOffRemoteList = kOffRemoteListHint;
std::atomic<bool> gFieldOffResolved{false};

constexpr int kEnumCapHard = 256;

// UserRemote.GetJobCode → *(uint16*)(this+0x3AE)；JobCategory = job%1000/100（8=Manager 9=Admin）.
constexpr size_t kOffRemoteJobCode = 0x3AE;
constexpr char kHashAvatarRootField[] =
    "f268160e507d06b690ae35c6902484c20f15ec4811a036d4bc5dd9560c99be1";
constexpr char kFldAvatarRoot[] = "_avatarRoot";
constexpr size_t kFbAvatarRoot = 0x80;
// User.<CharacterName>k__BackingField（dump.cs.restored TypeDef 1560；titlebar 同源）
constexpr char kHashCharacterNameField[] =
    "bae7058d71a538bb4a1cbbb8262033881623016136c28f0e655fd2f3d54a51a";
constexpr char kFldCharacterName[] = "<CharacterName>k__BackingField";
constexpr size_t kFbCharacterName = 0x1B8;
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E96720;  // remount 2026-08-06 · shop_port 同源

constexpr DWORD kJobWaitMs = 800;
constexpr DWORD kRebindMs = 2000;

void* gKlass = nullptr;
void* gPool = nullptr;
DWORD gLastBind = 0;

size_t gOffAvatarRoot = kFbAvatarRoot;
bool gAvatarOffTried = false;
size_t gOffCharacterName = kFbCharacterName;
bool gCharNameOffTried = false;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

using FnGoGetActiveSelf = bool (*)(void* self, const void* method);
MethodInfoHead* gMiGoGetActiveSelf = nullptr;
bool gActiveSelfTried = false;

using FnClassParent = void* (*)(void* klass);
using FnClassStaticData = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);

FnClassParent gClassParent = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;

std::atomic<bool> gExportsTried{false};

int ReadI32(void* base, size_t off) {
    if (!base) return 0;
    int v = 0;
    __try {
        v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return v;
}

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

void EnsureExports() {
    if (gExportsTried.exchange(true)) return;
    const auto& ex = x::runtime::il2cpp::Get();
    gClassParent = ex.classParent;
    gClassStaticData = ex.classStaticData;
    gRuntimeClassInit = ex.runtimeClassInit;
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

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

size_t FieldOffsetByHash(void* klass, const char* nameHash) {
    if (!klass || !nameHash || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    for (void* k = klass; k; ) {
        if (e.classGetFieldFromName && e.fieldGetOffset) {
            void* field = nullptr;
            __try {
                field = e.classGetFieldFromName(k, nameHash);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                field = nullptr;
            }
            if (field) {
                size_t off = 0;
                __try {
                    off = e.fieldGetOffset(field);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    off = 0;
                }
                if (off) return off;
            }
        }
        if (e.classGetFields && e.fieldGetName && e.fieldGetOffset) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* field = e.classGetFields(k, &iter);
                    if (!field) break;
                    const char* nm = nullptr;
                    __try {
                        nm = e.fieldGetName(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        nm = nullptr;
                    }
                    if (!nm) continue;
                    if (std::strcmp(nm, nameHash) != 0 && !std::strstr(nm, nameHash)) continue;
                    size_t off = 0;
                    __try {
                        off = e.fieldGetOffset(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        off = 0;
                    }
                    if (off) return off;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return 0;
}

void EnsureFieldOffsets() {
    if (gFieldOffResolved.load(std::memory_order_acquire)) return;
    if (!gKlass || !x::runtime::il2cpp::Ensure()) return;
    const size_t d = FieldOffsetByHash(gKlass, kHashRemoteDict);
    const size_t l = FieldOffsetByHash(gKlass, kHashRemoteList);
    if (d) gOffRemoteDict = d;
    if (l) gOffRemoteList = l;
    gFieldOffResolved.store(true, std::memory_order_release);
    const int hits = (d ? 1 : 0) + (l ? 1 : 0);
    x::runtime::LogI("UserPool", "field off path=%s hits=%d/2 dict=0x%zX list=0x%zX",
                     hits == 2 ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                     gOffRemoteDict, gOffRemoteList);
}

bool LooksLikeUserPool(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gKlass && !ObjKlassIs(cand, gKlass)) return false;
    void* dict = ReadPtr(cand, gOffRemoteDict);
    if (!dict) return true;
    return LooksLikeHeapPtr(dict);
}

void* ResolveUserPool() {
    const DWORD now = GetTickCount();
    if (gPool && LooksLikeUserPool(gPool) && now - gLastBind < kRebindMs) return gPool;
    gLastBind = now;
    gPool = nullptr;

    EnsureExports();
    if (!x::runtime::il2cpp::Ensure()) return nullptr;
    if (!gKlass) {
        gKlass = x::runtime::il2cpp::FindClass("", kUserPoolClass);
        if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("Msc.Game.Object", kUserPoolClass);
        if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("Msc.Game.Object", "UserPool");
    }
    if (!gKlass) {
        x::runtime::LogWThrottled(41, 15000, "UserPool", "FindClass miss");
        return nullptr;
    }
    EnsureFieldOffsets();

    if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gKlass);

    void* staticsKlass = gKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
            staticsKlass = parent;
        }
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gKlass);
    if (!statics) return nullptr;

    for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
        void* lazy = ReadPtr(statics, s);
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeUserPool(cand)) continue;
        gPool = cand;
        return gPool;
    }
    x::runtime::LogWThrottled(42, 15000, "UserPool", "resolve miss klass=%p", gKlass);
    return nullptr;
}

int CountFromPool(void* pool) {
    if (!pool) return -1;
    void* dict = ReadPtr(pool, gOffRemoteDict);
    if (LooksLikeHeapPtr(dict)) {
        const int n = ReadI32(dict, kOffDictCount);
        if (n >= 0 && n < 512) return n;
    }
    // 兜底：List._size（GetRemoteUserCount CFA 可见）
    void* list = ReadPtr(pool, gOffRemoteList);
    if (LooksLikeHeapPtr(list)) {
        const int n = ReadI32(list, kOffListSize);
        if (n >= 0 && n < 512) return n;
    }
    return 0;
}

void* UserLocalFromPool(void* pool) {
    if (!pool) return nullptr;
    void* lu = ReadPtr(pool, kOffUserLocalHint);
    return LooksLikeHeapPtr(lu) ? lu : nullptr;
}

bool PushUnique(void** out, int cap, int* n, void* user) {
    if (!out || !n || !LooksLikeHeapPtr(user) || *n >= cap) return false;
    for (int i = 0; i < *n; ++i) {
        if (out[i] == user) return false;
    }
    out[(*n)++] = user;
    return true;
}

// 主线程：List 优先，Dict entries 兜底；始终跳过 UserLocal。
int EnumFromPool(void* pool, void** out, int cap) {
    if (!pool || !out || cap <= 0) return -1;
    if (cap > kEnumCapHard) cap = kEnumCapHard;
    const void* local = UserLocalFromPool(pool);
    int n = 0;

    void* list = ReadPtr(pool, gOffRemoteList);
    if (LooksLikeHeapPtr(list)) {
        x::runtime::il2cpp_container::RefineFromListInstance(list);
        void* items = ReadPtr(list, kOffListItems);
        const int size = ReadI32(list, kOffListSize);
        if (LooksLikeHeapPtr(items) && size > 0 && size < 512) {
            const uintptr_t arrLen = x::runtime::il2cpp::ArrayLen(items);
            const int take = size < static_cast<int>(arrLen) ? size : static_cast<int>(arrLen);
            for (int i = 0; i < take && n < cap; ++i) {
                void* u = x::runtime::il2cpp::ArrayAt(items, static_cast<uintptr_t>(i));
                if (!LooksLikeHeapPtr(u) || u == local) continue;
                PushUnique(out, cap, &n, u);
            }
            if (n > 0) return n;
        }
    }

    void* dict = ReadPtr(pool, gOffRemoteDict);
    if (!LooksLikeHeapPtr(dict)) return n;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count <= 0 || count >= 512) return n;
    // 与 drop_pool 一致：扫 entries 全长，hash<0 为空槽；勿用 count 当索引上限。
    const uintptr_t arrLen = x::runtime::il2cpp::ArrayLen(entries);
    if (arrLen == 0 || arrLen > 2048) return n;
    int live = 0;
    for (uintptr_t i = 0; i < arrLen && n < cap; ++i) {
        uint8_t* entry =
            x::runtime::il2cpp_container::DictEntryAt(entries, static_cast<int>(i),
                                                       kEntryStride);
        if (!entry) continue;
        int hash = 0;
        __try {
            hash = *reinterpret_cast<int*>(entry + kOffEntryHash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (hash < 0) continue;
        ++live;
        void* u = nullptr;
        __try {
            u = *reinterpret_cast<void**>(entry + kOffEntryValue);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!LooksLikeHeapPtr(u) || u == local) continue;
        PushUnique(out, cap, &n, u);
        if (live >= count && n > 0) break;  // 已凑满存活槽，可提前停
    }
    return n;
}

uint16_t ReadU16(void* base, size_t off) {
    if (!base) return 0;
    uint16_t v = 0;
    __try {
        v = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return v;
}

bool IsAdminLikeJob(uint16_t job) {
    if (job == 0 || job == 0xFFFF) return false;
    const int cat = static_cast<int>(job % 1000) / 100;
    return cat == 8 || cat == 9;  // Manager / Admin
}

void EnsureAvatarRootOffset() {
    if (gAvatarOffTried) return;
    gAvatarOffTried = true;
    if (!x::runtime::il2cpp::Ensure()) return;
    const auto& e = x::runtime::il2cpp::Get();
    // remotes 是 User 子类；用 User 哈希类找 avatar（与 player_hide 同 hash）；不依赖 UserPool gKlass.
    void* k = x::runtime::il2cpp::FindClass("",
        "c3c6ef70537e5a2c4026c37e65e0d0a8a5f756988f3f3ee148a568fb3176f96");
    if (!k) k = x::runtime::il2cpp::FindClass("Msc.Game.Object", "User");
    if (!k) return;
    const char* names[] = {kFldAvatarRoot, kHashAvatarRootField};
    for (; k; ) {
        for (const char* nm : names) {
            if (!nm || !e.classGetFieldFromName || !e.fieldGetOffset) continue;
            void* field = nullptr;
            __try {
                field = e.classGetFieldFromName(k, nm);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                field = nullptr;
            }
            if (!field) continue;
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (off) {
                gOffAvatarRoot = off;
                x::runtime::LogI("UserPool", "AvatarRoot off=0x%zX", gOffAvatarRoot);
                return;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
}

void* ResolveUserKlass() {
    void* k = x::runtime::il2cpp::FindClass("",
        "c3c6ef70537e5a2c4026c37e65e0d0a8a5f756988f3f3ee148a568fb3176f96");
    if (!k) k = x::runtime::il2cpp::FindClass("Msc.Game.Object", "User");
    return k;
}

void EnsureCharacterNameOffset() {
    if (gCharNameOffTried) return;
    gCharNameOffTried = true;
    if (!x::runtime::il2cpp::Ensure()) return;
    void* k = ResolveUserKlass();
    if (!k) return;
    size_t off = FieldOffsetByHash(k, kHashCharacterNameField);
    if (!off) off = FieldOffsetByHash(k, kFldCharacterName);
    if (!off) off = FieldOffsetByHash(k, "CharacterName");
    if (off >= 0x10 && off < 0x800) {
        gOffCharacterName = off;
        x::runtime::LogI("UserPool", "CharacterName off=0x%zX", gOffCharacterName);
    }
}

bool ReadIl2CppStringUtf8(void* str, char* out, int outSz) {
    if (!str || !out || outSz <= 0) return false;
    out[0] = 0;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 64) return false;
        const wchar_t* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        // 按码点缩短，避免缓冲区不够时 WC 失败或截出半截 UTF-8。
        for (int wlen = len; wlen > 0; --wlen) {
            const int n =
                WideCharToMultiByte(CP_UTF8, 0, chars, wlen, out, outSz - 1, nullptr, nullptr);
            if (n > 0) {
                out[n] = 0;
                return true;
            }
        }
        out[0] = 0;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

bool ReadUserCharacterNameLocked(void* user, char* out, int outSz) {
    if (!LooksLikeHeapPtr(user) || !out || outSz <= 0) return false;
    out[0] = 0;
    EnsureCharacterNameOffset();
    void* nameObj = ReadPtr(user, gOffCharacterName);
    if (!LooksLikeHeapPtr(nameObj)) return false;
    return ReadIl2CppStringUtf8(nameObj, out, outSz);
}

void EnsureActiveSelfMi() {
    if (gActiveSelfTried) return;
    gActiveSelfTried = true;
    if (!x::runtime::il2cpp::Ensure()) return;
    const auto& e = x::runtime::il2cpp::Get();
    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    if (!goKlass || !e.classGetMethods || !e.ga) return;
    const uintptr_t want = reinterpret_cast<uintptr_t>(e.ga) + kRvaGoGetActiveSelf;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = e.classGetMethods(goKlass, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            void* mp = nullptr;
            __try {
                mp = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mp = nullptr;
            }
            if (reinterpret_cast<uintptr_t>(mp) == want) {
                gMiGoGetActiveSelf = mi;
                break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!gMiGoGetActiveSelf && e.classGetMethodFromName) {
        void* mi = nullptr;
        __try {
            mi = e.classGetMethodFromName(goKlass, "get_activeSelf", 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
        if (mi) gMiGoGetActiveSelf = reinterpret_cast<MethodInfoHead*>(mi);
    }
}

bool ReadAvatarActiveSelf(void* user) {
    if (!LooksLikeHeapPtr(user)) return true;
    EnsureAvatarRootOffset();
    void* go = ReadPtr(user, gOffAvatarRoot);
    if (!LooksLikeHeapPtr(go)) return true;  // 无 avatar → 不当隐身嫌疑
    EnsureActiveSelfMi();
    FnGoGetActiveSelf fn = nullptr;
    const void* mi = gMiGoGetActiveSelf;
    if (gMiGoGetActiveSelf && gMiGoGetActiveSelf->methodPointer)
        fn = reinterpret_cast<FnGoGetActiveSelf>(gMiGoGetActiveSelf->methodPointer);
    else
        fn = AtRva<FnGoGetActiveSelf>(kRvaGoGetActiveSelf);
    if (!fn) return true;
    bool active = true;
    __try {
        active = fn(go, mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active = true;
    }
    return active;
}

void FillThreatFromUsers(void** users, int n, RemoteThreatSample* out, bool checkAvatarHide) {
    if (!out) return;
    out->remoteCount = n > 0 ? n : 0;
    out->adminLikeCount = 0;
    out->hideSuspectCount = 0;
    out->sampleAdminJob = 0;
    out->nameCount = 0;
    for (int i = 0; i < kRemoteNameMax; ++i) out->names[i][0] = 0;
    for (int i = 0; i < n; ++i) {
        void* u = users[i];
        if (!LooksLikeHeapPtr(u)) continue;
        const uint16_t job = ReadU16(u, kOffRemoteJobCode);
        if (IsAdminLikeJob(job)) {
            ++out->adminLikeCount;
            if (!out->sampleAdminJob) out->sampleAdminJob = job;
        }
        if (checkAvatarHide && !ReadAvatarActiveSelf(u)) ++out->hideSuspectCount;
        if (out->nameCount < kRemoteNameMax) {
            char* slot = out->names[out->nameCount];
            if (ReadUserCharacterNameLocked(u, slot, kRemoteNameLen) && slot[0]) {
                ++out->nameCount;
            }
        }
    }
}

struct ThreatJobCtx {
    bool ok = false;
    bool checkAvatarHide = false;
    RemoteThreatSample* out = nullptr;
};

void ThreatJobFn(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("user_pool.Threat");
    auto* job = reinterpret_cast<ThreatJobCtx*>(user);
    if (!job || !job->out) return;
    job->ok = false;
    *job->out = RemoteThreatSample{};
    void* pool = ResolveUserPool();
    if (!pool) return;
    void* users[kEnumCapHard]{};
    const int n = EnumFromPool(pool, users, kEnumCapHard);
    if (n < 0) return;
    FillThreatFromUsers(users, n, job->out, job->checkAvatarHide);
    job->ok = true;
}

struct JobCtx {
    bool ok = false;
    int count = -1;
};

void JobFn(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("user_pool.Job");
    auto* job = reinterpret_cast<JobCtx*>(user);
    if (!job) return;
    job->ok = false;
    job->count = -1;
    void* pool = ResolveUserPool();
    if (!pool) return;
    job->count = CountFromPool(pool);
    job->ok = job->count >= 0;
}

struct EnumJobCtx {
    bool ok = false;
    void** out = nullptr;
    int cap = 0;
    int count = 0;
};

void EnumJobFn(void* user) {
    (void)x::runtime::main_thread::AssertOnPumpThread("user_pool.Enum");
    auto* job = reinterpret_cast<EnumJobCtx*>(user);
    if (!job || !job->out || job->cap <= 0) return;
    job->ok = false;
    job->count = 0;
    void* pool = ResolveUserPool();
    if (!pool) return;
    const int n = EnumFromPool(pool, job->out, job->cap);
    if (n < 0) return;
    job->count = n;
    job->ok = true;
}

}  // namespace

bool SampleRemoteUserCount(int* outCount) {
    if (!outCount) return false;
    *outCount = 0;
    JobCtx job{};
    if (!x::runtime::main_thread::Ensure()) return false;
    if (!x::runtime::main_thread::InvokeAndWait(&JobFn, &job, kJobWaitMs)) return false;
    if (!job.ok || job.count < 0) return false;
    *outCount = job.count;
    return true;
}

bool SampleRemoteThreat(RemoteThreatSample* out, bool checkAvatarHide) {
    if (!out) return false;
    *out = RemoteThreatSample{};
    if (x::runtime::main_thread::IsOnPumpThread()) {
        void* pool = ResolveUserPool();
        if (!pool) return false;
        void* users[kEnumCapHard]{};
        const int n = EnumFromPool(pool, users, kEnumCapHard);
        if (n < 0) return false;
        FillThreatFromUsers(users, n, out, checkAvatarHide);
        return true;
    }
    ThreatJobCtx job{};
    job.checkAvatarHide = checkAvatarHide;
    job.out = out;
    if (!x::runtime::main_thread::Ensure()) return false;
    if (!x::runtime::main_thread::InvokeAndWait(&ThreatJobFn, &job, kJobWaitMs)) return false;
    return job.ok;
}

bool EnumRemoteUsers(void** out, int cap, int* outCount) {
    if (outCount) *outCount = 0;
    if (!out || cap <= 0 || !outCount) return false;
    // 已在泵线程：直接枚举，避免自死锁。
    if (x::runtime::main_thread::IsOnPumpThread()) {
        void* pool = ResolveUserPool();
        if (!pool) return false;
        const int n = EnumFromPool(pool, out, cap);
        if (n < 0) return false;
        *outCount = n;
        return true;
    }
    EnumJobCtx job{};
    job.out = out;
    job.cap = cap;
    if (!x::runtime::main_thread::Ensure()) return false;
    if (!x::runtime::main_thread::InvokeAndWait(&EnumJobFn, &job, kJobWaitMs)) return false;
    if (!job.ok) return false;
    *outCount = job.count;
    return true;
}

void* PeekUserLocal() {
    void* pool = gPool;
    if (!pool) pool = ResolveUserPool();
    return UserLocalFromPool(pool);
}

void* PeekUserPool() { return gPool; }

bool PeekRemoteUserCount(int* outCount) {
    if (!outCount) return false;
    *outCount = 0;
    // 与 PeekUserLocal 一致：冷路径也 Resolve，避免 gPool 未绑时误判 remotes 失败→独图放行。
    void* pool = gPool;
    if (!pool || !LooksLikeUserPool(pool)) pool = ResolveUserPool();
    if (!pool || !LooksLikeUserPool(pool)) return false;
    const int n = CountFromPool(pool);
    if (n < 0) return false;
    *outCount = n;
    return true;
}

bool PeekEnumRemoteUsers(void** out, int cap, int* outCount) {
    if (outCount) *outCount = 0;
    if (!out || cap <= 0 || !outCount) return false;
    void* pool = gPool;
    if (!pool || !LooksLikeUserPool(pool)) pool = ResolveUserPool();
    if (!pool || !LooksLikeUserPool(pool)) return false;
    const int n = EnumFromPool(pool, out, cap);
    if (n < 0) return false;
    *outCount = n;
    return true;
}

bool ReadUserCharacterName(void* user, char* out, int outSz) {
    return ReadUserCharacterNameLocked(user, out, outSz);
}

}  // namespace x::features::ports::user_pool
