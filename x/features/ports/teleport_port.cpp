// Classic TWMS — 唯一产品瞬移：手填 Teleport + TryDoingTeleport（fill+Doing）
// 不含 ImpactNext / Attr=4 / SyncRel settle / Register 技能包。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "teleport_port.h"

#include "foothold_path.h"
#include "foothold_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace x::features::ports::teleport {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaTryDoingTeleport = 0x101ED30;  // remapped 2026-08-03
constexpr uint32_t kRvaVecCtrlSetForcedFlush = 0x119F7D0;  // remapped 2026-08-03
constexpr uint32_t kRvaMovePathSetForcedFlush = 0x11964E0;  // remapped 2026-08-03
constexpr char kHashTryDoingTeleport[] =
    "c891da90ab112a63128f2213731eb5efce8e9431fe09d64abc0d34f835b8bca";
constexpr char kHashVecCtrlSetForcedFlush[] =
    "dd0bb66db2a220aaace72406ec37aa235004600cba4a32d5dde9a67c79dcd7e";
constexpr char kHashMovePathSetForcedFlush[] =
    "ab7ec1f1952e7c608957abc1c9a3fe69f6bbfcb84697247b40082943eb0f76f";
constexpr char kVecCtrlClass[] =
    "d5ce57ae29519b9d8ea3e23c7f00e3995b1c02048eb8093dff28802f6cb9598";
constexpr char kMovePathClass[] =
    "c97a22c2794eea74004a5e051e8e2b1ec1d868efbf283342128741af40b64ea";
// True TW UserLocal = User subclass with Teleport@0x3C8（resolve: il2cpp_shape）

constexpr size_t kOffVecCtrl = 0x50;
constexpr size_t kOffVcCurFh = 0x28;
constexpr size_t kOffVcLastFh = 0x30;
constexpr size_t kOffVcMovePath = 0x78;
constexpr size_t kOffVcApX = 0x98;
constexpr size_t kOffVcApY = 0xA0;
constexpr size_t kOffVcAplX = 0xB8;
constexpr size_t kOffVcAplY = 0xC0;
constexpr size_t kOffVcMoveAction = 0x84;
constexpr size_t kOffMpForcedFlush = 0x48;
constexpr size_t kOffMpX = 0x10;
constexpr size_t kOffMpY = 0x12;

// UserLocal.Teleport pending — Doing 读取
constexpr size_t kOffTeleportIsValid = 0x3C8;
constexpr size_t kOffTeleportByPortal = 0x3C9;
constexpr size_t kOffTeleportPosX = 0x3CC;
constexpr size_t kOffTeleportPosY = 0x3D0;
constexpr size_t kOffTeleportStartTick = 0x3D4;
constexpr size_t kOffTeleportCoolTimeEnd = 0x3D8;

constexpr DWORD kNativeSelfCdMinMs = 5;
constexpr DWORD kNativeSelfCdMaxMs = 8000;
constexpr float kNativeShortHopPx = 140.f;
constexpr DWORD kJobWaitMs = 2000;

std::atomic<uint32_t> gNativeCdMs{kNativeSelfCdMinMs};
DWORD gLastNativeMs = 0;

using FnTryDoingTeleport = void (*)(void* self, const void* method);
using FnSetForcedFlush = void (*)(void* self, const void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

FnTryDoingTeleport gDoing = nullptr;
FnSetForcedFlush gSetFlush = nullptr;
FnSetForcedFlush gMpSetFlush = nullptr;

void* gGA = nullptr;
void* gLocalUserKlass = nullptr;
void* gMiDoing = nullptr;
std::atomic<bool> gBound{false};

template <typename T>
T AtRva(uint32_t rva) {
    return x::runtime::il2cpp::AtRva<T>(rva);
}

void WriteF64(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

void WriteF32(void* obj, size_t off, float v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteBool(void* obj, size_t off, bool v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReadBool(void* obj, size_t off) {
    if (!obj) return false;
    __try {
        return *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WriteI16(void* obj, size_t off, int16_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteI32(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WritePtr(void* obj, size_t off, void* v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool BindFns() {
    if (gBound.load(std::memory_order_acquire) && gDoing) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    gGA = x::runtime::il2cpp::GameAssembly();
    if (!gGA) return false;
    gLocalUserKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();

    auto findByName = [](void* klass, const char* name, int argc) -> void* {
        if (!klass || !name) return nullptr;
        const auto& e = x::runtime::il2cpp::Get();
        if (e.classGetMethodFromName) {
            void* mi = nullptr;
            __try {
                mi = e.classGetMethodFromName(klass, name, argc);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                mi = nullptr;
            }
            if (mi) return mi;
        }
        if (!e.classGetMethods || !e.methodGetName) return nullptr;
        void* iter = nullptr;
        __try {
            for (;;) {
                void* raw = e.classGetMethods(klass, &iter);
                if (!raw) break;
                const char* nm = e.methodGetName(raw);
                if (nm && strcmp(nm, name) == 0) return raw;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        return nullptr;
    };

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (gLocalUserKlass) {
        constexpr MethodShape kDoing{0, TypeKind::Void, true, true, {}};
        void* mi = findByName(gLocalUserKlass, "TryDoingTeleport", 0);
        if (!mi) mi = findByName(gLocalUserKlass, kHashTryDoingTeleport, 0);
        if (!mi) {
            const auto mr = x::runtime::il2cpp_method::FindMethodCached(
                gLocalUserKlass, kRvaTryDoingTeleport, kDoing);
            mi = mr.method;
            if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
                x::runtime::LogI("Teleport", "Doing MethodInfo via kind");
            }
        }
        gMiDoing = mi;
        if (mi) {
            auto* head = reinterpret_cast<MethodInfoHead*>(mi);
            if (head->methodPointer) gDoing = reinterpret_cast<FnTryDoingTeleport>(head->methodPointer);
        }
    }
    if (!gDoing) gDoing = AtRva<FnTryDoingTeleport>(kRvaTryDoingTeleport);

    // SetForcedFlush：void() 不唯一 → 方法哈希。
    void* vcKlass = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* mpKlass = x::runtime::il2cpp::FindClass("", kMovePathClass);
    void* miVc = vcKlass ? findByName(vcKlass, kHashVecCtrlSetForcedFlush, 0) : nullptr;
    if (!miVc && vcKlass) miVc = findByName(vcKlass, "SetForcedFlush", 0);
    void* miMp = mpKlass ? findByName(mpKlass, kHashMovePathSetForcedFlush, 0) : nullptr;
    if (!miMp && mpKlass) miMp = findByName(mpKlass, "SetForcedFlush", 0);
    if (miVc) {
        auto* h = reinterpret_cast<MethodInfoHead*>(miVc);
        if (h->methodPointer) gSetFlush = reinterpret_cast<FnSetForcedFlush>(h->methodPointer);
    }
    if (miMp) {
        auto* h = reinterpret_cast<MethodInfoHead*>(miMp);
        if (h->methodPointer) gMpSetFlush = reinterpret_cast<FnSetForcedFlush>(h->methodPointer);
    }
    if (!gSetFlush) gSetFlush = AtRva<FnSetForcedFlush>(kRvaVecCtrlSetForcedFlush);
    if (!gMpSetFlush) gMpSetFlush = AtRva<FnSetForcedFlush>(kRvaMovePathSetForcedFlush);

    const bool ok = gDoing != nullptr;
    gBound.store(ok, std::memory_order_release);
    if (ok) {
        x::runtime::LogI("Teleport", "bound path=fill+Doing Doing@0x%X Flush@0x%X MpFlush@0x%X mi=%p",
                         kRvaTryDoingTeleport, kRvaVecCtrlSetForcedFlush,
                         kRvaMovePathSetForcedFlush, gMiDoing);
    } else {
        x::runtime::LogW("Teleport", "bind fail Doing=%p", gDoing);
    }
    return ok;
}

bool CallDoingAndForcedFlushSeh(void* lu) {
    if (!gDoing || !LooksLikeHeapPtr(lu)) return false;
    __try {
        gDoing(lu, gMiDoing);
        void* vc = ReadPtr(lu, kOffVecCtrl);
        void* mp = LooksLikeHeapPtr(vc) ? ReadPtr(vc, kOffVcMovePath) : nullptr;
        if (LooksLikeHeapPtr(vc) && gSetFlush) {
            gSetFlush(vc, nullptr);
        } else if (LooksLikeHeapPtr(mp)) {
            WriteBool(mp, kOffMpForcedFlush, true);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearTeleportClientCooldown(void* lu) {
    if (!LooksLikeHeapPtr(lu)) return;
    const int oldCd = ReadI32(lu, kOffTeleportCoolTimeEnd);
    const int oldStart = ReadI32(lu, kOffTeleportStartTick);
    const bool wasValid = ReadBool(lu, kOffTeleportIsValid);
    WriteI32(lu, kOffTeleportCoolTimeEnd, 0);
    WriteI32(lu, kOffTeleportStartTick, 0);
    WriteBool(lu, kOffTeleportIsValid, false);
    x::runtime::LogI("Teleport", "clear CoolTimeEnd %d→0 startTick=%d wasValid=%d", oldCd,
                     oldStart, wasValid ? 1 : 0);
}

bool PickNativeShortLand(void* /*lu*/, void* vc, float* outX, float* outY, uint32_t* outFh,
                         int* outFaceLeft) {
    if (!outX || !outY) return false;
    const double ox = ReadF64(vc, kOffVcApX);
    const double oy = ReadF64(vc, kOffVcApY);
    int faceLeft = 0;
    __try {
        const int ma = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(vc) + kOffVcMoveAction);
        faceLeft = ma & 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faceLeft = 0;
    }
    const bool keyLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    const bool keyRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
    if (keyLeft && !keyRight) faceLeft = 1;
    else if (keyRight && !keyLeft) faceLeft = 0;

    const float prefer = faceLeft ? -kNativeShortHopPx : kNativeShortHopPx;
    const float alt = -prefer;
    float tx = 0.f, ty = 0.f;
    uint32_t fh = 0;
    if (foothold_path::SnapStandAt(static_cast<float>(ox) + prefer, static_cast<float>(oy), &tx,
                                   &ty, &fh)) {
        *outX = tx;
        *outY = ty;
        if (outFh) *outFh = fh;
        if (outFaceLeft) *outFaceLeft = faceLeft;
        return true;
    }
    if (foothold_path::SnapStandAt(static_cast<float>(ox) + alt, static_cast<float>(oy), &tx, &ty,
                                   &fh)) {
        *outX = tx;
        *outY = ty;
        if (outFh) *outFh = fh;
        if (outFaceLeft) *outFaceLeft = faceLeft ? 0 : 1;
        return true;
    }
    *outX = static_cast<float>(ox) + prefer;
    *outY = static_cast<float>(oy);
    if (outFh) *outFh = 0;
    if (outFaceLeft) *outFaceLeft = faceLeft;
    return true;
}

bool FillTeleportPending(void* lu, float tx, float ty) {
    if (!LooksLikeHeapPtr(lu)) return false;
    WriteF32(lu, kOffTeleportPosX, tx);
    WriteF32(lu, kOffTeleportPosY, ty);
    WriteBool(lu, kOffTeleportByPortal, false);
    WriteI32(lu, kOffTeleportStartTick, 0);
    WriteI32(lu, kOffTeleportCoolTimeEnd, 0);
    WriteBool(lu, kOffTeleportIsValid, true);
    return true;
}

// 手填 Teleport + Mp +（可选 CurFh 指针）+ Doing。不 SyncRel、不 Register、不 ImpactNext。
bool ApplyFillDoing(void* lu, void* vc, float tx, float ty, uint32_t fhId, const char* tag) {
    if (!FillTeleportPending(lu, tx, ty)) return false;
    void* mp = ReadPtr(vc, kOffVcMovePath);
    if (LooksLikeHeapPtr(mp)) {
        WriteI16(mp, kOffMpX, static_cast<int16_t>(static_cast<int>(std::lround(tx))));
        WriteI16(mp, kOffMpY, static_cast<int16_t>(static_cast<int>(std::lround(ty))));
    }
    if (fhId != 0) {
        void* plantFh = foothold::ResolveFhObject(fhId);
        if (LooksLikeHeapPtr(plantFh)) {
            WritePtr(vc, kOffVcCurFh, plantFh);
            WritePtr(vc, kOffVcLastFh, plantFh);
        }
    }
    if (!CallDoingAndForcedFlushSeh(lu)) return false;
    // 皮 = lerp(Ap, Apl)：Doing 后同帧对齐，避免拖影。见 P0c §8.1
    {
        const double ax = ReadF64(vc, kOffVcApX);
        const double ay = ReadF64(vc, kOffVcApY);
        if (std::isfinite(ax) && std::isfinite(ay)) {
            WriteF64(vc, kOffVcAplX, ax);
            WriteF64(vc, kOffVcAplY, ay);
        }
    }
    x::runtime::LogI("Teleport", "fill+Doing %s land=(%.0f,%.0f) fh=%u ap=(%.0f,%.0f)",
                     tag ? tag : "?", tx, ty, (unsigned)fhId, ReadF64(vc, kOffVcApX),
                     ReadF64(vc, kOffVcApY));
    return true;
}

struct NativeJob {
    bool ok = false;
    bool overrideLand = false;
    bool snapStand = true;
    float landX = 0.f;
    float landY = 0.f;
    uint32_t plantFhId = 0;
    char fail[48]{};
};

void SetNativeFail(NativeJob* job, const char* why) {
    if (!job) return;
    strncpy_s(job->fail, why ? why : "fail", _TRUNCATE);
}

void NativeTeleportJobFn(void* user) {
    auto* job = reinterpret_cast<NativeJob*>(user);
    if (!job) return;
    job->ok = false;
    job->fail[0] = 0;
    if (!BindFns() || !gDoing) {
        SetNativeFail(job, "bind");
        return;
    }

    player_combat::CombatCtx ctx{};
    void* lu = nullptr;
    // 绝对落点（fly / 贴怪）：不要求 Ap 已 PosSane——换图落地瞬间 QueryCombatCtx 会误报 no_user。
    // 短距自选落点仍走 QueryCombatCtx（需要朝向/坐标）。
    if (job->overrideLand) {
        if (!player_combat::QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) {
            SetNativeFail(job, "no_user");
            return;
        }
    } else {
        if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok || !LooksLikeHeapPtr(ctx.localUser)) {
            SetNativeFail(job, "no_user");
            return;
        }
        lu = ctx.localUser;
    }
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) {
        SetNativeFail(job, "no_vc");
        return;
    }

    ClearTeleportClientCooldown(lu);

    float tx = job->landX;
    float ty = job->landY;
    uint32_t fh = job->plantFhId;
    const char* tag = "long";

    if (!job->overrideLand) {
        tag = "short";
        int faceLeft = 0;
        if (!PickNativeShortLand(lu, vc, &tx, &ty, &fh, &faceLeft)) {
            SetNativeFail(job, "no_land");
            return;
        }
        x::runtime::LogI("Teleport", "short pick land=(%.0f,%.0f) faceL=%d fh=%u", tx, ty, faceLeft,
                         (unsigned)fh);
    } else {
        if (!std::isfinite(tx) || !std::isfinite(ty)) {
            SetNativeFail(job, "bad_land");
            return;
        }
        if (job->snapStand) {
            float sx = tx, sy = ty;
            uint32_t sfh = 0;
            if (foothold_path::SnapStandAt(tx, ty, &sx, &sy, &sfh)) {
                tx = sx;
                ty = sy;
                if (sfh != 0) fh = sfh;
            }
        }
    }

    // 硬门禁：fh==0 禁止 fill+Doing。
    // 例外：点飞 overrideLand && !snapStand（fly）允许悬空落点。
    if (fh == 0) {
        if (!(job->overrideLand && !job->snapStand)) {
            SetNativeFail(job, "fh0_forbid");
            x::runtime::LogW("Teleport", "forbid fill+Doing fh=0 land=(%.0f,%.0f) snap=%d", tx, ty,
                             job->snapStand ? 1 : 0);
            return;
        }
    }

    if (!ApplyFillDoing(lu, vc, tx, ty, fh, tag)) {
        SetNativeFail(job, "seh_doing");
        return;
    }
    job->ok = true;
    job->landX = tx;
    job->landY = ty;
    job->plantFhId = fh;
    SetNativeFail(job, job->overrideLand ? "ok_fill_long" : "ok_fill_doing");
}

}  // namespace

bool EnsureBound() { return BindFns(); }

void SetNativeCooldownMs(uint32_t ms) {
    if (ms < kNativeSelfCdMinMs) ms = kNativeSelfCdMinMs;
    if (ms > kNativeSelfCdMaxMs) ms = kNativeSelfCdMaxMs;
    gNativeCdMs.store(ms, std::memory_order_release);
}

void ForceNativeCooldownMs(uint32_t ms) {
    SetNativeCooldownMs(ms);
    gLastNativeMs = GetTickCount();
    x::runtime::LogI("Teleport", "force self-cd %ums (trip arm)",
                     gNativeCdMs.load(std::memory_order_acquire));
}

void ClearNativeSelfCd() { gLastNativeMs = 0; }

uint32_t NativeCooldownRemainingMs() {
    const DWORD now = GetTickCount();
    const DWORD cd = gNativeCdMs.load(std::memory_order_acquire);
    if (!gLastNativeMs || !cd) return 0;
    const DWORD elapsed = now - gLastNativeMs;
    if (elapsed >= cd) return 0;
    return static_cast<uint32_t>(cd - elapsed);
}

bool TeleportNativeSkillCall() {
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        x::runtime::LogW("Teleport", "native short reject not_play_ready scene=%d",
                         static_cast<int>(world::GetSceneState()));
        return false;
    }
    if (!BindFns() || !gDoing) {
        x::runtime::LogW("Teleport", "native bind fail Doing=%p", gDoing);
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD cd = gNativeCdMs.load(std::memory_order_acquire);
    if (gLastNativeMs && now - gLastNativeMs < cd) {
        x::runtime::LogW("Teleport", "native self-cd remain=%ums", cd - (now - gLastNativeMs));
        return false;
    }

    if (!runtime::main_thread::Ensure()) {
        x::runtime::LogW("Teleport", "native main pump missing");
        return false;
    }

    NativeJob job{};
    job.overrideLand = false;
    if (!runtime::main_thread::InvokeAndWait(&NativeTeleportJobFn, &job, kJobWaitMs)) {
        x::runtime::LogW("Teleport", "native main-thread timeout");
        return false;
    }
    if (!job.ok) {
        x::runtime::LogW("Teleport", "native fail=%s", job.fail[0] ? job.fail : "?");
        return false;
    }
    gLastNativeMs = now;
    x::runtime::LogI("Teleport", "native ok tag=%s land=(%.0f,%.0f) fh=%u", job.fail, job.landX,
                     job.landY, (unsigned)job.plantFhId);
    return true;
}

bool TeleportNativeSkillCall(float landX, float landY, uint32_t plantFhId, bool snapStand) {
    if (!std::isfinite(landX) || !std::isfinite(landY)) {
        x::runtime::LogW("Teleport", "native land reject non-finite");
        return false;
    }
    // 硬门禁：换图 scene!=play 禁止瞬移
    if (!world::IsInMapScene() || !world::IsPlayReady()) {
        x::runtime::LogW("Teleport", "native reject not_play_ready scene=%d",
                         static_cast<int>(world::GetSceneState()));
        return false;
    }
    if (!BindFns() || !gDoing) {
        x::runtime::LogW("Teleport", "native bind fail Doing=%p", gDoing);
        return false;
    }

    const DWORD now = GetTickCount();
    const DWORD cd = gNativeCdMs.load(std::memory_order_acquire);
    if (gLastNativeMs && now - gLastNativeMs < cd) {
        x::runtime::LogW("Teleport", "native self-cd remain=%ums", cd - (now - gLastNativeMs));
        return false;
    }

    if (!runtime::main_thread::Ensure()) {
        x::runtime::LogW("Teleport", "native main pump missing");
        return false;
    }

    NativeJob job{};
    job.overrideLand = true;
    job.snapStand = snapStand;
    job.landX = landX;
    job.landY = landY;
    job.plantFhId = plantFhId;
    if (!runtime::main_thread::InvokeAndWait(&NativeTeleportJobFn, &job, kJobWaitMs)) {
        x::runtime::LogW("Teleport", "native main-thread timeout");
        return false;
    }
    if (!job.ok) {
        x::runtime::LogW("Teleport", "native fail=%s land=(%.0f,%.0f) fh=%u snap=%d",
                         job.fail[0] ? job.fail : "?", landX, landY, (unsigned)plantFhId,
                         snapStand ? 1 : 0);
        return false;
    }
    gLastNativeMs = now;
    x::runtime::LogI("Teleport", "native ok tag=%s land=(%.0f,%.0f) fh=%u snap=%d", job.fail,
                     job.landX, job.landY, (unsigned)job.plantFhId, snapStand ? 1 : 0);
    return true;
}

}  // namespace x::features::ports::teleport
