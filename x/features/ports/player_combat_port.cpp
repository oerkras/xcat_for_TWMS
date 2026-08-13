// Classic TWMS — LocalUser combat context (read-only position).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "player_combat_port.h"

#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <cmath>
#include <cstring>

namespace x::features::ports::player_combat {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Unity FindAll / get_gameObject / get_name → x::runtime::il2cpp::kRva*（il2cpp_bind.h SSOT）

// UserLocal → il2cpp_shape::ResolveUserLocalKlass
// Remount 2026-08-06：ACS hash 全换。08-13：CurPos 哈希从数组槽撕开（见 kHashLogicalPos）。
constexpr char kActorBaseClass[] =
    "bef0eed02528709201717d93717a1904bfa2e850dfe1f5fadf473c0e9c78d9b";
constexpr char kVecCtrlClass[] =
    "b4117afc7f6f9c58587c528c3dec862d440e5d266ad70b764c0058566918784";
constexpr char kFhClass[] =
    "aa3d8db6b3f8e88e2163fa99baddd275f7dd95560a044f4fbf0d9f8f17ff067";

// hash → field_get_offset（与 foothold / mob / invuln / skill 同源）
constexpr char kHashUserVecCtrl[] =
    "<bfd62ef3b3e356b3d554a10a21a0f46b1272d519b934db1a7c4df88a0adcd52>k__BackingField";
constexpr char kHashPos[] =
    "c1792cf58ceda9b4f12cacf2746c06b70c90d516c43cbdf1fcea7c7b1bde37b";
constexpr char kHashVcCurFh[] =
    "<f191525b3e75203f83e8420264723f78754068945de8fb7c4c15e3a96c42d3b>k__BackingField";
constexpr char kHashVcAp[] =
    "eeb6a8c060a20622eef369b184c450bad5515167fa2cb42392277c7b0cbfe7d";  // AbsPos; Y=+8
constexpr char kHashVcApl[] =
    "e5a40d07ca56525d5a34df38aaeadf1dc421da7a583f82a6b8a0e7736ebd8aa";  // Apl; Y=+8
constexpr char kHashVcRelPos[] =
    "bf593c8b8886d021b370e0ab3d9c8eb97bf29672db1114a1309edd63039ade7";  // RelPos; V=+8
constexpr char kHashFhId[] =
    "<ccb1e319bb15474d5f8226a83809805ed660d7d082d8423a4bf403493cce67f>k__BackingField";
// LocalUser 镜头 CurPos（只读诊断）。08-13：真 Vector2 是 ccce125f@0x2B0；
// d6f3e65b@0x240 已是数组，equal-offset 会假命中。窗口 0x200–0x300 仍包住 0x2B0。
// 2026-08-04 撤销「镜头自愈」：0.1.36 实测只读探针 dApCur 静息 27~53px、dApPos=0、
// dAA=0.0，引擎跟随本就是活的；dApCur 变大只是因为贴怪每秒瞬移 ~6.7 次、每次几百像素，
// 平滑跟随物理上无法收敛。自愈据此每 0.29s 硬拧一次镜头（中位 343px、峰值 1419px），
// 反而成了撕裂源。原「镜头粘死」证据取自 nSlow_=140 污染动作层的旧局，根因已换，立论作废。
constexpr char kHashLogicalPos[] =
    "ccce125f69593bc9100a9b227cda0be73abc63fefc4b848f92efe9a72930859";
// e2a28(Key) 键位移偏移对（IDA：mov [rsi+4B4h], rdx · 两 int32）
constexpr char kHashKeyMoveDelta[] =
    "<ecb94eef01196b5d240f83e34f0a089e0add6fccc05c16832e9529227473449>k__BackingField";

constexpr size_t kOffCachedPtr = 0x10;
#define kOffWmMyUser (x::ui::player::OffWmMyUser())
// Mob 专属 PvcActive；LU 上无同名槽，保留软探针（LooksLikeHeapPtr 失败 → VecCtrl）
constexpr char kHashPvcActive[] =
    "dfdfb42a91b7e684e1007b0b0d20e9e301b977c437ce72241a81b78c18f960e";
constexpr char kMobClass[] =
    "d8d3c81101a6fa89d05fe89ac3cfe1b8801c9afd67abd392d5637681410b92f";
constexpr size_t kFbPvcActive = 0xF0;
size_t gOffPvcActive = kFbPvcActive;
#define kOffPvcActive (gOffPvcActive)
constexpr size_t kFbVecCtrl = 0x50, kFbPos = 0x64, kFbVcCurFh = 0x28, kFbVcAp = 0x98;
constexpr size_t kFbVcApl = 0xB8, kFbVcRelPos = 0x88, kFbFhId = 0x10, kFbLogicalPos = 0x2B0;
constexpr size_t kFbKeyMoveDelta = 0x47C;
size_t gOffVecCtrl = kFbVecCtrl, gOffPos = kFbPos, gOffVcCurFh = kFbVcCurFh, gOffVcAp = kFbVcAp;
size_t gOffVcApl = kFbVcApl, gOffVcRelPos = kFbVcRelPos, gOffFhId = kFbFhId;
size_t gOffLogicalPos = kFbLogicalPos;
size_t gOffKeyMoveDelta = kFbKeyMoveDelta;
#define kOffVecCtrl (gOffVecCtrl)
#define kOffPos (gOffPos)
#define kOffVcCurFh (gOffVcCurFh)
#define kOffVcApX (gOffVcAp)
#define kOffVcApY (gOffVcAp + 8)
#define kOffVcAplX (gOffVcApl)
#define kOffVcAplY (gOffVcApl + 8)
#define kOffVcRpPos (gOffVcRelPos)
#define kOffVcRpV (gOffVcRelPos + 8)
#define kOffFhId (gOffFhId)
#define kOffLuCurPos (gOffLogicalPos)
#define kOffKeyMoveDelta (gOffKeyMoveDelta)
bool gCombatFieldTried = false;

bool CombatFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out, size_t lo,
                       size_t hi) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    for (void* k = klass; k;) {
        void* field = nullptr;
        __try {
            field = e.classGetFieldFromName(k, hash);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            field = nullptr;
        }
        if (field) {
            size_t off = 0;
            __try {
                off = e.fieldGetOffset(field);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                off = 0;
            }
            if (off >= lo && off < hi) {
                *out = off;
                return true;
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return false;
}

void EnsureCombatFieldOff() {
    constexpr int kExpect = 10;
    static int sLastHits = -1;
    if (gCombatFieldTried && sLastHits >= kExpect) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* actor = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    if (!actor) actor = ul;
    void* vc = x::runtime::il2cpp::FindClass("", kVecCtrlClass);
    void* fh = x::runtime::il2cpp::FindClass("", kFhClass);
    int hits = 0;
    auto hit = [&](bool ok) {
        if (ok) ++hits;
    };
    hit(CombatFieldOffHit(actor, kHashUserVecCtrl, kFbVecCtrl, &gOffVecCtrl, 0x40, 0x100));
    hit(CombatFieldOffHit(actor, kHashPos, kFbPos, &gOffPos, 0x40, 0x100));
    hit(CombatFieldOffHit(vc, kHashVcCurFh, kFbVcCurFh, &gOffVcCurFh, 0x10, 0x80));
    hit(CombatFieldOffHit(vc, kHashVcAp, kFbVcAp, &gOffVcAp, 0x80, 0x100));
    hit(CombatFieldOffHit(vc, kHashVcApl, kFbVcApl, &gOffVcApl, 0x80, 0x100));
    hit(CombatFieldOffHit(vc, kHashVcRelPos, kFbVcRelPos, &gOffVcRelPos, 0x60, 0x100));
    hit(CombatFieldOffHit(fh, kHashFhId, kFbFhId, &gOffFhId, 0x10, 0x80));  // 与 foothold 同窗
    hit(CombatFieldOffHit(ul, kHashLogicalPos, kFbLogicalPos, &gOffLogicalPos, 0x200, 0x300));
    hit(CombatFieldOffHit(ul, kHashKeyMoveDelta, kFbKeyMoveDelta, &gOffKeyMoveDelta, 0x400, 0x600));
    void* mob = x::runtime::il2cpp::FindClass("", kMobClass);
    hit(CombatFieldOffHit(mob, kHashPvcActive, kFbPvcActive, &gOffPvcActive, 0x80, 0x180));
    gCombatFieldTried = true;
    if (hits != sLastHits) {
        sLastHits = hits;
        x::runtime::LogI("PlayerCombat",
                         "combat slots path=%s hits=%d/%d vc=0x%zX pos=0x%zX ap=0x%zX key=0x%zX "
                         "pvc=0x%zX myUser=0x%zX",
                         hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                         kExpect, gOffVecCtrl, gOffPos, gOffVcAp, gOffKeyMoveDelta, gOffPvcActive,
                         x::ui::player::OffWmMyUser());
    }
}

// 仅用于 dCurBody 日志，把镜头静息高度折算回身体位；不参与任何写入。
constexpr float kRestCamOffYNominal = 23.f;
// 只读探针的「脱节」报警线（贴怪瞬移期镜头本就会拉开，仅记录不干预）。
constexpr float kCamProbeStuckPx = 200.f;
constexpr float kHealPosDriftPx = 48.f;
constexpr float kHealAplDriftPx = 8.f;

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

uint32_t ReadU32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
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

void WriteF32(void* obj, size_t off, float v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteF64(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

float Dist2(float ax, float ay, float bx, float by) {
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
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

    // 裸 gFindAll 绕过 managed_main 包装 —— 与 invuln 同守仓级闸。
    if (x::runtime::managed_main::IsLoginFrozen() ||
        x::runtime::managed_main::IsMapTransitBlocked() || !world::IsPlayReady()) {
        return;
    }

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

    // InterStage / 卸图：禁主线程 FindAll（拖黑屏）；MyUser 空则等 PlayReady。
    if (!world::IsPlayReady()) return false;

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
    EnsureCombatFieldOff();
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

void PeekLocalPosDiag(PosDiag& out) {
    out = PosDiag{};
    if (!EnsureBound()) {
        out.why = "not_bound";
        return;
    }
    const DWORD now = GetTickCount();
    if (!TryResolveLocalUser(now)) {
        out.why = "no_local_user";
        return;
    }
    out.hasLocalUser = true;
    out.luAlive = UnityObjectAlive(gLocalUser);
    if (!out.luAlive) {
        out.why = "lu_dead";
        return;
    }
    float x = 0.f, y = 0.f;
    ReadActorPos(gLocalUser, x, y);
    out.x = x;
    out.y = y;
    if (!std::isfinite(x) || !std::isfinite(y)) {
        out.why = "nan_inf";
        return;
    }
    if (std::fabs(x) > kMaxPosAbs || std::fabs(y) > kMaxPosAbs) {
        out.why = "abs_overflow";
        return;
    }
    if (std::fabs(x) < kMinPosAbs && std::fabs(y) < kMinPosAbs) {
        out.why = "near_zero";
        return;
    }
    out.sane = true;
    out.why = "ok";
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

bool QueryVisualSnap(VisualSnap& out) {
    out = VisualSnap{};
    void* lu = nullptr;
    if (!QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) vc = ReadPtr(lu, kOffPvcActive);
    if (!LooksLikeHeapPtr(vc)) return false;

    const double apx = ReadF64(vc, kOffVcApX);
    const double apy = ReadF64(vc, kOffVcApY);
    const double aplx = ReadF64(vc, kOffVcAplX);
    const double aply = ReadF64(vc, kOffVcAplY);
    out.apX = static_cast<float>(apx);
    out.apY = static_cast<float>(apy);
    out.aplX = static_cast<float>(aplx);
    out.aplY = static_cast<float>(aply);
    out.posX = ReadF32(lu, kOffPos);
    out.posY = ReadF32(lu, kOffPos + 4);
    out.curPosX = ReadF32(lu, kOffLuCurPos);
    out.curPosY = ReadF32(lu, kOffLuCurPos + 4);
    out.keyDeltaX = ReadI32(lu, kOffKeyMoveDelta);
    out.keyDeltaY = ReadI32(lu, kOffKeyMoveDelta + 4);
    out.keyDeltaOk = true;
    void* fh = ReadPtr(vc, kOffVcCurFh);
    if (LooksLikeHeapPtr(fh)) out.curFh = ReadU32(fh, kOffFhId);
    out.rpPos = ReadF64(vc, kOffVcRpPos);
    out.rpV = ReadF64(vc, kOffVcRpV);
    if (std::isfinite(out.apX) && std::isfinite(out.apY)) {
        if (std::isfinite(out.aplX) && std::isfinite(out.aplY))
            out.dApApl = Dist2(out.apX, out.apY, out.aplX, out.aplY);
        if (std::isfinite(out.posX) && std::isfinite(out.posY))
            out.dApPos = Dist2(out.apX, out.apY, out.posX, out.posY);
        if (std::isfinite(out.curPosX) && std::isfinite(out.curPosY))
            out.dApCur = Dist2(out.apX, out.apY, out.curPosX, out.curPosY);
    }
    out.ok = true;
    return true;
}

void LogVisualLayer(const char* comp, const char* phase, const char* note,
                    const VisualSnap* baseline) {
    VisualSnap s{};
    if (!QueryVisualSnap(s) || !s.ok) {
        x::runtime::LogWThrottled(92, 2000, comp && comp[0] ? comp : "Vis",
                                  "vis phase=%s note=%s snap=fail", phase ? phase : "?",
                                  note ? note : "-");
        return;
    }
    // 砍日志：pre 无对照不刷；post/t+120 只在层间真漂时打。
    if (!(baseline && baseline->ok)) return;
    const float dAp = Dist2(s.apX, s.apY, baseline->apX, baseline->apY);
    const float dApl = Dist2(s.aplX, s.aplY, baseline->aplX, baseline->aplY);
    const float dPos = Dist2(s.posX, s.posY, baseline->posX, baseline->posY);
    const float dCur = Dist2(s.curPosX, s.curPosY, baseline->curPosX, baseline->curPosY);
    const float ddAA = s.dApApl - baseline->dApApl;
    const bool drifted = dAp >= 8.f || dApl >= 8.f || dPos >= 48.f || dCur >= 200.f ||
                         s.dApPos >= 48.f || s.dApCur >= 200.f || std::fabs(ddAA) >= 4.f;
    if (!drifted) return;
    x::runtime::LogI(
        comp && comp[0] ? comp : "Vis",
        "vis phase=%s note=%s ap=(%.0f,%.0f) apl=(%.0f,%.0f) dAA=%.1f pos=(%.0f,%.0f) "
        "cur=(%.0f,%.0f) dPos=%.0f dCur=%.0f fh=%u dAp=%.1f dApl=%.1f dPosB=%.1f dCurB=%.1f "
        "dAA=%+.1f dfh=%d",
        phase ? phase : "?", note ? note : "-", s.apX, s.apY, s.aplX, s.aplY, s.dApApl, s.posX,
        s.posY, s.curPosX, s.curPosY, s.dApPos, s.dApCur, (unsigned)s.curFh, dAp, dApl, dPos, dCur,
        ddAA, static_cast<int>(s.curFh) - static_cast<int>(baseline->curFh));
}

namespace {
bool ProbeEnvFlag(const char* name) {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y');
}
bool ProbeOnKeyEnvOn() { return ProbeEnvFlag("XCAT_PROBE_ONKEY"); }
bool ProbeDoActiveEnvOn() { return ProbeEnvFlag("XCAT_PROBE_DOACTIVE"); }
bool ProbeFillCamEnvOn() { return ProbeEnvFlag("XCAT_PROBE_FILLCAM"); }
}  // namespace

void LogE2a28KeyProbe(const char* phase, const char* note, const VisualSnap* baseline) {
    VisualSnap s{};
    if (!QueryVisualSnap(s) || !s.ok) {
        x::runtime::LogWThrottled(93, 2000, "E2a28", "probe phase=%s note=%s snap=fail",
                                  phase ? phase : "?", note ? note : "-");
        return;
    }
    const bool envOn = ProbeOnKeyEnvOn();
    if (baseline && baseline->ok) {
        const float dAp = Dist2(s.apX, s.apY, baseline->apX, baseline->apY);
        const float dApl = Dist2(s.aplX, s.aplY, baseline->aplX, baseline->aplY);
        const float dPos = Dist2(s.posX, s.posY, baseline->posX, baseline->posY);
        const float dCur = Dist2(s.curPosX, s.curPosY, baseline->curPosX, baseline->curPosY);
        const bool drifted = dAp >= 8.f || dApl >= 8.f || dPos >= 48.f || dCur >= 200.f ||
                             s.dApPos >= 48.f ||
                             s.keyDeltaX != baseline->keyDeltaX || s.keyDeltaY != baseline->keyDeltaY;
        if (!drifted && !envOn) return;
        // e2a28 钉完后 Ap/Pos 常等于 int 坐标；用截断 Ap 近似当时的 (edi,r14d)
        const int approxX = static_cast<int>(s.apX);
        const int approxY = static_cast<int>(s.apY);
        x::runtime::LogI(
            "E2a28",
            "probe phase=%s note=%s d4b4=(%d,%d) was=(%d,%d) ap=(%.0f,%.0f) apl=(%.0f,%.0f) "
            "pos=(%.0f,%.0f) cur=(%.0f,%.0f) dAp=%.0f dApl=%.0f dPos=%.0f dCur=%.0f "
            "approxXY=(%d,%d) dApPos=%.0f",
            phase ? phase : "?", note ? note : "-", s.keyDeltaX, s.keyDeltaY, baseline->keyDeltaX,
            baseline->keyDeltaY, s.apX, s.apY, s.aplX, s.aplY, s.posX, s.posY, s.curPosX, s.curPosY,
            dAp, dApl, dPos, dCur, approxX, approxY, s.dApPos);
        return;
    }
    // pre：仅 env 开启时打，避免常态刷屏
    if (!envOn) return;
    x::runtime::LogI("E2a28",
                     "probe phase=%s note=%s d4b4=(%d,%d) ap=(%.0f,%.0f) apl=(%.0f,%.0f) "
                     "pos=(%.0f,%.0f) cur=(%.0f,%.0f) dApPos=%.0f",
                     phase ? phase : "?", note ? note : "-", s.keyDeltaX, s.keyDeltaY, s.apX, s.apY,
                     s.aplX, s.aplY, s.posX, s.posY, s.curPosX, s.curPosY, s.dApPos);
}

void LogDoActiveVisProbe(const char* phase, const char* note, const VisualSnap* baseline) {
    VisualSnap s{};
    if (!QueryVisualSnap(s) || !s.ok) {
        x::runtime::LogWThrottled(94, 2000, "DoActive", "vis phase=%s note=%s snap=fail",
                                  phase ? phase : "?", note ? note : "-");
        return;
    }
    const bool envOn = ProbeDoActiveEnvOn();
    if (baseline && baseline->ok) {
        const float dAp = Dist2(s.apX, s.apY, baseline->apX, baseline->apY);
        const float dApl = Dist2(s.aplX, s.aplY, baseline->aplX, baseline->aplY);
        const float dPos = Dist2(s.posX, s.posY, baseline->posX, baseline->posY);
        const float dCur = Dist2(s.curPosX, s.curPosY, baseline->curPosX, baseline->curPosY);
        const bool drifted = dAp >= 8.f || dApl >= 8.f || dPos >= 48.f || dCur >= 200.f ||
                             s.dApPos >= 48.f || s.dApCur >= 200.f;
        if (!drifted && !envOn) return;
        x::runtime::LogI(
            "DoActive",
            "vis phase=%s note=%s ap=(%.0f,%.0f) apl=(%.0f,%.0f) pos=(%.0f,%.0f) cur=(%.0f,%.0f) "
            "dAp=%.0f dApl=%.0f dPos=%.0f dCur=%.0f dApPos=%.0f dApCur=%.0f d4b4=(%d,%d)",
            phase ? phase : "?", note ? note : "-", s.apX, s.apY, s.aplX, s.aplY, s.posX, s.posY,
            s.curPosX, s.curPosY, dAp, dApl, dPos, dCur, s.dApPos, s.dApCur, s.keyDeltaX,
            s.keyDeltaY);
        return;
    }
    if (!envOn) return;
    x::runtime::LogI("DoActive",
                     "vis phase=%s note=%s ap=(%.0f,%.0f) apl=(%.0f,%.0f) pos=(%.0f,%.0f) "
                     "cur=(%.0f,%.0f) dApPos=%.0f dApCur=%.0f d4b4=(%d,%d)",
                     phase ? phase : "?", note ? note : "-", s.apX, s.apY, s.aplX, s.aplY, s.posX,
                     s.posY, s.curPosX, s.curPosY, s.dApPos, s.dApCur, s.keyDeltaX, s.keyDeltaY);
}

void LogFillDoingCamProbe(const char* note) {
    // 只读探针：镜头由引擎独占，本模块不再写 CurPos/PrevPos。
    VisualSnap s{};
    if (!QueryVisualSnap(s) || !s.ok) return;
    const bool envOn = ProbeFillCamEnvOn();
    const bool stuck = s.dApCur >= kCamProbeStuckPx;
    static bool wasStuck = false;
    static DWORD lastLogAt = 0;
    const DWORD now = GetTickCount();
    const bool risingEdge = stuck && !wasStuck;
    wasStuck = stuck;
    if (!stuck && !envOn) return;
    if (!envOn && !risingEdge && (now - lastLogAt) < 2000) return;
    lastLogAt = now;
    x::runtime::LogI("FillCam",
                     "cam note=%s%s ap=(%.0f,%.0f) apl=(%.0f,%.0f) pos=(%.0f,%.0f) cur=(%.0f,%.0f) "
                     "dApPos=%.0f dApCur=%.0f dAA=%.1f edge=%d stuck=%d",
                     note ? note : "-", risingEdge ? " FIRST_STUCK" : "", s.apX, s.apY, s.aplX,
                     s.aplY, s.posX, s.posY, s.curPosX, s.curPosY, s.dApPos, s.dApCur, s.dApApl,
                     risingEdge ? 1 : 0, stuck ? 1 : 0);
}

void LogBuffCamProbe(const char* note) {
    // 只读：buffs Tick 在 worker；禁止在此写 CurPos（跨线程拧镜头）。
    VisualSnap s{};
    if (!QueryVisualSnap(s) || !s.ok) return;
    static DWORD lastLogAt = 0;
    const DWORD now = GetTickCount();
    if ((now - lastLogAt) < 1000) return;
    lastLogAt = now;
    x::runtime::LogI("BuffCam",
                     "cam note=%s ap=(%.0f,%.0f) apl=(%.0f,%.0f) pos=(%.0f,%.0f) cur=(%.0f,%.0f) "
                     "dApPos=%.0f dApCur=%.0f dAA=%.1f fh=%u",
                     note ? note : "-", s.apX, s.apY, s.aplX, s.aplY, s.posX, s.posY, s.curPosX,
                     s.curPosY, s.dApPos, s.dApCur, s.dApApl, (unsigned)s.curFh);
}

bool HealVisualToAp(const char* why, bool force) {
    // 主线程专用：只收 Apl/Pos，不碰镜头（CurPos/PrevPos 归引擎）。
    // 禁止从 worker 调；已在 pump job 内时勿再套 InvokeAndWait。
    if (!x::runtime::main_thread::IsOnPumpThread()) {
        x::runtime::LogWThrottled(94, 3000, "PlayerCombat",
                                  "vis heal refuse why=%s reason=not_pump_thread",
                                  why ? why : "?");
        return false;
    }
    void* lu = nullptr;
    if (!QueryLocalUser(&lu) || !LooksLikeHeapPtr(lu)) return false;
    void* vc = ReadPtr(lu, kOffVecCtrl);
    if (!LooksLikeHeapPtr(vc)) vc = ReadPtr(lu, kOffPvcActive);
    if (!LooksLikeHeapPtr(vc)) return false;

    const float apx = static_cast<float>(ReadF64(vc, kOffVcApX));
    const float apy = static_cast<float>(ReadF64(vc, kOffVcApY));
    if (!std::isfinite(apx) || !std::isfinite(apy)) return false;
    if (!PosSane(apx, apy)) return false;

    const float aplx = static_cast<float>(ReadF64(vc, kOffVcAplX));
    const float aply = static_cast<float>(ReadF64(vc, kOffVcAplY));
    const float posx = ReadF32(lu, kOffPos);
    const float posy = ReadF32(lu, kOffPos + 4);
    const float curx = ReadF32(lu, kOffLuCurPos);
    const float cury = ReadF32(lu, kOffLuCurPos + 4);

    const float dApl = Dist2(apx, apy, aplx, aply);
    const float dPos = Dist2(apx, apy, posx, posy);
    const float dCurBody = Dist2(apx, apy, curx, cury - kRestCamOffYNominal);

    const bool posBad = !std::isfinite(posx) || !std::isfinite(posy) || dPos >= kHealPosDriftPx;
    const bool aplBad = !std::isfinite(aplx) || !std::isfinite(aply) || dApl >= kHealAplDriftPx;

    bool wrote = false;
    if (force || posBad || aplBad) {
        WriteF64(vc, kOffVcAplX, apx);
        WriteF64(vc, kOffVcAplY, apy);
        WriteF32(lu, kOffPos, apx);
        WriteF32(lu, kOffPos + 4, apy);
        wrote = true;
        if (posBad || aplBad) {
            x::runtime::LogI("PlayerCombat",
                             "vis heal why=%s force=%d ap=(%.0f,%.0f) wasPos=(%.0f,%.0f) dPos=%.0f "
                             "dApl=%.1f dCurBody=%.0f",
                             why ? why : "?", force ? 1 : 0, apx, apy, posx, posy, dPos, dApl,
                             dCurBody);
        }
    }
    return wrote;
}

}  // namespace x::features::ports::player_combat
