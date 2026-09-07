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
    "c16208d2417f4394cc74f1071c8079b60ee678709f60677c1d587fe0ea9647b";
constexpr char kHashNmSessionState[] =
    "e63dd27f14dac8dc72175e743b313999823529b2722572b6400671fee29ed7e";
constexpr char kHashNmPacketQueue[] =
    "bdd50ee36b8cbf8aad3a9b7f36db854a0835e5df80046c5de7dd36e7ecff5b5";
constexpr char kHashNmOpcodeHashSet[] =
    "dbd6d653a6c91bea2a157ebb2bea027d2923dbb37099d3b5659d4a0ae653c67";

// Session fields
constexpr char kHashSessionSeqSend[] =
    "b3928ba27bcb11ead2e69665541410197b2787b424764d7ad096c4574cee387";
constexpr char kHashSessionClosed[] =
    "ac63b0e456eeefe059d77571a30a6186eee34070c5f900f45c11319a62c8c34";
constexpr char kHashSessionPendingError[] =
    "c02581e3cf68697381ec50ea3bb0c02752f5b1ff594d079db04c971bcc95bdf";
constexpr char kHashSessionRecvList[] =
    "ec4b2d3d228a5bfe9ad40e2c6e249b7b1f9a07bf07e501103d85db54d86592a";
constexpr char kHashSessionState[] =
    "<daec067ec8acc136172cc5443d26376c3cc8969b1cc481e935c214144966848>k__BackingField";

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
