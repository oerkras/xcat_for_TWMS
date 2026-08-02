// Classic TWMS — UserPool remote-user count (encounter strategy).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "user_pool_port.h"

#include "../../runtime/il2cpp_bind.h"
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

// TW dump.cs.restored TypeDef 1582（hashed UserPool : Singleton<UserPool>）
constexpr char kUserPoolClass[] =
    "e0e036a9ec92270b56665a9e49b13b623901f668de20e19a5a1ede61aa7942e";

// IDA：0x110E230 = get_UserLocal（return *(this+0x10)）；0x110E240 = GetRemoteUserCount（CFA）.
// 采样优先读 Dictionary@+0x20 的 count@+0x20，避免调 CFA。
constexpr size_t kOffRemoteDict = 0x20;
constexpr size_t kOffDictCount = 0x20;  // IL2CPP Dictionary.count（与 drop/mob/skill 一致）
constexpr size_t kOffRemoteList = 0x18;
constexpr size_t kOffListSize = 0x18;

constexpr DWORD kJobWaitMs = 800;
constexpr DWORD kRebindMs = 2000;

void* gKlass = nullptr;
void* gPool = nullptr;
DWORD gLastBind = 0;

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

bool LooksLikeUserPool(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gKlass && !ObjKlassIs(cand, gKlass)) return false;
    void* dict = ReadPtr(cand, kOffRemoteDict);
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

    if (gRuntimeClassInit) {
        __try {
            gRuntimeClassInit(gKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* staticsKlass = gKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gKlass);
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
    void* dict = ReadPtr(pool, kOffRemoteDict);
    if (LooksLikeHeapPtr(dict)) {
        const int n = ReadI32(dict, kOffDictCount);
        if (n >= 0 && n < 512) return n;
    }
    // 兜底：List@+0x18._size（GetRemoteUserCount CFA 路径可见 [rax+18h]）
    void* list = ReadPtr(pool, kOffRemoteList);
    if (LooksLikeHeapPtr(list)) {
        const int n = ReadI32(list, kOffListSize);
        if (n >= 0 && n < 512) return n;
    }
    return 0;
}

struct JobCtx {
    bool ok = false;
    int count = -1;
};

void JobFn(void* user) {
    auto* job = reinterpret_cast<JobCtx*>(user);
    if (!job) return;
    job->ok = false;
    job->count = -1;
    void* pool = ResolveUserPool();
    if (!pool) return;
    job->count = CountFromPool(pool);
    job->ok = job->count >= 0;
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

void* PeekUserPool() { return gPool; }

}  // namespace x::features::ports::user_pool
