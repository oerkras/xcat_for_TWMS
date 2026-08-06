// Classic TWMS — mouse fly.
// Mode A: LMB edge → hop (fill+Doing).
// Mode B: armed → hop toward cursor on self-CD (follow).
// 防漂：Unity Camera/Transform 无游戏哈希 → FindMethodResolved(明文→RVA/kind)；日志 methods hits=4/4。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "fly.h"

#include "../ports/teleport_port.h"
#include "../ports/action_gate.h"
#include "../ports/world_port.h"
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

// 屏→世界：回归更新前 `ScreenToWorldPoint(Vector3)` 三参重载。
// 2026-08-03 误把旧 0x4DDEF70 映到四参版 0x4DDD340，且 eye 误用 Mono=0
//（Unity 枚举：Left=0 Right=1 Mono=2）。三参包装在 IDA 内硬编码 eye=2。
// 正确孪生：ScreenToWorldPoint_…824 @ 0x4DDD5F0。
// get_position 走包装 @0x4E62790（Injected 桩不转发参数）。
constexpr uint32_t kRvaCamGetMain = 0x4DE8FF0;         // remounted 2026-08-04 Camera.get_main
constexpr uint32_t kRvaCamScreenToWorld = 0x4DE8B20;  // remounted 2026-08-04 Camera.ScreenToWorldPoint(Vector3)
constexpr uint32_t kRvaCompGetTransform = 0x4E53250;  // remounted 2026-08-04 Component.get_transform
constexpr uint32_t kRvaTfGetPos = 0x4E6DCC0;          // remounted 2026-08-04 Transform.get_position

constexpr DWORD kWorkerSleepMs = 1;
constexpr DWORD kDefaultHopCdMs = 16;
// 跟随飞：用客户区像素判「鼠标是否动了」。世界坐标死区会在相机跟随后把同一屏点
// STW 漂进 12 内 → 误判静止并 hover 钉旧点，表现为抖动、不跟鼠标。
constexpr float kScreenStillPx = 3.f;
// 屏光标静止时偶尔重钉上次落点（滞空），勿每 CD 狂跳。
constexpr DWORD kHoverRepinMs = 200;

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
std::atomic<unsigned> gMode{1};  // 0=A 1=B
std::atomic<unsigned> gHopCdMs{kDefaultHopCdMs};

bool gF6WasDown = false;
bool gLmbWasDown = false;
float gClickScale = 1.f;

DWORD gLastHopMs = 0;
float gLastHopWx = 0.f;
float gLastHopWy = 0.f;
bool gHaveLastHop = false;
DWORD gLastFollowLogMs = 0;
long gLastClientX = 0;
long gLastClientY = 0;
bool gHaveLastClient = false;
bool gWasPlayReady = false;
int gLastMapId = -1;

void ClearFollowTrack() {
    gHaveLastHop = false;
    gHaveLastClient = false;
}

// 换图 / 重新进 PlayReady：丢掉旧图落点与屏点门控，并清瞬移自冷，立刻允许首跳。
// 假到位由 teleport IsPhysicsReady / RelPos 收态挡住；此处不拉长 F6 起飞手感。
void NoteMapLandGate(DWORD now) {
    const bool play = ports::world::IsPlayReady();
    const int mapId = play ? ports::world::GetMapId() : -1;
    const bool rose = play && !gWasPlayReady;
    const bool mapChanged = play && mapId > 0 && mapId != gLastMapId && gLastMapId > 0;
    if (rose || mapChanged) {
        ClearFollowTrack();
        gLastHopMs = 0;
        ports::teleport::ClearNativeSelfCd();
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

const char* ModeName(unsigned mode) { return mode == 1u ? "follow" : "click"; }

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

DWORD HopCdMs() { return static_cast<DWORD>(gHopCdMs.load(std::memory_order_acquire)); }

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

bool HopReady(DWORD now) {
    const DWORD cd = HopCdMs();
    return !gLastHopMs || (now - gLastHopMs) >= cd;
}

bool HopTo(float wx, float wy, const char* tag, bool quiet) {
    if (!std::isfinite(wx) || !std::isfinite(wy)) return false;
    if (!ports::world::IsPlayReady()) {
        x::runtime::LogW("Fly", "hop skip not_play_ready");
        return false;
    }
    const DWORD cd = HopCdMs();
    ports::teleport::SetNativeCooldownMs(cd);
    if (!ports::teleport::TeleportNativeSkillCall(wx, wy, 0, false)) {
        if (!quiet) {
            x::runtime::LogW("Fly", "hop fail tag=%s to=(%.0f,%.0f) snap=0 fh=0",
                             tag ? tag : "?", wx, wy);
        }
        return false;
    }
    gLastHopMs = GetTickCount();
    gLastHopWx = wx;
    gLastHopWy = wy;
    gHaveLastHop = true;
    if (!quiet) {
        x::runtime::LogI("Fly", "hop ok tag=%s to=(%.0f,%.0f) snap=0 fh=0", tag ? tag : "?", wx,
                         wy);
    } else {
        const DWORD now = gLastHopMs;
        if (!gLastFollowLogMs || now - gLastFollowLogMs > 1000) {
            gLastFollowLogMs = now;
            x::runtime::LogI("Fly", "hover hold to=(%.0f,%.0f) cd=%ums", wx, wy, cd);
        }
    }
    return true;
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
        const DWORD now = GetTickCount();
        if (!HopReady(now)) {
            gLmbWasDown = down;
            return;
        }
        float wx = 0.f, wy = 0.f;
        if (ScreenToWorld(&wx, &wy, /*verbose=*/true)) {
            (void)HopTo(wx, wy, "A", /*quiet=*/false);
        } else {
            x::runtime::LogW("Fly", "ScreenToWorld fail (A)");
        }
    }
    gLmbWasDown = down;
}

void PollFollowMouse() {
    if (!gArmed.load(std::memory_order_acquire)) return;
    if (gExternalPause.load(std::memory_order_acquire)) return;
    if (x::features::ports::action_gate::IsSkillCastBusy()) return;
    if (!GameWindowFocused()) return;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) return;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) return;

    const DWORD now = GetTickCount();
    if (!HopReady(now)) return;

    long cx = 0, cy = 0;
    if (!ReadClientCursor(&cx, &cy)) return;

    // 屏光标几乎不动：不 STW、不每拍 hover（防相机反馈抖）；偶发重钉保滞空。
    if (!ScreenCursorMoved(cx, cy)) {
        if (gHaveLastHop && (now - gLastHopMs) >= kHoverRepinMs) {
            (void)HopTo(gLastHopWx, gLastHopWy, "Bh", /*quiet=*/true);
        }
        return;
    }

    float wx = 0.f, wy = 0.f;
    if (!ScreenToWorld(&wx, &wy, /*verbose=*/false)) {
        if (!gLastFollowLogMs || now - gLastFollowLogMs > 1000) {
            gLastFollowLogMs = now;
            x::runtime::LogW("Fly", "follow ScreenToWorld fail");
        }
        // STW 失败也不要 5ms 狂钉；等下次鼠标再动或软重钉窗口。
        if (gHaveLastHop && (now - gLastHopMs) >= kHoverRepinMs) {
            (void)HopTo(gLastHopWx, gLastHopWy, "Bh", /*quiet=*/true);
        }
        return;
    }
    NoteClientCursor(cx, cy);
    (void)HopTo(wx, wy, "B", /*quiet=*/false);
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("Fly", "worker start Camera.STW scale=%.2f cd=%ums", gClickScale, HopCdMs());
    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const DWORD now = GetTickCount();
        NoteMapLandGate(now);
        PollF6();
        if (gMode.load(std::memory_order_acquire) == 1u) {
            PollFollowMouse();
        } else {
            PollLmbHop();
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
    unsigned cd = kDefaultHopCdMs;
    if (GetEnvironmentVariableA("FLY_FOLLOW_CD_MS", env, sizeof(env)) > 0) {
        const int v = atoi(env);
        if (v >= 5 && v <= 2000) cd = static_cast<unsigned>(v);
    }
    gHopCdMs.store(cd, std::memory_order_release);
    gArmed.store(false, std::memory_order_release);
    gMode.store(1, std::memory_order_release);
    gWorkerStop.store(false, std::memory_order_release);
    gLastHopMs = 0;
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
    x::runtime::LogI("Fly", "init fly A/B (click/follow hop; arm session-only)");
}

void Shutdown() { StopWorker(); }

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
    const unsigned mode = gMode.load(std::memory_order_acquire);
    x::runtime::LogI("Fly", "panel/ipc %s mode=%u(%s)", on ? "ARMED" : "OFF", mode, ModeName(mode));
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
    if (ms < 5u) ms = 5u;
    if (ms > 2000u) ms = 2000u;
    const unsigned prev = gHopCdMs.exchange(ms, std::memory_order_acq_rel);
    if (prev == ms) return;
    x::runtime::LogI("Fly", "hop cd -> %ums", ms);
}

unsigned GetHopCdMs() { return gHopCdMs.load(std::memory_order_acquire); }

}  // namespace x::features::fly
