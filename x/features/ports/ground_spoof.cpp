// Classic TWMS — 站立伪装（详见 ground_spoof.h 的锚点与不冲突论证）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "ground_spoof.h"

#include "fly_fh_ban.h"
#include "foothold_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>

namespace x::features::ports::ground_spoof {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;

// 与 fly_fh_ban / foothold_port / attack_input_port 同源 fb。
constexpr size_t kFbUserVecCtrl = 0x50;
constexpr size_t kFbVcCurFh = 0x28;
constexpr size_t kFbFhId = 0x10;

std::atomic<bool> gEnabled{false};

// 起飞时禁台钩子清掉的那块台（只存 ID，换图后解析不到就不种）。
std::atomic<uint32_t> gLastFhId{0};

// 解析结果缓存：ResolveFhObject 是整字典线性扫，别每刀都扫。
// 换图即失效——mapId 变了就重解析，所以缓存指针不会跨图悬垂。
void* gResolvedFh = nullptr;
uint32_t gResolvedId = 0;
int gResolvedMap = 0;
// 失败也要记：换图后旧 ID 永远解析不到，不记就变成「每刀全字典扫一遍」，
// 8 刀/秒 × 上千条 fh 全压在主线程泵的阻塞 job 里。
uint32_t gFailedId = 0;
int gFailedMap = 0;
DWORD gFailedAtMs = 0;
// 负缓存留个到期时间：进图那几拍 MapData 可能还没铺好 fh，一次失败就永久放弃
// 会让功能在整张图上静默失效。3s 重试一次，最坏也就每 3s 一次字典扫。
constexpr DWORD kFailRetryMs = 3000;

// 本次现场（只主线程泵写，*Debug 读；atomics 让面板/其它线程读也安全）。
// 「种了哪块台」两路共用（顺序执行、不嵌套）；判决位按路分开，免得互相冲掉取证。
void* gPlantedVc = nullptr;
void* gPlantedFh = nullptr;
std::atomic<int> gVerdict{0};
std::atomic<uint32_t> gPlantedId{0};
std::atomic<int> gCastVerdict{0};
std::atomic<uint32_t> gCastPlantedId{0};

void* ReadPtrSeh(void* base, size_t off) {
    if (!base) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint32_t ReadU32Seh(void* base, size_t off) {
    if (!base) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(base) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool WritePtrSeh(void* base, size_t off, void* v) {
    if (!base) return false;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + off) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 只在种台那一刻调（≈出刀频率），不在物理帧上。
void* ResolveCached(uint32_t id) {
    if (!id) return nullptr;
    const int map = world::GetMapId();
    if (gResolvedFh && gResolvedId == id && gResolvedMap == map && LooksLikeHeapPtr(gResolvedFh))
        return gResolvedFh;
    // 这一对（id, map）刚查过且没查到，别再扫一遍字典；换图/换台/到期自然放行。
    const DWORD now = GetTickCount();
    if (gFailedId == id && gFailedMap == map &&
        static_cast<DWORD>(now - gFailedAtMs) < kFailRetryMs)
        return nullptr;
    void* fh = foothold::ResolveFhObject(id);
    if (!LooksLikeHeapPtr(fh)) {
        gResolvedFh = nullptr;
        gResolvedId = 0;
        gResolvedMap = 0;
        gFailedId = id;
        gFailedMap = map;
        gFailedAtMs = now;
        return nullptr;
    }
    gResolvedFh = fh;
    gResolvedId = id;
    gResolvedMap = map;
    gFailedId = 0;
    gFailedMap = 0;
    return fh;
}

bool PlantImpl(void* localUser, std::atomic<int>& verdictSlot, std::atomic<uint32_t>& idSlot) {
    const auto finish = [&verdictSlot](int verdict) {
        verdictSlot.store(verdict, std::memory_order_relaxed);
    };

    gPlantedVc = nullptr;
    gPlantedFh = nullptr;
    idSlot.store(0, std::memory_order_relaxed);

    void* vc = LooksLikeHeapPtr(localUser) ? ReadPtrSeh(localUser, kFbUserVecCtrl) : nullptr;
    if (!LooksLikeHeapPtr(vc)) {
        finish(-4);
        return false;
    }
    gPlantedVc = vc;

    if (!gEnabled.load(std::memory_order_acquire)) {
        finish(0);
        return false;
    }
    // 没在飞就没什么好伪装的：真在台上时引擎自己的 CurFh 才是对的，别覆盖。
    if (!fly_fh_ban::IsBanActive()) {
        finish(-2);
        return false;
    }
    if (LooksLikeHeapPtr(ReadPtrSeh(vc, kFbVcCurFh))) {
        finish(-1);
        return false;
    }

    void* fh = ResolveCached(gLastFhId.load(std::memory_order_acquire));
    if (!LooksLikeHeapPtr(fh) || !WritePtrSeh(vc, kFbVcCurFh, fh)) {
        finish(-3);
        return false;
    }
    gPlantedFh = fh;
    idSlot.store(gResolvedId, std::memory_order_relaxed);
    finish(1);
    return true;
}

}  // namespace

void SetEnabled(bool on) {
    const bool prev = gEnabled.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;  // IPC 每拍下发全量配置，没变就别刷日志
    x::runtime::LogI("GroundSpoof", "standing disguise %s", on ? "ON" : "OFF");
}

bool IsEnabled() { return gEnabled.load(std::memory_order_acquire); }

void NoticeFhObject(void* fh) {
    if (!LooksLikeHeapPtr(fh)) return;
    const uint32_t id = ReadU32Seh(fh, kFbFhId);
    if (id) gLastFhId.store(id, std::memory_order_release);
}

bool PlantForFire(void* localUser) { return PlantImpl(localUser, gVerdict, gPlantedId); }

bool PlantForCast(void* localUser) { return PlantImpl(localUser, gCastVerdict, gCastPlantedId); }

void UnplantAfterFire() {
    void* vc = gPlantedVc;
    if (!vc || !gPlantedFh) return;
    // 只在还是我们种的那块时才摘：中途要是引擎自己挂上了别的台，留给它。
    if (ReadPtrSeh(vc, kFbVcCurFh) == gPlantedFh) WritePtrSeh(vc, kFbVcCurFh, nullptr);
    gPlantedFh = nullptr;
}

void FireDebug(int* verdict, uint32_t* fhId) {
    if (verdict) *verdict = gVerdict.load(std::memory_order_relaxed);
    if (fhId) *fhId = gPlantedId.load(std::memory_order_relaxed);
}

void CastDebug(int* verdict, uint32_t* fhId) {
    if (verdict) *verdict = gCastVerdict.load(std::memory_order_relaxed);
    if (fhId) *fhId = gCastPlantedId.load(std::memory_order_relaxed);
}

}  // namespace x::features::ports::ground_spoof
