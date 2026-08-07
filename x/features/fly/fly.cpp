// Classic TWMS — mouse fly：鼠标世界点 → 旋翼 setpoint（A 层闭环）+ 武装期 fh-ban。
//
// 2026-08-07 从开环 `ImpactImpulseToward` 换成 F5 那套旋翼。旧实现每 120ms 朝目标发一发
// 冲量（speedScale=4 / maxSpeed=1600），**从不读自己的速度、也没有任何重力配平**——靠
// 超调硬顶重力，所以鼠标不动时要每 200ms「重钉」一次落点才不往下沉（kHoverRepinMs）。
// 换成旋翼后白拿四样：重力配平（真悬停，重钉整段删掉）、速度闭环、撞墙预刹 + 位置包线
// （旧实现能把人直接飞出图，那正是越界断线的来源）、落速闸与深度缴械。
//
// 坐标系可直接对接：`ImpactImpulseToward` 内部就是拿 `worldX - apX` 算差值，而旋翼的
// setpoint / QueryFlightState 用的是同一组 Ap —— ScreenToWorldPoint 的输出无需任何换算。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fly.h"

#include "../ports/teleport_port.h"
#include "../ports/fly_fh_ban.h"
#include "../ports/action_gate.h"
#include "../ports/world_port.h"
#include "../simple_combat/heli_rotor.h"
#include "../invuln/invuln.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"

#include "../../../common/xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace x::features::fly {
namespace {

namespace heli = x::features::simple_combat::heli;

// 屏→世界：回归更新前 `ScreenToWorldPoint(Vector3)` 三参重载。
// 2026-08-03 误把旧 0x4DDEF70 映到四参版 0x4DDD340，且 eye 误用 Mono=0
//（Unity 枚举：Left=0 Right=1 Mono=2）。三参包装在 IDA 内硬编码 eye=2。
// 正确孪生：ScreenToWorldPoint_…824 @ 0x4DDD5F0。
// get_position 走包装（Injected 桩不转发参数）；STW 必须 arity=1 三参重载。
constexpr uint32_t kRvaCamGetMain = 0x4DECFC0;         // remounted 2026-08-06 Camera.get_main
constexpr uint32_t kRvaCamScreenToWorld = 0x4DECAF0;  // remounted 2026-08-06 Camera.ScreenToWorldPoint(Vector3)
constexpr uint32_t kRvaCompGetTransform = 0x4E57220;  // remounted 2026-08-06 Component.get_transform
constexpr uint32_t kRvaTfGetPos = 0x4E71C90;          // remounted 2026-08-06 Transform.get_position

// 1ms 空转会放大 F6 跟飞对主泵的压力；8ms 足够跟手且显著减负。
constexpr DWORD kWorkerSleepMs = 8;
// 目标点刷新间隔（原「hop 冷却」，语义已变）。旧实现里它是发冲量的节奏；现在冲量节奏
// 由旋翼内部自控（~11Hz），这里只管「多久重算一次鼠标对应的世界点」。
// 默认值与下限的取值依据（含实测的 15.625ms 时钟地板）见 xcat_payload_control.h，
// 此处只做别名，别再单独存一份——之前两份各写 40，改一边就会悄悄分叉。
constexpr DWORD kDefaultAimCdMs = xcat::kFlyHopCdDefaultMs;
// 跟随飞：用客户区像素判「鼠标是否动了」。世界坐标死区会在相机跟随后把同一屏点
// STW 漂进 12 内 → 误判静止并钉旧点，表现为抖动、不跟鼠标。
constexpr float kScreenStillPx = 3.f;
// 远于此距离用 Cruise（最快档）。近了**且光标停手**才转 Station 收速；光标还在动就一直
// 留在 Cruise（理由见 DriveRotor 里的选档注释）。F5 打怪没有这条，它的 Station 就是要收速。
constexpr float kCruiseRadiusPx = 140.f;

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

using FnCamMain = void* (*)(const void* method);
using FnScreenToWorld = Vec3* (*)(Vec3* ret, void* cam, const Vec3* screen);
using FnCompTf = void* (*)(void* comp, const void* method);
using FnGetPos = Vec3* (*)(Vec3* ret, void* transform, const void* method);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

HMODULE gGA = nullptr;
FnCamMain gCamMain = nullptr;
FnScreenToWorld gScreenToWorld = nullptr;
FnCompTf gCompTf = nullptr;
FnGetPos gGetPos = nullptr;
MethodInfoHead* gMiCamMain = nullptr;
MethodInfoHead* gMiScreenToWorld = nullptr;
MethodInfoHead* gMiCompTf = nullptr;
MethodInfoHead* gMiGetPos = nullptr;
bool gCamBound = false;

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gArmed{false};
std::atomic<bool> gExternalPause{false};
std::atomic<unsigned> gMode{0};  // 0=NockBack 1=SetImpactNext
std::atomic<unsigned> gAimCdMs{kDefaultAimCdMs};

bool gF6WasDown = false;
bool gLmbWasDown = false;
float gClickScale = 1.f;

// 当前悬停目标（世界坐标，与 Ap 同一空间）。旋翼会一直朝它收敛，所以鼠标不动＝真悬停。
DWORD gLastAimMs = 0;
float gTgtX = 0.f;
float gTgtY = 0.f;
bool gHaveTgt = false;
DWORD gLastFollowLogMs = 0;
long gLastClientX = 0;
long gLastClientY = 0;
bool gHaveLastClient = false;
bool gWasPlayReady = false;
int gLastMapId = -1;
// 因旋翼判死而临时卸掉禁挂台的闩，仅用于去抖，真值以 fly_fh_ban 为准。
bool gBailBanReleased = false;
// 断供探针状态（见 DriveRotor）。发射间隔超过阈值就记一行，正常节拍是 ~90ms。
constexpr DWORD kStarveWarnMs = 250;
DWORD gLastFiredMs = 0;
float gStarveY = 0.f;
char gStarveGuard[16] = {};

// 光标世界速度估计，喂给旋翼前馈（见 heli::Setpoint::leadVx）。
// EMA 系数按「两三次刷新内跟上、又滤掉单次抖动」取；上限只挡离谱毛刺，真正的限速在旋翼。
constexpr float kLeadEma = 0.5f;
constexpr float kLeadMax = 2400.f;
// 超过这么久没有新的跟随刷新，就认定光标停了、前馈归零，否则角色会顺着旧速度一直飘。
// 取值须大于刷新间隔上限（aim cd 最大 400ms），否则连续扫动中途会被误判成停手。
constexpr DWORD kLeadHoldMs = 500;
// 低于此速视为「光标基本停着」，用于选档（见 DriveRotor）。
constexpr float kLeadIdle = 200.f;
float gLeadVx = 0.f;
float gLeadVy = 0.f;
float gPrevTgtX = 0.f;
float gPrevTgtY = 0.f;
DWORD gPrevTgtMs = 0;

float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void ClearLead() {
    gLeadVx = 0.f;
    gLeadVy = 0.f;
    gPrevTgtMs = 0;
}

void ClearFollowTrack() {
    gHaveTgt = false;
    gHaveLastClient = false;
    // 断供探针也要清：跨过一段「没武装」的空窗算出来的 gap 只是关机时长，不是断供。
    gLastFiredMs = 0;
    gStarveGuard[0] = '\0';
    ClearLead();
}

// 换图 / 重新进 PlayReady：丢掉旧图落点与屏点门控，立刻允许首跳。
void NoteMapLandGate(DWORD now) {
    const bool play = ports::world::IsPlayReady();
    const int mapId = play ? ports::world::GetMapId() : -1;
    const bool rose = play && !gWasPlayReady;
    const bool mapChanged = play && mapId > 0 && mapId != gLastMapId && gLastMapId > 0;
    if (rose || mapChanged) {
        ClearFollowTrack();
        gLastAimMs = 0;
        // 换图后旋翼的发射时钟/缴械态要清，否则上一张图的 bailout 一直挡着新图起飞。
        // ★ 必须先确认自己是持有者：Reset 是全局复位（连所有权一起清），F6 没武装时
        //   在这里无条件复位会把正在打怪的 F5 一起掀翻。
        if (heli::CurrentOwner() == heli::Owner::Fly) heli::Reset();
        x::runtime::LogI("Fly", "map land gate open why=%s map=%d", rose ? "play_ready" : "map_id",
                         mapId);
    }
    if (play && mapId > 0) gLastMapId = mapId;
    if (!play) {
        // 卸图：清跟踪，避免落地后用旧世界坐标 soft-repin。
        if (gWasPlayReady) ClearFollowTrack();
        gLastMapId = -1;
    }
    gWasPlayReady = play;
    (void)now;
}

// ⚠️ 换旋翼后这个 mode 对**飞行**已不再起作用：冲量路由由 A 层内部的 ImpactSetVelocity
// 决定，不走 NockBack / SetImpactNext 分支。字段与 IPC 保留只为不破面板与存档兼容，
// 日志里照打便于对照历史 BIN。要真删得连 payload 字段一起清，属另一件事。
const char* ModeName(unsigned mode) {
    return mode == 1u ? "impact_setnext" : "impact_nockback";
}

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

// Unity 无游戏侧哈希名：FindMethodResolved = 明文 → RVA/kind（ScreenToWorldPoint 必须 arity=1）。
MethodInfoHead* ResolveUnityMi(void* klass, uint32_t rva, const char* plain,
                               const x::runtime::il2cpp_method::MethodShape& shape,
                               x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, nullptr);
    if (outPath) *outPath = mr.path;
    if (mr.method && mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
        x::runtime::LogI("Fly", "ResolveUnityMi kind hit rva=0x%X plain=%s", rva,
                         plain ? plain : "-");
    }
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

bool BindCameraApis() {
    if (gCamBound && gCamMain && gScreenToWorld && gGetPos) return true;
    gGA = GetModuleHandleW(L"GameAssembly.dll");
    if (!gGA) return false;

    // Unity 明文名稳定；ScreenToWorldPoint 必须 arity=1（三参包装），避开四参眼位重载。
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    ResolvePath pMain{}, pStw{}, pTf{}, pPos{};
    if (x::runtime::il2cpp::Ensure()) {
        void* camKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Camera");
        void* compKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
        void* tfKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Transform");
        if (camKlass) {
            constexpr MethodShape kMain{0, TypeKind::Ptr, true, true, {}};
            if (!gMiCamMain)
                gMiCamMain =
                    ResolveUnityMi(camKlass, kRvaCamGetMain, "get_main", kMain, &pMain);
            constexpr MethodShape kStw{1, TypeKind::Any, true, true, {TypeKind::Any}};
            if (!gMiScreenToWorld)
                gMiScreenToWorld = ResolveUnityMi(camKlass, kRvaCamScreenToWorld,
                                                  "ScreenToWorldPoint", kStw, &pStw);
        }
        if (compKlass && !gMiCompTf) {
            constexpr MethodShape kTf{0, TypeKind::Ptr, true, true, {}};
            gMiCompTf =
                ResolveUnityMi(compKlass, kRvaCompGetTransform, "get_transform", kTf, &pTf);
        }
        if (tfKlass && !gMiGetPos) {
            constexpr MethodShape kPos{0, TypeKind::Any, true, true, {}};
            gMiGetPos = ResolveUnityMi(tfKlass, kRvaTfGetPos, "get_position", kPos, &pPos);
        }
    }

    gCamMain = FnFromMi<FnCamMain>(gMiCamMain, kRvaCamGetMain);
    gScreenToWorld = FnFromMi<FnScreenToWorld>(gMiScreenToWorld, kRvaCamScreenToWorld);
    gCompTf = FnFromMi<FnCompTf>(gMiCompTf, kRvaCompGetTransform);
    gGetPos = FnFromMi<FnGetPos>(gMiGetPos, kRvaTfGetPos);
    gCamBound = gCamMain && gScreenToWorld && gCompTf && gGetPos;

    static bool sMethodHitsLogged = false;
    if (!sMethodHitsLogged && (gMiCamMain || gMiScreenToWorld || gMiCompTf || gMiGetPos)) {
        sMethodHitsLogged = true;
        // 已缓存的 MI 不会再解析 → 用「本次 path」+「已有 MI」合并计分。
        const ResolvePath paths[4] = {pMain, pStw, pTf, pPos};
        const MethodInfoHead* mis[4] = {gMiCamMain, gMiScreenToWorld, gMiCompTf, gMiGetPos};
        int hits = 0;
        int plain = 0, rva = 0, kind = 0;
        for (int i = 0; i < 4; ++i) {
            if (!mis[i]) continue;
            ++hits;
            if (paths[i] == ResolvePath::Plain) ++plain;
            else if (paths[i] == ResolvePath::Rva) ++rva;
            else if (paths[i] == ResolvePath::Kind) ++kind;
            else if (paths[i] == ResolvePath::Miss) {
                // 复用旧 MI：视为已命中，路径未知 → 计 plain 侧（Unity 主路径）
                ++plain;
            }
        }
        const char* pathLabel = "fallback";
        if (hits == 4 && plain == 4) pathLabel = "plain";
        else if (hits == 4 && rva == 4) pathLabel = "rva";
        else if (hits) pathLabel = "meta-partial";
        x::runtime::LogI("Fly", "methods path=%s hits=%d/4 plain=%d rva=%d kind=%d", pathLabel,
                         hits, plain, rva, kind);
    }

    if (!gCamBound) {
        x::runtime::LogW("Fly", "camera bind fail main=%p stw=%p tf=%p pos=%p", gCamMain,
                         gScreenToWorld, gCompTf, gGetPos);
        char detail[48]{};
        snprintf(detail, sizeof(detail), "mi %d/%d/%d/%d", gMiCamMain ? 1 : 0,
                 gMiScreenToWorld ? 1 : 0, gMiCompTf ? 1 : 0, gMiGetPos ? 1 : 0);
        x::runtime::anchor_lamps::Set("FlyCam", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     detail);
    } else {
        x::runtime::LogI(
            "Fly",
            "camera bind ok mi(main=%d stw=%d tf=%d pos=%d) ScreenToWorld@0x%X get_main@0x%X "
            "get_pos@0x%X",
            gMiCamMain ? 1 : 0, gMiScreenToWorld ? 1 : 0, gMiCompTf ? 1 : 0, gMiGetPos ? 1 : 0,
            kRvaCamScreenToWorld, kRvaCamGetMain, kRvaTfGetPos);
        const int n = (gMiCamMain ? 1 : 0) + (gMiScreenToWorld ? 1 : 0) + (gMiCompTf ? 1 : 0) +
                      (gMiGetPos ? 1 : 0);
        char detail[48]{};
        snprintf(detail, sizeof(detail), "mi %d/4", n);
        x::runtime::anchor_lamps::Set(
            "FlyCam",
            n == 4 ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                   : x::runtime::anchor_lamps::AnchorLampCode::Degraded,
            detail);
    }
    return gCamBound;
}

bool GameWindowFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

HWND GameHwnd() {
    HWND fg = GetForegroundWindow();
    if (!fg) return nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return nullptr;
    return fg;
}

void LoadScaleFromEnv() {
    char env[32]{};
    if (GetEnvironmentVariableA("FLY_CLICK_SCALE", env, sizeof(env)) > 0) {
        const float v = static_cast<float>(atof(env));
        if (v > 0.05f && v < 20.f) gClickScale = v;
    }
}

DWORD AimCdMs() { return static_cast<DWORD>(gAimCdMs.load(std::memory_order_acquire)); }

struct StwJob {
    float unitySx = 0.f;
    float unitySy = 0.f;
    float outX = 0.f;
    float outY = 0.f;
    float camX = 0.f;
    float camY = 0.f;
    float camZ = 0.f;
    bool ok = false;
};

void ScreenToWorldJobFn(void* user) {
    auto* job = reinterpret_cast<StwJob*>(user);
    if (!job) return;
    job->ok = false;
    if (!BindCameraApis()) return;

    void* cam = nullptr;
    void* tf = nullptr;
    Vec3 camPos{};
    Vec3 screen{};
    Vec3 world{};
    __try {
        cam = gCamMain(gMiCamMain);
        if (!cam) return;
        tf = gCompTf(cam, gMiCompTf);
        if (!tf) return;
        gGetPos(&camPos, tf, gMiGetPos);
        float zDist = std::fabs(camPos.z);
        if (zDist < 0.01f) zDist = 10.f;
        screen.x = job->unitySx;
        screen.y = job->unitySy;
        screen.z = zDist;
        gScreenToWorld(&world, cam, &screen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!std::isfinite(world.x) || !std::isfinite(world.y)) return;
    job->camX = camPos.x;
    job->camY = camPos.y;
    job->camZ = camPos.z;
    job->outX = world.x;
    job->outY = world.y;
    job->ok = true;
}

bool ScreenToWorld(float* outX, float* outY, bool verbose) {
    if (!outX || !outY) return false;
    HWND hwnd = GameHwnd();
    if (!hwnd) return false;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;
    if (!ScreenToClient(hwnd, &cursor)) return false;

    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;

    const float unitySx = static_cast<float>(cursor.x) * gClickScale;
    const float unitySy = static_cast<float>(ch - cursor.y) * gClickScale;

    if (!runtime::main_thread::Ensure()) {
        x::runtime::LogW("Fly", "ScreenToWorld main pump missing");
        return false;
    }

    StwJob job{};
    job.unitySx = unitySx;
    job.unitySy = unitySy;
    if (!runtime::main_thread::InvokeAndWait(&ScreenToWorldJobFn, &job, 800)) {
        x::runtime::LogW("Fly", "ScreenToWorld main-thread timeout");
        return false;
    }
    if (!job.ok) {
        x::runtime::LogW("Fly", "ScreenToWorld camera fail client=(%ld,%ld) unity=(%.0f,%.0f)",
                         cursor.x, cursor.y, unitySx, unitySy);
        return false;
    }

    *outX = job.outX;
    *outY = job.outY;
    if (verbose) {
        x::runtime::LogI(
            "Fly",
            "click client=(%ld,%ld) size=%dx%d unity=(%.0f,%.0f) camTf=(%.2f,%.2f,%.2f) "
            "world=(%.2f,%.2f) scale=%.2f",
            cursor.x, cursor.y, cw, ch, unitySx, unitySy, job.camX, job.camY, job.camZ, *outX,
            *outY, gClickScale);
    }
    return true;
}

bool AimReady(DWORD now) {
    const DWORD cd = AimCdMs();
    return !gLastAimMs || (now - gLastAimMs) >= cd;
}

bool ClientToUnityScreen(float* outSx, float* outSy) {
    if (!outSx || !outSy) return false;
    HWND hwnd = GameHwnd();
    if (!hwnd) return false;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;
    if (!ScreenToClient(hwnd, &cursor)) return false;
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) return false;
    const int cw = rc.right - rc.left;
    const int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;
    *outSx = static_cast<float>(cursor.x) * gClickScale;
    *outSy = static_cast<float>(ch - cursor.y) * gClickScale;
    return true;
}

// 设新目标点。只更新 setpoint，不直接发冲量——发不发、发多大交给旋翼闭环。
//
// **不夹进合法空域**：F6 是手动驾驶，指哪儿飞哪儿（见 heli::Setpoint::unbounded）。
// 早先这里夹过一版，结果是把目标点钉在了包线的绊线上，过冲 1px 就吃一发满档下压，
// 表现为「到点→掉落→吸附」的循环（BIN a0ab58）。现在包线本身对 F6 已让位，
// 再夹一次反而是重新造出那条绊线。
//
// track=true 表示「这是连续扫动中的一次刷新」，用它差分出光标的世界速度喂给旋翼前馈
// （见 heli::Setpoint::leadVx）。点击瞬移传 false：那是跳变，差分出来是毛刺不是速度。
void SetTarget(float wx, float wy, const char* tag, bool quiet, bool track) {
    if (!std::isfinite(wx) || !std::isfinite(wy)) return;
    const DWORD now = GetTickCount();
    if (track && gPrevTgtMs) {
        const DWORD dt = now - gPrevTgtMs;
        // 下界挡住 dt≈0 的除零放大，上界挡住「停了很久又动一下」被算成高速扫动。
        if (dt >= 8 && dt <= 300) {
            const float k = 1000.f / static_cast<float>(dt);
            const float rawVx = (wx - gPrevTgtX) * k;
            const float rawVy = (wy - gPrevTgtY) * k;
            gLeadVx += (rawVx - gLeadVx) * kLeadEma;
            gLeadVy += (rawVy - gLeadVy) * kLeadEma;
            gLeadVx = ClampF(gLeadVx, -kLeadMax, kLeadMax);
            gLeadVy = ClampF(gLeadVy, -kLeadMax, kLeadMax);
        }
    } else if (!track) {
        gLeadVx = 0.f;  // 跳变落点：从零重新起估，别把跳变距离当速度发出去
        gLeadVy = 0.f;
    }
    gPrevTgtX = wx;
    gPrevTgtY = wy;
    gPrevTgtMs = now ? now : 1;

    gTgtX = wx;
    gTgtY = wy;
    gHaveTgt = true;
    gLastAimMs = now;
    if (!quiet) {
        x::runtime::LogI("Fly", "aim tag=%s to=(%.0f,%.0f)", tag ? tag : "?", wx, wy);
    }
}

bool ReadClientCursor(long* outX, long* outY) {
    if (!outX || !outY) return false;
    HWND hwnd = GameHwnd();
    if (!hwnd) return false;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;
    if (!ScreenToClient(hwnd, &cursor)) return false;
    *outX = cursor.x;
    *outY = cursor.y;
    return true;
}

bool ScreenCursorMoved(long cx, long cy) {
    if (!gHaveLastClient) return true;
    const float dx = static_cast<float>(cx - gLastClientX);
    const float dy = static_cast<float>(cy - gLastClientY);
    return (dx * dx + dy * dy) >= (kScreenStillPx * kScreenStillPx);
}

void NoteClientCursor(long cx, long cy) {
    gLastClientX = cx;
    gLastClientY = cy;
    gHaveLastClient = true;
}

void PollF6() {
    if (!GameWindowFocused()) {
        gF6WasDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        return;
    }
    const bool down = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (down && !gF6WasDown) {
        const bool next = !gArmed.load(std::memory_order_acquire);
        x::ipc::PayloadControl_PublishFly(next);
        gLmbWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const unsigned mode = gMode.load(std::memory_order_acquire);
        x::runtime::LogI("Fly", "F6 %s mode=%u(%s)", next ? "ARMED" : "OFF", mode, ModeName(mode));
    }
    gF6WasDown = down;
}

void PollLmbHop() {
    if (!gArmed.load(std::memory_order_acquire) ||
        gExternalPause.load(std::memory_order_acquire) ||
        x::features::ports::action_gate::IsSkillCastBusy()) {
        gLmbWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        return;
    }
    if (!GameWindowFocused()) {
        gLmbWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        return;
    }
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (ctrl || shift) {
        gLmbWasDown = down;
        return;
    }
    if (down && !gLmbWasDown) {
        float wx = 0.f, wy = 0.f;
        if (ScreenToWorld(&wx, &wy, /*verbose=*/true)) {
            SetTarget(wx, wy, "click", /*quiet=*/false, /*track=*/false);
        } else {
            x::runtime::LogW("Fly", "ScreenToWorld fail (click)");
        }
    }
    gLmbWasDown = down;
}

// 只负责「目标点跟不跟鼠标」。失焦 / 按住 Ctrl-Shift / 主泵拥堵时**只是不更新目标**，
// 绝不影响下面 DriveRotor 继续悬停——旧实现在这些情况下直接 return，等于停发冲量，
// 而 fh-ban 还挂着，人就往下掉。
void PollAimFollow() {
    if (x::features::ports::action_gate::IsSkillCastBusy()) return;
    if (!GameWindowFocused()) return;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) return;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) return;

    const DWORD now = GetTickCount();
    if (!AimReady(now)) return;
    // 主泵拥堵时宁可不跟这一拍，避免 job timeout 螺旋卡死。
    if (runtime::main_thread::IsCongested()) return;

    long cx = 0, cy = 0;
    if (!ReadClientCursor(&cx, &cy)) return;
    // 屏光标几乎不动：不必重算 STW。旋翼会自己钉住旧目标，不需要「重钉」那套 hack。
    if (!ScreenCursorMoved(cx, cy)) return;
    if (!runtime::main_thread::Ensure()) return;

    float sx = 0.f, sy = 0.f;
    if (!ClientToUnityScreen(&sx, &sy)) return;

    StwJob job{};
    job.unitySx = sx;
    job.unitySy = sy;
    if (!runtime::main_thread::InvokeAndWait(&ScreenToWorldJobFn, &job, 800,
                                            runtime::main_thread::JobPrio::High)) {
        if (!gLastFollowLogMs || now - gLastFollowLogMs > 1000) {
            gLastFollowLogMs = now;
            x::runtime::LogW("Fly", "aim pump timeout");
        }
        return;
    }
    if (!job.ok) {
        if (!gLastFollowLogMs || now - gLastFollowLogMs > 1000) {
            gLastFollowLogMs = now;
            x::runtime::LogW("Fly", "aim ScreenToWorld fail");
        }
        return;
    }
    NoteClientCursor(cx, cy);
    SetTarget(job.outX, job.outY, "follow", /*quiet=*/true, /*track=*/true);
}

// A 层入口。武装期**每个 worker 拍都要调**（旋翼内部自控 ~11Hz，不必外部限频）：
// 只要 fh-ban 挂着，停一拍就是掉一段——这条与 F5 的 TickHeliRotor 是同一条铁律。
void DriveRotor(DWORD now) {
    if (!gArmed.load(std::memory_order_acquire)) return;
    // 外部暂停（auto_supply 补给中）：停发冲量，语义与旧实现「不 hop」一致。
    // 注：此时 fh-ban 仍挂着，角色会持续下坠且接不住地板——这是改造前就有的行为，
    // 本轮原样保留，不在这里顺手改语义。
    if (gExternalPause.load(std::memory_order_acquire)) {
        heli::Disarm(heli::Owner::Fly);
        return;
    }
    if (!ports::world::IsPlayReady()) return;
    // 产品门禁：飞需无敌；不偷偷 SetDesired。（Impact 端口自身也会拒，这里只是早退省开销）
    if (!x::features::invuln::IsEnabled()) return;

    // 旋翼判死（状态停更 / 深度出界）后必须**先卸掉禁挂台**：清 bail 的唯一条件是 onFh，
    // 而禁挂台挂着就永远接不住地板 —— 那是「一路掉到出图也醒不过来」的死锁。注意不能就此
    // return，清 bail 发生在 Tick 内部（在 Bailed 闸之前），停调 Tick 等于把自愈路径也掐了。
    // 落地清掉 bail 后这里再把禁挂台接回去，闭环自愈。语义同 F5 的 SyncImpactFhBan。
    const bool bailed = heli::Bailed();
    if (bailed != gBailBanReleased) {
        gBailBanReleased = bailed;
        ports::fly_fh_ban::SetArmedBan(!bailed);
        x::runtime::LogW("Fly", "rotor bailed=%d -> fh-ban %s", bailed ? 1 : 0,
                         bailed ? "released(let engine catch)" : "restored");
    }

    ports::teleport::FlightState st{};
    const bool haveSt = ports::teleport::QueryFlightState(st) && st.ok;
    // 还没取到目标（刚武装 / 失焦 / STW 未回）：拿当前位置当目标原地悬停。
    // 不能直接 return —— fh-ban 此刻已经挂上了，不发冲量那一段就是自由落体。
    if (!gHaveTgt) {
        if (!haveSt) return;
        // 不走 SetTarget：那会消耗掉刷新冷却，把真正的首次跟鼠标推迟一整个间隔。
        gTgtX = st.x;
        gTgtY = st.y;
        gHaveTgt = true;
    }

    // 光标停手后前馈必须归零，否则角色会顺着最后一次扫动的速度一直飘出去。
    // 判据用「距上次跟随刷新多久」而不是「光标是否移动」：后者由 ScreenCursorMoved 的
    // 死区决定，微抖也算动，会让停手判不出来。
    if (gLastAimMs && now - gLastAimMs > kLeadHoldMs) ClearLead();

    heli::Setpoint sp{};
    sp.x = gTgtX;
    sp.y = gTgtY;
    sp.leadVx = gLeadVx;
    sp.leadVy = gLeadVy;
    // 手动驾驶：左右与上方交还给用户，只保留「不许俯冲出图」那一面。
    sp.unbounded = true;
    if (haveSt) {
        const float dx = sp.x - st.x;
        const float dy = sp.y - st.y;
        // 别把它叫 far：Windows.h 里 far 还是个遗留宏，会把声明整段吃掉。
        const bool outside = (dx * dx + dy * dy) > (kCruiseRadiusPx * kCruiseRadiusPx);
        // 目标在动就必须给快档，哪怕此刻误差很小：Station 的低上限会把前馈裁掉，角色跟不上
        // 光标 ⇒ 误差变大 ⇒ 又切回 Cruise，白白在两档之间来回。Station 只留给「光标停手」。
        const bool moving = (gLeadVx * gLeadVx + gLeadVy * gLeadVy) > (kLeadIdle * kLeadIdle);
        sp.mode = (outside || moving) ? heli::Mode::Cruise : heli::Mode::Station;
    } else {
        sp.mode = heli::Mode::Cruise;
    }
    heli::SetSetpoint(heli::Owner::Fly, sp);

    heli::Telemetry tm{};
    (void)heli::Tick(heli::Owner::Fly, now, &tm);

    // ── 断供探针 ───────────────────────────────────────────────────────
    // 现有证据到这里就分不下去了：UserMove 的 BODY 截到 64B，一次 flush 只看得见前 4 个
    // 元素，于是「vy=+1227 是旋翼下令下降」还是「连续 20 个重力步无人对抗」在包里长得一样。
    // 1Hz 遥测同样抓不到 —— 断供只要几百毫秒就够掉一大截，采样期望连一次都碰不上。
    // 所以在这里按事件记账：只要两次成功发射之间超过 kStarveWarnMs 就必然留一行，
    // 并带上期间最后一个 guard —— 那才是「谁把冲量拦下来的」的直接答案。
    if (tm.fired) {
        if (gLastFiredMs && now - gLastFiredMs > kStarveWarnMs) {
            x::runtime::LogW("Fly", "rotor starve gap=%ums lastGuard=%s vy=%.0f fell=%.0f",
                             static_cast<unsigned>(now - gLastFiredMs),
                             gStarveGuard[0] ? gStarveGuard : "-", tm.vy, gStarveY - tm.y);
        }
        gLastFiredMs = now;
        gStarveGuard[0] = '\0';
        gStarveY = tm.y;
    } else if (tm.guard && tm.guard[0]) {
        // 只留最后一个：cadence 是常态噪声，真凶（impact_fail / congested / stale / bailed）
        // 会紧贴断供尾部，覆盖掉 cadence 正是我们想要的。
        strncpy_s(gStarveGuard, tm.guard, _TRUNCATE);
    }

    if (!gLastFollowLogMs || now - gLastFollowLogMs > 1000) {
        gLastFollowLogMs = now;
        x::runtime::LogI("Fly",
                         "heli mode=%s tgt=(%.0f,%.0f) ap=(%.0f,%.0f) v=(%.0f,%.0f) "
                         "lead=(%.0f,%.0f) des=(%.0f,%.0f) emg=%d since=%ums speed=%.2fX%s%s",
                         heli::ModeName(tm.mode), sp.x, sp.y, tm.x, tm.y, tm.vx, tm.vy,
                         sp.leadVx, sp.leadVy, tm.desiredVx, tm.desiredVy, tm.emergency ? 1 : 0,
                         static_cast<unsigned>(gLastFiredMs ? now - gLastFiredMs : 0),
                         heli::SpeedScale(heli::Owner::Fly), tm.guard && tm.guard[0] ? " guard=" : "",
                         tm.guard ? tm.guard : "");
    }
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("Fly", "worker start Camera.STW scale=%.2f aimCd=%ums drive=heli fh-ban",
                     gClickScale, AimCdMs());
    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        NoteMapLandGate(now);
        PollF6();
        if (gArmed.load(std::memory_order_acquire)) {
            // 顺序要紧：先更目标再驱动，同一拍就能跟上鼠标。禁挂台由 SetArmed 负责。
            PollAimFollow();
            PollLmbHop();
            DriveRotor(now);
        }
        Sleep(kWorkerSleepMs);
    }
    x::runtime::LogI("Fly", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    LoadScaleFromEnv();
    char env[32]{};
    unsigned cd = kDefaultAimCdMs;
    if (GetEnvironmentVariableA("FLY_FOLLOW_CD_MS", env, sizeof(env)) > 0) {
        const int v = atoi(env);
        if (v >= static_cast<int>(xcat::kFlyHopCdMinMs) && v <= 2000) cd = static_cast<unsigned>(v);
    }
    gAimCdMs.store(cd, std::memory_order_release);
    gArmed.store(false, std::memory_order_release);
    gMode.store(0, std::memory_order_release);
    gWorkerStop.store(false, std::memory_order_release);
    gLastAimMs = 0;
    ClearFollowTrack();
    gWasPlayReady = false;
    gLastMapId = -1;
    const char* bin = x::runtime::GetBinDir();
    if (bin && bin[0]) {
        xcat::PayloadControl c{};
        if (xcat::ReadPayloadControl(bin, c)) {
            c.fly = 0;
            c.writeTickMs = GetTickCount64();
            (void)xcat::WritePayloadControl(bin, c);
        } else {
            xcat::ClearFlyArmedSession(bin);
        }
    }
    x::runtime::LogI("Fly", "init drive=heli (closed-loop rotor; open-loop impact removed)");
}

void Shutdown() {
    StopWorker();
    ports::fly_fh_ban::Shutdown();
}

void StartWorker() {
    if (gWorkerThread.load(std::memory_order_acquire)) return;
    gWorkerStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("Fly", "CreateThread fail err=%lu", GetLastError());
        return;
    }
    gWorkerThread.store(th, std::memory_order_release);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 500);
        CloseHandle(th);
    }
    gArmed.store(false, std::memory_order_release);
    ports::fly_fh_ban::SetArmedBan(false);
    heli::Disarm(heli::Owner::Fly);
    heli::Release(heli::Owner::Fly);
    ClearFollowTrack();
}

bool IsArmed() { return gArmed.load(std::memory_order_acquire); }

void SetExternalPause(bool on) {
    const bool prev = gExternalPause.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    x::runtime::LogI("Fly", "external pause %s", on ? "ON" : "OFF");
}

bool IsExternallyPaused() { return gExternalPause.load(std::memory_order_acquire); }

void SetArmed(bool on) {
    const bool prev = gArmed.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    gLmbWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    ClearFollowTrack();
    gBailBanReleased = false;  // 与紧接着这次 SetArmedBan 对齐，别把上一轮的闩带进新一轮
    // 武装：禁挂台；ApplyImpact 放行（Impact 消费 → Attr=2）。
    ports::fly_fh_ban::SetArmedBan(on);
    if (on) {
        // 手动入口用**抢占式** Acquire：人按了 F6 就该立刻听人的，哪怕 F5 正在打怪。
        // 被抢的一方 SetSetpoint/Tick 静默 no-op，它自己的循环不用改；旋翼一拍没停。
        (void)heli::Acquire(heli::Owner::Fly);
    } else {
        // 交还后旋翼变无主，F5/赶路下一拍的 TryAcquire 会把它接回去，不会出现空转期。
        heli::Disarm(heli::Owner::Fly);
        heli::Release(heli::Owner::Fly);
    }
    const unsigned mode = gMode.load(std::memory_order_acquire);
    x::runtime::LogI("Fly", "panel/ipc %s drive=heli mode=%u(%s) fhBan=%d speed=%.2fX",
                     on ? "ARMED" : "OFF", mode, ModeName(mode),
                     ports::fly_fh_ban::IsBanActive() ? 1 : 0,
                     heli::SpeedScale(heli::Owner::Fly));
}

void SetMode(unsigned mode) {
    const unsigned next = xcat::ClampFlyMode(mode);
    const unsigned prev = gMode.exchange(next, std::memory_order_acq_rel);
    if (prev == next) return;
    ClearFollowTrack();
    gLmbWasDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    x::runtime::LogI("Fly", "mode -> %u(%s)", next, ModeName(next));
}

unsigned GetMode() { return gMode.load(std::memory_order_acquire); }

void SetHopCdMs(unsigned ms) {
    // 与面板/IPC 同一口径，下限见 xcat_payload_control.h 的 kFlyHopCdMinMs：那里记着实测的
    // 15.625ms 时钟分辨率——比它小的设定不会报错，只是走同一条路径，不会更跟手。
    if (ms < xcat::kFlyHopCdMinMs) ms = xcat::kFlyHopCdMinMs;
    if (ms > 2000u) ms = 2000u;
    const unsigned prev = gAimCdMs.exchange(ms, std::memory_order_acq_rel);
    if (prev == ms) return;
    x::runtime::LogI("Fly", "aim cd -> %ums", ms);
}

unsigned GetHopCdMs() { return gAimCdMs.load(std::memory_order_acquire); }

void SetSpeedPct(unsigned pct) {
    const float scale = static_cast<float>(pct) / 100.f;
    const float prev = heli::SpeedScale(heli::Owner::Fly);
    heli::SetSpeedScale(heli::Owner::Fly, scale);
    const float now = heli::SpeedScale(heli::Owner::Fly);
    // Clamp 后仍相同就不刷日志：IPC 每次下发全量配置，否则每轮都打一行。
    if (std::fabs(now - prev) < 1e-3f) return;
    x::runtime::LogI("Fly", "speed %u%% -> %.2fX (req %.2f)", pct, now, scale);
}

unsigned SpeedPct() {
    return static_cast<unsigned>(heli::SpeedScale(heli::Owner::Fly) * 100.f + 0.5f);
}

}  // namespace x::features::fly
