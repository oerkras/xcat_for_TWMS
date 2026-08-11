// TWMS Classic — auction_town_bypass.
//
// SendMigrateToGlobalMarketRequest (RVA 0xDDD620 @ remount 2026-08-04; was 0xDD8610)
// gates on MapDataInfo:
//   1) IsUnableToMigrate ≡ (Option & 0x10) != 0
//   2) IsTown
// Status-bar uses a direct call (MI swap ineffective). Zero .text:
// keep Field/WM Info.IsTown=1 and clear Option bit 0x10 while enabled.
// Restore on disable / map change / stop.
// Server may refuse / disconnect (GlobalMarketTerminated); with 守护模式 that
// triggers clean relaunch — not NGS. Default on; turn off under hangup watchdog.
//
// Anti-drift: resolve field offsets at runtime via class/field name hashes +
// il2cpp_field_get_offset; dump-verified constants are fallback only.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "auction_town_bypass.h"

#include "../ports/world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"

#include <atomic>
#include <cstdint>

namespace x::features::auction_town_bypass {
namespace {

namespace il2 = x::runtime::il2cpp;
namespace world = x::features::ports::world;
namespace shape = x::runtime::il2cpp_shape;

// Dump-verified fallbacks (remount 2026-08-04; same as 08-03 layout).
constexpr size_t kFbWmField = 0x58;
constexpr size_t kFbWmMapData = 0x88;
constexpr size_t kFbFieldMapData = 0x38;
constexpr size_t kFbMapDataInfo = 0x18;
constexpr size_t kFbInfoIsTown = 0x50;
constexpr size_t kFbInfoOption = 0x5C;
constexpr uint32_t kUnableMigrateBit = 0x10u;

// TypeDef hashes (dump.cs 2026-08-04).
constexpr char kHashMapDataInfo[] =
    "a5fbae684ca9f39b46ece8fc0599b711f8e42ceb90ae91f2c0c9d1e65c38710";
constexpr char kHashMapData[] =
    "a2eca01ad670b3c4b4ba85636ed1dfa4dc01ac28c03a91163597f3e0c9f4cf1";
constexpr char kHashSceneMap[] =
    "a0e81e802b84e652108c9ee7f8c05d725ea5198ae5b3ac01471779364217bfb";
// IsTown / Option / Info / MapData backing / WM._field / WM._currentMapData
constexpr char kHashIsTown[] =
    "a0b6dac350c4a664a60c84dc90562fc5e127ab592ec77dbd7f08b7a25ebfeb3";
constexpr char kHashOption[] =
    "a47df7b10bff5609aa05203b6f844beb94245afcbb4a848e47c9e8d289e70b3";
constexpr char kHashMapDataInfoField[] =
    "d08fedab3887df3ffd2354535aa3baa385c9e49c098e1911177ebe43144150b";
constexpr char kHashSceneMapDataBacking[] =
    "<bcab656236804c9b54d1bee33c5176eb9fa13c123b9787bd7ff0ae4903722ca>k__BackingField";
constexpr char kHashWmField[] =
    "cd05676f83eff32a7a754d6c6287f124ca239987ddd9c57667f9cce26502a0e";
constexpr char kHashWmCurrentMapData[] =
    "ca7537d2fae3d15edc28a3fb9695328e94aedb3358c69a3c33807338687b5b9";

constexpr DWORD kTickMsApply = 50;    // 未稳住 / 换图：快拍一次写到位
constexpr DWORD kTickMsHold = 1000;   // 已稳住：慢校验（游戏一般不回写 IsTown）
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kLogMs = 15000;

using FnFieldFromName = void* (*)(void* klass, const char* name);

struct GateOff {
    size_t wmField = kFbWmField;
    size_t wmMapData = kFbWmMapData;
    size_t fieldMapData = kFbFieldMapData;
    size_t mapDataInfo = kFbMapDataInfo;
    size_t infoIsTown = kFbInfoIsTown;
    size_t infoOption = kFbInfoOption;
    bool tried = false;
    const char* path = "fallback";  // meta | meta-partial | fallback
};

GateOff gOff{};
FnFieldFromName gFieldFromName = nullptr;

std::atomic<bool> gDesired{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
// Worker-only: true when gates already applied on current Info and last check ok.
bool gHolding = false;

// All gBak / heap writes happen only on the worker thread (Tick/Worker exit).
// SetEnabled only flips gDesired; StopWorker joins then restores once.
struct InfoBackup {
    void* info = nullptr;
    uint8_t savedIsTown = 0;
    uint32_t savedUnableBit = 0;  // 0 or kUnableMigrateBit only
    bool have = false;
};
InfoBackup gBak[2]{};

DWORD gLastLogMs = 0;
std::atomic<uint32_t> gForceHits{0};
std::atomic<uint32_t> gWriteHits{0};  // actual memory writes (not no-op holds)
// Worker Tick 发布；QueryNativeIsTown 只读此原子，避免跨线程碰 gBak。
std::atomic<int> gNativeIsTown{-1};

bool PlausibleInstanceOff(size_t off) {
    // Managed instance fields sit past object header; reject nonsense.
    return off >= 0x10 && off < 0x1000;
}

// Returns true when metadata supplied a plausible offset (may equal fallback).
bool FieldOffOrFb(void* klass, const char* fieldHash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !fieldHash || !gFieldFromName) return false;
    const auto& e = il2::Get();
    if (!e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = gFieldFromName(klass, fieldHash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!PlausibleInstanceOff(off)) return false;
    *out = off;
    return true;
}

void EnsureGateOffsets() {
    if (gOff.tried) return;
    gOff.tried = true;

    HMODULE ga = il2::GameAssembly();
    if (!ga || !il2::Ensure()) {
        x::runtime::LogW("AuctionTown", "offset resolve: GA/bind miss — using dump fallbacks");
        return;
    }
    if (!gFieldFromName) {
        gFieldFromName = reinterpret_cast<FnFieldFromName>(
            GetProcAddress(ga, "il2cpp_class_get_field_from_name"));
    }
    if (!gFieldFromName || !il2::Get().fieldGetOffset) {
        x::runtime::LogW("AuctionTown", "offset resolve: field exports miss — using dump fallbacks");
        return;
    }

    void* wmKlass = shape::ResolveWorldManagerKlass();
    void* infoKlass = il2::FindClass("", kHashMapDataInfo);
    void* mapKlass = il2::FindClass("", kHashMapData);
    void* sceneKlass = il2::FindClass("", kHashSceneMap);

    int hits = 0;
    size_t oWmF = kFbWmField, oWmM = kFbWmMapData, oFM = kFbFieldMapData;
    size_t oInfo = kFbMapDataInfo, oTown = kFbInfoIsTown, oOpt = kFbInfoOption;
    if (FieldOffOrFb(wmKlass, kHashWmField, kFbWmField, &oWmF)) ++hits;
    if (FieldOffOrFb(wmKlass, kHashWmCurrentMapData, kFbWmMapData, &oWmM)) ++hits;
    if (FieldOffOrFb(sceneKlass, kHashSceneMapDataBacking, kFbFieldMapData, &oFM)) ++hits;
    if (FieldOffOrFb(mapKlass, kHashMapDataInfoField, kFbMapDataInfo, &oInfo)) ++hits;
    if (FieldOffOrFb(infoKlass, kHashIsTown, kFbInfoIsTown, &oTown)) ++hits;
    if (FieldOffOrFb(infoKlass, kHashOption, kFbInfoOption, &oOpt)) ++hits;

    gOff.wmField = oWmF;
    gOff.wmMapData = oWmM;
    gOff.fieldMapData = oFM;
    gOff.mapDataInfo = oInfo;
    gOff.infoIsTown = oTown;
    gOff.infoOption = oOpt;
    gOff.path = hits == 6 ? "meta" : (hits ? "meta-partial" : "fallback");

    x::runtime::LogI(
        "AuctionTown",
        "offsets path=%s hits=%d/6 wmF=0x%zx wmM=0x%zx fieldMD=0x%zx info=0x%zx "
        "IsTown=0x%zx Option=0x%zx (migrate RVA 0xDDD620 / op 0x002E)",
        gOff.path, hits, gOff.wmField, gOff.wmMapData, gOff.fieldMapData,
        gOff.mapDataInfo, gOff.infoIsTown, gOff.infoOption);
}

void RestoreOne(InfoBackup& b) {
    if (!b.have || !b.info) {
        b.have = false;
        b.info = nullptr;
        return;
    }
    if (il2::LooksLikeHeapPtr(b.info)) {
        __try {
            auto* base = reinterpret_cast<uint8_t*>(b.info);
            *reinterpret_cast<uint8_t*>(base + gOff.infoIsTown) = b.savedIsTown;
            uint32_t* opt = reinterpret_cast<uint32_t*>(base + gOff.infoOption);
            *opt = (*opt & ~kUnableMigrateBit) | (b.savedUnableBit & kUnableMigrateBit);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    b.have = false;
    b.info = nullptr;
}

void RestoreAllBackups() {
    RestoreOne(gBak[0]);
    RestoreOne(gBak[1]);
}

bool ForceInfoGates(void* info, InfoBackup& bak) {
    if (!il2::LooksLikeHeapPtr(info)) return false;
    uint8_t curTown = 0;
    uint32_t curOpt = 0;
    __try {
        auto* base = reinterpret_cast<uint8_t*>(info);
        curTown = *reinterpret_cast<uint8_t*>(base + gOff.infoIsTown);
        curOpt = *reinterpret_cast<uint32_t*>(base + gOff.infoOption);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (bak.info != info) {
        RestoreOne(bak);
        bak.info = info;
        bak.savedIsTown = curTown;
        bak.savedUnableBit = curOpt & kUnableMigrateBit;
        bak.have = true;
    }

    // Already holding desired gates — no write (hot path when Sleep=1s).
    if (curTown == 1 && (curOpt & kUnableMigrateBit) == 0) return true;

    __try {
        auto* base = reinterpret_cast<uint8_t*>(info);
        *reinterpret_cast<uint8_t*>(base + gOff.infoIsTown) = 1;
        uint32_t* opt = reinterpret_cast<uint32_t*>(base + gOff.infoOption);
        *opt = *opt & ~kUnableMigrateBit;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RestoreOne(bak);
        return false;
    }
    gWriteHits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void* InfoFromMapData(void* mapData) {
    if (!il2::LooksLikeHeapPtr(mapData)) return nullptr;
    return il2::ReadPtr(mapData, gOff.mapDataInfo);
}

bool ForceGatesOnCurrentMap(DWORD now) {
    EnsureGateOffsets();
    if (!world::IsPlayReady()) {
        RestoreAllBackups();
        gHolding = false;
        return false;
    }
    void* wm = world::GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) {
        RestoreAllBackups();
        gHolding = false;
        return false;
    }

    void* infoWm = InfoFromMapData(il2::ReadPtr(wm, gOff.wmMapData));
    void* field = il2::ReadPtr(wm, gOff.wmField);
    void* infoField = nullptr;
    if (il2::LooksLikeHeapPtr(field))
        infoField = InfoFromMapData(il2::ReadPtr(field, gOff.fieldMapData));

    bool ok = false;
    if (infoField)
        ok = ForceInfoGates(infoField, gBak[0]) || ok;
    if (infoWm && infoWm != infoField)
        ok = ForceInfoGates(infoWm, gBak[1]) || ok;
    else if (infoWm && !infoField)
        ok = ForceInfoGates(infoWm, gBak[0]) || ok;

    if (!ok) {
        RestoreAllBackups();
        gHolding = false;
        return false;
    }

    gHolding = true;
    gForceHits.fetch_add(1, std::memory_order_relaxed);

    uint8_t townNow = 0;
    uint32_t optionNow = 0;
    void* infoLog = infoField ? infoField : infoWm;
    __try {
        auto* base = reinterpret_cast<uint8_t*>(infoLog);
        townNow = *reinterpret_cast<uint8_t*>(base + gOff.infoIsTown);
        optionNow = *reinterpret_cast<uint32_t*>(base + gOff.infoOption);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    if (!gLastLogMs || now - gLastLogMs >= kLogMs) {
        gLastLogMs = now;
        const uint8_t savedTown =
            (gBak[0].have && gBak[0].info == infoLog) ? gBak[0].savedIsTown
            : (gBak[1].have && gBak[1].info == infoLog) ? gBak[1].savedIsTown
                                                         : 0;
        const uint32_t savedBit =
            (gBak[0].have && gBak[0].info == infoLog) ? gBak[0].savedUnableBit
            : (gBak[1].have && gBak[1].info == infoLog) ? gBak[1].savedUnableBit
                                                         : 0;
        x::runtime::LogI(
            "AuctionTown",
            "data-plane mapId=%d infoF=%p infoW=%p same=%d IsTown %u->%u "
            "unableBit %u->%u checks=%u writes=%u off=%s hold=1s "
            "(zero .text; client-only)",
            world::GetMapId(), infoField, infoWm,
            (infoField && infoField == infoWm) ? 1 : 0,
            static_cast<unsigned>(savedTown), static_cast<unsigned>(townNow),
            (savedBit ? 1u : 0u), ((optionNow & kUnableMigrateBit) ? 1u : 0u),
            gForceHits.load(), gWriteHits.load(), gOff.path);
    }
    return true;
}

void* CurrentMapDataInfo() {
    void* wm = world::GetWorldManager();
    if (!il2::LooksLikeHeapPtr(wm)) return nullptr;
    void* field = il2::ReadPtr(wm, gOff.wmField);
    if (il2::LooksLikeHeapPtr(field)) {
        void* infoField = InfoFromMapData(il2::ReadPtr(field, gOff.fieldMapData));
        if (infoField) return infoField;
    }
    return InfoFromMapData(il2::ReadPtr(wm, gOff.wmMapData));
}

// Worker 线程：采样原生 IsTown 到原子（bypass 持有时用 bak.savedIsTown）。
void PublishNativeIsTownSample() {
    EnsureGateOffsets();
    if (!world::IsPlayReady()) {
        gNativeIsTown.store(-1, std::memory_order_relaxed);
        return;
    }
    void* info = CurrentMapDataInfo();
    if (!il2::LooksLikeHeapPtr(info)) {
        gNativeIsTown.store(-1, std::memory_order_relaxed);
        return;
    }

    int native = -1;
    if (gDesired.load(std::memory_order_relaxed)) {
        if (gBak[0].have && gBak[0].info == info)
            native = gBak[0].savedIsTown ? 1 : 0;
        else if (gBak[1].have && gBak[1].info == info)
            native = gBak[1].savedIsTown ? 1 : 0;
        else {
            // 绕过已开但 bak 尚未对齐本帧 Info：勿读被强制写成 1 的活值（会把野外当主城）。
            gNativeIsTown.store(-1, std::memory_order_relaxed);
            return;
        }
    }
    if (native < 0) {
        uint8_t cur = 0;
        bool ok = false;
        __try {
            cur = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(info) + gOff.infoIsTown);
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) {
            gNativeIsTown.store(-1, std::memory_order_relaxed);
            return;
        }
        native = cur ? 1 : 0;
    }
    gNativeIsTown.store(native, std::memory_order_relaxed);
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("AuctionTown",
                     "worker start — zero .text; apply-once + 1s hold check "
                     "(no click hook: status-bar is direct call); default on");
    for (int i = 0; i < 400 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll");
         ++i)
        Sleep(50);
    Tick(GetTickCount());
    while (!gStop.load()) {
        const bool on = gDesired.load();
        Tick(GetTickCount());
        DWORD sleepMs = kTickMsOff;
        if (on) sleepMs = gHolding ? kTickMsHold : kTickMsApply;
        Sleep(sleepMs);
    }
    RestoreAllBackups();
    gHolding = false;
    x::runtime::LogI("AuctionTown", "worker stop checks=%u writes=%u", gForceHits.load(),
                     gWriteHits.load());
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(false);
    x::runtime::LogI("AuctionTown",
                     "init — field auction client bypass via MapDataInfo gates only "
                     "(no .text); migrate RVA 0xDDD620 / op 0x002E; "
                     "offsets via hash+field_get_offset; default on");
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gWorker.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) gWorker.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gWorker.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
    // Worker already restored on exit; belt-and-suspenders if join timed out.
    RestoreAllBackups();
}

void SetEnabled(bool on) {
    const bool prev = gDesired.exchange(on);
    if (prev == on) return;
    x::runtime::LogI("AuctionTown", "SetEnabled %d", on ? 1 : 0);
    // Restore only on worker Tick when gDesired==false (avoids cross-thread gBak races).
}

bool IsEnabled() { return gDesired.load(); }

void Tick(DWORD now) {
    if (!gDesired.load(std::memory_order_relaxed)) {
        RestoreAllBackups();
        gHolding = false;
        PublishNativeIsTownSample();
        return;
    }
    (void)ForceGatesOnCurrentMap(now);
    PublishNativeIsTownSample();
}

int QueryNativeIsTown() { return gNativeIsTown.load(std::memory_order_relaxed); }

}  // namespace x::features::auction_town_bypass
