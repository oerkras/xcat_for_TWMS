// Classic TWMS — LocalUser combat context (read-only position).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "player_combat_port.h"

#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <cmath>
#include <cstring>

namespace x::features::ports::player_combat {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaCompGetGo = 0x4E47E00;  // remapped 2026-08-03
constexpr uint32_t kRvaObjGetName = 0x4E54D60;  // remapped 2026-08-03

// UserLocal → il2cpp_shape::ResolveUserLocalKlass

constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kOffWmMyUser = 0x28;
constexpr size_t kOffPos = 0x64;
constexpr size_t kOffVecCtrl = 0x50;
constexpr size_t kOffPvcActive = 0xF0;
constexpr size_t kOffVcApX = 0x98;
constexpr size_t kOffVcApY = 0xA0;

constexpr float kMinPosAbs = 0.5f;
// BIN land_miss atY=-2147483648：读到崩位后仍继续瞬移/出刀。
constexpr float kMaxPosAbs = 100000.f;
// 曾 3000ms：换图首绑失败后整窗 no_user，F6 起飞空等 ~1–3s（BIN 06:29）。
constexpr DWORD kRebindMs = 250;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;

void* gLuType = nullptr;
void* gLocalUser = nullptr;
DWORD gLastLuRebind = 0;
bool gBound = false;

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

float ReadF32(void* obj, size_t off) {
    if (!obj) return 0.f;
    __try {
        return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
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

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

bool GetGoName(void* comp, char* out, int outSz) {
    if (!comp || !out || outSz <= 0 || !gCompGo || !gObjName) return false;
    out[0] = 0;
    __try {
        void* go = gCompGo(comp, nullptr);
        if (!LooksLikeHeapPtr(go)) return false;
        void* str = gObjName(go, nullptr);
        if (!LooksLikeHeapPtr(str)) return false;
        // Il2CppString: length@0x10, chars@0x14
        const int len = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 128) return false;
        const auto* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        int n = WideCharToMultiByte(CP_UTF8, 0, chars, len, out, outSz - 1, nullptr, nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool UnityObjectAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    return LooksLikeHeapPtr(ReadPtr(obj, kOffCachedPtr));
}

bool PosSane(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (std::fabs(x) > kMaxPosAbs || std::fabs(y) > kMaxPosAbs) return false;
    return true;
}

void ReadActorPos(void* actor, float& x, float& y) {
    // AbsPos 优先：vis@0x64 分家时仍停在旧精灵位（BIN 左精灵 / 右伤害）。
    void* vc = LooksLikeHeapPtr(actor) ? ReadPtr(actor, kOffPvcActive) : nullptr;
    if (!LooksLikeHeapPtr(vc)) vc = LooksLikeHeapPtr(actor) ? ReadPtr(actor, kOffVecCtrl) : nullptr;
    if (LooksLikeHeapPtr(vc)) {
        const double ax = ReadF64(vc, kOffVcApX);
        const double ay = ReadF64(vc, kOffVcApY);
        if (PosSane(static_cast<float>(ax), static_cast<float>(ay)) &&
            (std::fabs(ax) >= kMinPosAbs || std::fabs(ay) >= kMinPosAbs)) {
            x = static_cast<float>(ax);
            y = static_cast<float>(ay);
            return;
        }
    }
    x = ReadF32(actor, kOffPos);
    y = ReadF32(actor, kOffPos + 4);
    if (!PosSane(x, y)) {
        x = 0.f;
        y = 0.f;
    }
}

bool LocalUserStillAlive() {
    // Worker-safe: raw reads only. GetGoName allocates / touches GC →
    // "Fatal error in GC: Collecting from unknown thread".
    if (!gLocalUser || !UnityObjectAlive(gLocalUser)) return false;
    // 换图后旧对象可能仍 PosSane；WM.MyUser 变了必须强制重绑。
    void* wm = world::PeekWorldManager();
    if (wm) {
        void* mu = ReadPtr(wm, kOffWmMyUser);
        if (UnityObjectAlive(mu) && mu != gLocalUser) return false;
        // WM.MyUser 已对齐：落地瞬间 Ap 可能暂坏，仍视为活（点飞自带落点，不靠旧 Ap）。
        if (UnityObjectAlive(mu) && mu == gLocalUser) return true;
    }
    float x = 0.f, y = 0.f;
    ReadActorPos(gLocalUser, x, y);
    return PosSane(x, y) && (std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs);
}

struct ResolveLuCtx {
    bool ok = false;
};

void ResolveLuJobOnMain(void* user) {
    auto* c = reinterpret_cast<ResolveLuCtx*>(user);
    c->ok = false;
    gLocalUser = nullptr;

    void* wm = world::GetWorldManager();
    if (wm) {
        void* mu = ReadPtr(wm, kOffWmMyUser);
        if (UnityObjectAlive(mu)) {
            char name[64]{};
            if (GetGoName(mu, name, sizeof(name)) && _stricmp(name, "MyUser") == 0) {
                gLocalUser = mu;
                c->ok = true;
                return;
            }
            // WM.MyUser 权威指针：名读失败时仍可按位存活接受（避免卡死）
            float x = 0.f, y = 0.f;
            ReadActorPos(mu, x, y);
            if (PosSane(x, y) && (std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs)) {
                gLocalUser = mu;
                c->ok = true;
                x::runtime::LogI("PlayerCombat", "LocalUser ACCEPT wm.MyUser lu=%p (no name)",
                                 gLocalUser);
                return;
            }
        }
    }

    if (!gFindAll || !gLuType) return;

    void* arr = nullptr;
    __try {
        arr = gFindAll(gLuType, nullptr);  // already on main — no nested pump
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!arr) return;

    const uintptr_t n = ArrayLen(arr);
    for (uintptr_t i = 0; i < n && i < 64; ++i) {
        void* cand = ArrayAt(arr, i);
        if (!UnityObjectAlive(cand)) continue;
        char name[64]{};
        if (!GetGoName(cand, name, sizeof(name))) continue;
        if (_stricmp(name, "MyUser") == 0) {
            gLocalUser = cand;
            x::runtime::LogI("PlayerCombat", "LocalUser ACCEPT lu=%p", gLocalUser);
            c->ok = true;
            return;
        }
    }
}

bool TryResolveLocalUser(DWORD now) {
    if (LocalUserStillAlive()) return true;

    // MyUser 指针变了：先清缓存并强制解节流，禁止「throttle && gLocalUser」卡死旧指针。
    bool forceRebind = false;
    void* wm = world::PeekWorldManager();
    void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
    if (gLocalUser) {
        if (UnityObjectAlive(mu) && mu != gLocalUser) forceRebind = true;
    } else if (UnityObjectAlive(mu) && !gLastLuRebind) {
        // 进程内首次：缓存空但 WM 已有人 → 立刻首绑。失败后仍走 kRebindMs，
        // 禁止「mu 活着却一直 resolve 失败」时每 tick 打主线程 FindAll。
        forceRebind = true;
    }
    gLocalUser = nullptr;
    if (!forceRebind && gLastLuRebind && now - gLastLuRebind < kRebindMs) return false;
    gLastLuRebind = now;

    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }

    ResolveLuCtx ctx{};
    if (!x::runtime::managed_main::Call(&ResolveLuJobOnMain, &ctx, 2000)) {
        x::runtime::LogWThrottled(70, 5000, "PlayerCombat", "LocalUser resolve main pump fail");
        return false;
    }
    return ctx.ok;
}

bool BindApis() {
    if (gGA && gFindAll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gCompGo = e.compGo;
    gObjName = e.objName;
    return gFindAll != nullptr;
}

}  // namespace

bool EnsureBound() {
    if (gBound && gGA) return true;
    if (!BindApis()) return false;
    gBound = true;
    x::runtime::LogI("PlayerCombat", "bound FindAll=%p", gFindAll);
    return true;
}

bool QueryCombatCtx(CombatCtx& out) {
    out = CombatCtx{};
    if (!EnsureBound()) return false;
    const DWORD now = GetTickCount();
    if (!TryResolveLocalUser(now)) return false;
    float x = 0.f, y = 0.f;
    ReadActorPos(gLocalUser, x, y);
    if (!PosSane(x, y)) return false;
    if (std::fabs(x) < kMinPosAbs && std::fabs(y) < kMinPosAbs) return false;
    out.ok = true;
    out.localUser = gLocalUser;
    out.x = x;
    out.y = y;
    return true;
}

bool QueryLocalUser(void** outLu) {
    if (!outLu) return false;
    *outLu = nullptr;
    if (!EnsureBound()) return false;
    if (!TryResolveLocalUser(GetTickCount())) return false;
    if (!LooksLikeHeapPtr(gLocalUser) || !UnityObjectAlive(gLocalUser)) return false;
    *outLu = gLocalUser;
    return true;
}

}  // namespace x::features::ports::player_combat
