// TWMS Classic — 一次调用官方状态栏拍卖按钮（实验，默认关）。
//
// 0820：UIStatusBar.OnClickButton(17 / CnNpt) 是状态栏真入口；体内
// CheckRedAccountRestriction 之后直调 SendMigrateToGlobalMarketRequest
// （RVA 0xDFB9D0）。BIN 08-21：假等级/假建角时间骗不过服端，且会污染战斗真源。
// 本探针不再写字段，只在 Unity 主泵上点官方按钮。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "auction_gate_probe.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../ports/world_port.h"

#include <atomic>
#include <cstdint>

namespace x::features::auction_gate_probe {

// UIStatusBar（dump class hash）；OnClickButton 明文仍在 script.json
constexpr char kHashStatusBar[] =
    "e586a5254f41bdc4112064bc295980a39d5eb73ad06d1b178a4b758d67a8648";
constexpr uint32_t kRvaOnClickButton = 0x66C7A0;  // 08-20 dump；hash/plain 优先
constexpr int32_t kBtnAuction = 17;               // CMS CnNpt
constexpr DWORD kPumpTimeoutMs = 4000;

namespace {

namespace il2 = x::runtime::il2cpp;
namespace world = x::features::ports::world;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<bool> gBusy{false};
std::atomic<bool> gStop{false};
std::atomic<bool> gJobDone{true};

void* PickStatusBar(void* klass) {
    if (!klass || !il2::Ensure()) return nullptr;
    const auto& e = il2::Get();
    if (!e.findAll) return nullptr;
    void* typeObj = il2::ClassTypeObjectOnMain(klass);
    if (!typeObj) return nullptr;
    void* arr = nullptr;
    __try {
        arr = e.findAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(
            "auction_gate_probe.FindAll");
        arr = nullptr;
    }
    const uintptr_t n = il2::ArrayLen(arr);
    for (uintptr_t i = 0; i < n && i < 8; ++i) {
        void* o = il2::ArrayAt(arr, i);
        if (il2::LooksLikeHeapPtr(o)) return o;
    }
    return nullptr;
}

void PumpJob(void* /*user*/) {
    if (!world::IsPlayReady()) {
        x::runtime::LogW("AuctionGateProbe", "skip: not play-ready");
        return;
    }
    const auto st = world::GetSceneState();
    if (st == world::SceneState::GlobalMarket) {
        x::runtime::LogW("AuctionGateProbe", "skip: already GlobalMarket");
        return;
    }

    void* klass = il2::FindClass("", kHashStatusBar);
    if (!klass) {
        x::runtime::LogW("AuctionGateProbe", "skip: UIStatusBar klass miss");
        return;
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kSh{1, TypeKind::Void, true, true, {TypeKind::I32}};
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        klass, kRvaOnClickButton, kSh, "OnClickButton", nullptr);
    auto* mi = reinterpret_cast<MethodInfoHead*>(mr.method);
    void* fnp = nullptr;
    if (mi) {
        __try {
            fnp = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            fnp = nullptr;
        }
    }
    if (!fnp) fnp = il2::AtRva<void*>(kRvaOnClickButton);
    if (!fnp) {
        x::runtime::LogW("AuctionGateProbe", "skip: OnClickButton miss path=%s",
                         x::runtime::il2cpp_method::PathName(mr.path));
        return;
    }

    void* bar = PickStatusBar(klass);
    if (!il2::LooksLikeHeapPtr(bar)) {
        x::runtime::LogW("AuctionGateProbe", "skip: UIStatusBar instance miss");
        return;
    }

    x::runtime::LogI("AuctionGateProbe",
                     "native OnClickButton(%d) bar=%p map=%d path=%s (no spoof)",
                     kBtnAuction, bar, world::GetMapId(),
                     x::runtime::il2cpp_method::PathName(mr.path));

    bool called = false;
    bool excepted = false;
    using FnClick = void (*)(void* self, int32_t buttonId, void* methodInfo);
    __try {
        reinterpret_cast<FnClick>(fnp)(bar, kBtnAuction, mr.method);
        called = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        excepted = true;
        x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(
            "auction_gate_probe.OnClickButton");
    }

    x::runtime::LogI("AuctionGateProbe",
                     "native click done called=%d except=%d "
                     "(official client gates still apply; server 0x002E authoritative)",
                     called ? 1 : 0, excepted ? 1 : 0);
}

void PumpJobThunk(void* user) {
    PumpJob(user);
    gJobDone.store(true, std::memory_order_release);
}

DWORD WINAPI ProbeThread(LPVOID) {
    if (gStop.load(std::memory_order_relaxed)) {
        gBusy.store(false, std::memory_order_release);
        return 0;
    }
    if (!x::runtime::main_thread::Ensure() || !x::runtime::main_thread::IsInstalled()) {
        x::runtime::LogW("AuctionGateProbe", "MainPump not installed");
        gBusy.store(false, std::memory_order_release);
        return 0;
    }
    gJobDone.store(false, std::memory_order_release);
    const bool ok = x::runtime::main_thread::InvokeAndWait(
        &PumpJobThunk, nullptr, kPumpTimeoutMs, x::runtime::main_thread::JobPrio::High);
    if (!ok) {
        x::runtime::LogW("AuctionGateProbe",
                         "InvokeAndWait failed/timeout %ums (换图 quiesce 或点击慢)",
                         static_cast<unsigned>(kPumpTimeoutMs));
        for (int i = 0; i < 100 && !gJobDone.load(std::memory_order_acquire); ++i) Sleep(20);
        gJobDone.store(true, std::memory_order_release);
    }
    gBusy.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

void Init() {
    x::runtime::LogI("AuctionGateProbe",
                     "init — one-shot native UIStatusBar.OnClickButton(%d); "
                     "no fake level/date; RVA 0x%X fallback",
                     kBtnAuction, kRvaOnClickButton);
}

void Shutdown() {
    gStop.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && gBusy.load(std::memory_order_acquire); ++i) Sleep(20);
}

void RequestRun() {
    if (gStop.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!gBusy.compare_exchange_strong(expected, true)) {
        x::runtime::LogW("AuctionGateProbe", "busy — refuse overlapping probe");
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, ProbeThread, nullptr, 0, nullptr);
    if (!th) {
        gBusy.store(false, std::memory_order_release);
        x::runtime::LogW("AuctionGateProbe", "CreateThread failed");
        return;
    }
    CloseHandle(th);
}

}  // namespace x::features::auction_gate_probe
