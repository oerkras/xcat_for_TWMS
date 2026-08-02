#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "managed_main.h"

#include "log.h"
#include "main_thread_pump.h"

#include <atomic>

namespace x::runtime::managed_main {
namespace {

// Frozen until in-map: blocks lobby FindAll storms from invuln/titlebar/ports.
std::atomic<int> gLoginFreeze{1};
std::atomic<DWORD> gLastFreezeLogMs{0};

struct FindAllCtx {
    FnFindAll fn = nullptr;
    void* typeObj = nullptr;
    void* result = nullptr;
};

void FindAllJob(void* user) {
    auto* c = reinterpret_cast<FindAllCtx*>(user);
    if (!c || !c->fn || !c->typeObj) return;
    __try {
        c->result = c->fn(c->typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c->result = nullptr;
    }
}

struct TypeObjCtx {
    FnTypeGetObject fn = nullptr;
    void* type = nullptr;
    void* result = nullptr;
};

void TypeObjJob(void* user) {
    auto* c = reinterpret_cast<TypeObjCtx*>(user);
    if (!c || !c->fn || !c->type) return;
    __try {
        c->result = c->fn(c->type);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        c->result = nullptr;
    }
}

void FreezeLogOnce(const char* what) {
    const DWORD now = GetTickCount();
    if (now - gLastFreezeLogMs.load() < 5000) return;
    gLastFreezeLogMs.store(now);
    x::runtime::LogI("ManagedMain", "login-freeze skip %s", what);
}

}  // namespace

void SetLoginFreeze(bool on) {
    const int prev = gLoginFreeze.exchange(on ? 1 : 0);
    if ((prev != 0) != on) {
        x::runtime::LogI("ManagedMain", "login-freeze=%d", on ? 1 : 0);
    }
}

bool IsLoginFrozen() { return gLoginFreeze.load() != 0; }

bool Call(JobFn fn, void* user, DWORD timeoutMs) {
    return main_thread::InvokeAndWait(fn, user, timeoutMs);
}

void* FindAll(FnFindAll fn, void* typeObj, DWORD timeoutMs, bool bypassFreeze) {
    if (!fn || !typeObj) return nullptr;
    if (!bypassFreeze && IsLoginFrozen()) {
        FreezeLogOnce("FindAll");
        return nullptr;
    }
    FindAllCtx c{fn, typeObj, nullptr};
    if (!Call(&FindAllJob, &c, timeoutMs)) return nullptr;
    return c.result;
}

void* TypeGetObject(FnTypeGetObject fn, void* type, DWORD timeoutMs, bool bypassFreeze) {
    if (!fn || !type) return nullptr;
    if (!bypassFreeze && IsLoginFrozen()) {
        FreezeLogOnce("TypeGetObject");
        return nullptr;
    }
    TypeObjCtx c{fn, type, nullptr};
    if (!Call(&TypeObjJob, &c, timeoutMs)) return nullptr;
    return c.result;
}

}  // namespace x::runtime::managed_main
