#include "il2cpp_network.h"

#include "il2cpp_bind.h"
#include "il2cpp_shape.h"
#include "log.h"
#include "main_thread_pump.h"

#include <atomic>
#include <cstdio>

namespace x::runtime::il2cpp_network {
namespace {

constexpr size_t kFbNmSession = 0x10;
constexpr size_t kFbNmSessionState = 0x18;
constexpr size_t kFbNmPacketQueue = 0x28;
constexpr size_t kFbNmOpcodeHashSet = 0x48;
constexpr size_t kFbSessionSeqSend = 0x18;
constexpr size_t kFbSessionClosed = 0x20;
constexpr size_t kFbSessionPendingError = 0x40;
constexpr size_t kFbSessionRecvList = 0x58;
constexpr size_t kFbSessionState = 0x60;

// dump.cs 2026-08-06 · Facade / Session fields（TypeDef 13772 / 13797）
constexpr char kHashNmSession[] =
    "abfccaf997f7745a1b90ffd7ef71f1af47f742f290b23abfe343fe955fa748c";
constexpr char kHashNmSessionState[] =
    "c3b0945c2cb2fa57c7da9e40ecba87371b043f6a866114d7772ffb1ab4a80bb";
constexpr char kHashNmPacketQueue[] =
    "aefefafb44d77dd51f4549d423eda1471a990ab4fb9db0788a432a4f91e9cd7";
constexpr char kHashNmOpcodeHashSet[] =
    "e10f67937e316d2594aa2ea2da0eaa4b36412be4d61b5230de8a080b1d7518a";

// Session fields
constexpr char kHashSessionSeqSend[] =
    "c4a5f31ee04094cb5365502f059ddf2b755cba4b4a4ff1652c444335072fb04";
constexpr char kHashSessionClosed[] =
    "bdfdbf0751415bd23c25fad2d640d159196b876692ec47e501e41faa4693fd5";
constexpr char kHashSessionPendingError[] =
    "b58b5ff1ed1c92987888ecb93ba0328b0a15fdd37959779b0f786ceb7a011f0";
constexpr char kHashSessionRecvList[] =
    "fadca82a94fb7e3dd8ee8585ae665ea0336b84b59efa9e6d7f840faba77f01a";
constexpr char kHashSessionState[] =
    "<c112f5a4e5f8cc4228e8080bc172ea148550e5783b3d84121e14fd9071cfc5e>k__BackingField";

size_t gOffNmSession = kFbNmSession;
size_t gOffNmSessionState = kFbNmSessionState;
size_t gOffNmPacketQueue = kFbNmPacketQueue;
size_t gOffNmOpcodeHashSet = kFbNmOpcodeHashSet;
size_t gOffSessionSeqSend = kFbSessionSeqSend;
size_t gOffSessionClosed = kFbSessionClosed;
size_t gOffSessionPendingError = kFbSessionPendingError;
size_t gOffSessionRecvList = kFbSessionRecvList;
size_t gOffSessionState = kFbSessionState;

std::atomic<bool> gTried{false};
char gPath[64]{};

bool PlausibleNm(size_t off) { return off >= 0x10 && off < 0x80; }
bool PlausibleSess(size_t off) { return off >= 0x10 && off < 0x100; }

size_t FieldOff(void* klass, const char* name) {
    if (!klass || !name || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return 0;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        field = nullptr;
    }
    if (!field) return 0;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        off = 0;
    }
    return off;
}

void ApplyOne(void* klass, const char* nm, size_t fb, size_t* out, bool (*ok)(size_t), int* hits) {
    if (!klass || !out || !hits) return;
    size_t got = FieldOff(klass, nm);
    if (got && ok && !ok(got)) got = 0;
    if (got) {
        *out = got;
        ++(*hits);
    } else {
        *out = fb;
    }
}

// 必须在 MainPump 上跑：Resolve klass + field_get_offset 可能触元数据/隐式类初始化。
// BIN 10:11：KickSniff worker 上 Ensure → GC「Collecting from unknown thread」。
void EnsureOnPump() {
    if (gTried.load(std::memory_order_acquire)) return;
    gTried.store(true, std::memory_order_release);
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(gPath, sizeof(gPath), "fallback");
        x::runtime::LogW("Il2CppNetwork", "field off: bind miss — dump fallback");
        return;
    }

    void* fac = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    void* sess = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    int hits = 0;
    ApplyOne(fac, kHashNmSession, kFbNmSession, &gOffNmSession, PlausibleNm, &hits);
    ApplyOne(fac, kHashNmSessionState, kFbNmSessionState, &gOffNmSessionState, PlausibleNm, &hits);
    ApplyOne(fac, kHashNmPacketQueue, kFbNmPacketQueue, &gOffNmPacketQueue, PlausibleNm, &hits);
    ApplyOne(fac, kHashNmOpcodeHashSet, kFbNmOpcodeHashSet, &gOffNmOpcodeHashSet, PlausibleNm,
             &hits);
    ApplyOne(sess, kHashSessionSeqSend, kFbSessionSeqSend, &gOffSessionSeqSend, PlausibleSess,
             &hits);
    ApplyOne(sess, kHashSessionClosed, kFbSessionClosed, &gOffSessionClosed, PlausibleSess, &hits);
    ApplyOne(sess, kHashSessionPendingError, kFbSessionPendingError, &gOffSessionPendingError,
             PlausibleSess, &hits);
    ApplyOne(sess, kHashSessionRecvList, kFbSessionRecvList, &gOffSessionRecvList, PlausibleSess,
             &hits);
    ApplyOne(sess, kHashSessionState, kFbSessionState, &gOffSessionState, PlausibleSess, &hits);

    constexpr int kExpect = 9;
    snprintf(gPath, sizeof(gPath), "%s",
             hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"));
    x::runtime::LogI(
        "Il2CppNetwork",
        "field off path=%s hits=%d/%d nm={sess=0x%zX st=0x%zX q=0x%zX hs=0x%zX} "
        "session={err=0x%zX recv=0x%zX st=0x%zX}",
        gPath, hits, kExpect, gOffNmSession, gOffNmSessionState, gOffNmPacketQueue,
        gOffNmOpcodeHashSet, gOffSessionPendingError, gOffSessionRecvList, gOffSessionState);
}

void EnsureJob(void* /*user*/) { EnsureOnPump(); }

}  // namespace

void Ensure() {
    if (gTried.load(std::memory_order_acquire)) return;
    if (x::runtime::main_thread::IsOnPumpThread()) {
        EnsureOnPump();
        return;
    }
    // 泵未装好时禁止 InvokeAndWait（其内部会再 Ensure/InstallPump，与 shape 冷解析互锁）。
    if (!x::runtime::main_thread::IsInstalled()) {
        EnsureOnPump();
        return;
    }
    // worker：投到泵上解析，禁止本线程碰 klass/field meta。
    if (!x::runtime::main_thread::InvokeAndWait(&EnsureJob, nullptr, 2500,
                                                 x::runtime::main_thread::JobPrio::High)) {
        if (gTried.exchange(true, std::memory_order_acq_rel)) return;
        snprintf(gPath, sizeof(gPath), "fallback-offpump");
        x::runtime::LogW("Il2CppNetwork",
                         "Ensure pump-wait fail — dump fallback (avoid GC unknown-thread)");
    }
}

void WarmForLoginWorkers() {
    // 在泵上把 FAC/NM/WM/UL klass + network 字段偏移一次算完；LOGIN workers 只读缓存。
    auto job = [](void*) {
        (void)x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
        (void)x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
        (void)x::runtime::il2cpp_shape::ResolveWorldManagerKlass();
        (void)x::runtime::il2cpp_shape::ResolveUserLocalKlass();
        EnsureOnPump();
        x::runtime::LogI("Il2CppNetwork", "WarmForLoginWorkers done path=%s",
                         gPath[0] ? gPath : "?");
    };
    if (x::runtime::main_thread::IsOnPumpThread()) {
        job(nullptr);
        return;
    }
    if (!x::runtime::main_thread::InvokeAndWait(job, nullptr, 2500,
                                                 x::runtime::main_thread::JobPrio::High)) {
        // 勿在此 Ensure()→fallback-offpump 永久钉死：冷启偶发 idle 拒排时还应允许后续 Off* 再试。
        x::runtime::LogW("Il2CppNetwork",
                         "WarmForLoginWorkers pump-wait fail — leave untried for later Ensure");
    }
}

size_t OffNmSession() {
    Ensure();
    return gOffNmSession;
}
size_t OffNmSessionState() {
    Ensure();
    return gOffNmSessionState;
}
size_t OffNmPacketQueue() {
    Ensure();
    return gOffNmPacketQueue;
}
size_t OffNmOpcodeHashSet() {
    Ensure();
    return gOffNmOpcodeHashSet;
}
size_t OffSessionSeqSend() {
    Ensure();
    return gOffSessionSeqSend;
}
size_t OffSessionClosed() {
    Ensure();
    return gOffSessionClosed;
}
size_t OffSessionPendingError() {
    Ensure();
    return gOffSessionPendingError;
}
size_t OffSessionRecvList() {
    Ensure();
    return gOffSessionRecvList;
}
size_t OffSessionState() {
    Ensure();
    return gOffSessionState;
}

}  // namespace x::runtime::il2cpp_network
