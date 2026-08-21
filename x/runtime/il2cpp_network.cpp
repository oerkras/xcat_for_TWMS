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
    "da73f1d4ed3f13f6f5b6b62758b6ac84d1796dec4993e6b4eeb48816ea28dcf";
constexpr char kHashNmSessionState[] =
    "ec581c162f3d359afbca1f1cbb2b4a3cf4fb9f79d191d7711d0f080813f5cde";
constexpr char kHashNmPacketQueue[] =
    "a67560f33546a1c8a013d549e8086d255523725156e2e758147e907268e60a1";
constexpr char kHashNmOpcodeHashSet[] =
    "aa79b5c7e84d4506a2618521e37aaf5877b26f68dcad68ec5b189b74d37bfba";

// Session fields
constexpr char kHashSessionSeqSend[] =
    "f5ed859e0d339b1640f6604d92b1a605cc9ea372953b37d7b956ab7a1169a40";
constexpr char kHashSessionClosed[] =
    "de059a3abec7948f43491f624f9b89c29d437df5c56e632ddef49f2d328e936";
constexpr char kHashSessionPendingError[] =
    "b13993988658ac28ad0880440258fca164873f697256fa08142884753c3856b";
constexpr char kHashSessionRecvList[] =
    "aefc5e0c55c3548e5b427eec96bf72a49ac81e2ecd18689de9e9ab9f57a974b";
constexpr char kHashSessionState[] =
    "<fbc062330944059859fd631620021d4f0423a45f5ccd6fbb20cd71164886796>k__BackingField";

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
