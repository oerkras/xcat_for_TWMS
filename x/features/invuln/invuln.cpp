// TWMS Classic — data-plane invuln v2.6.8 (land-pin after InterStage 2026-08-09).
//
// Hit gate: User+0x270 i-frame (~100ms worker top-up).
// Anti-blink hybrid: MainPump frame tick (before+after SendWill) + worker 8ms backup.
// Soft +0x20C/+0x210 DISABLED. Optional layout probe: XCAT_INVULN_PROBE=1 (default off).
// Bind SSOT: WM.MyUser@+0x28 first (same as Drop/Skill/Combat); FindAll fallback.
// Rebind: WM path every tick when unbound; FindAll 仅 IsPlayReady（禁 InterStage/Login/
// CashShop/GlobalMarket — BIN D217：拍卖 scene=5 时 80ms FindAll 堵 MainPump）。
// 裸 gFindAll：泵内硬检 LoginFreeze + MapTransitBlock + PlayReady。
// BIN 7ae984：!PlayReady（InterStage 等）停写 SS/hit + 卸 FrameTick，避免迁频窗污染/抢主线程。
// map_id quiet：挡「MapId 闪仍 PlayReady」进门脏窗（BIN 02:48）；回场急钉靠 resume
// 先于 quiet continue（BIN 07:51：旧顺序让 quiet 活过 InterStage → 落地无伤真空）。
// 不在 !PlayReady 提前清 quiet，避免脏窗内 PlayReady 闪断误开写闸。
// 1.5s ACCEPT grace; LU drop keeps SecondaryStat（仅 PlayReady 内写）。
// No hotkey — panel / [core] invuln / XCAT_INVULN=1 only.
// Docs: docs/features/invuln/模块设计.md
// Remount 2026-08-06: GA MD5 c7a3842d…; User=b8c9aedb…; UserLocal=d81db6fb…;
// SS=fda0a837… @WM+0xF0（勿用 +0xB8 嵌套 struct）；字段偏移未漂，类/字段哈希已换。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "invuln.h"

#include "../../ipc/payload_control.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"
#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../../ui/player_vitals.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"

#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Psapi.lib")

namespace x {
namespace features {
namespace invuln {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// True UserLocal → il2cpp_shape::ResolveUserLocalKlass（hash d81db6fb… + Teleport@0x3C8）

// User / SecondaryStat：dump.cs 2026-08-06 字段哈希 → field_get_offset；失败回退下方 kFb*
// User=b8c9aedb…（TDI 1560）；SS=fda0a837…（TDI 1329，WM+0xF0）
constexpr char kSecondaryStatClass[] =
    "d76746e1e5090a3f7627f84d73ef602cea9ec903277cda194d79c833738095c";
constexpr char kUserClass[] =
    "ead9ab2e851cf06879704044ce549197d7fb5017ecb635104f7a6e366f9ac7a";
constexpr char kHashNInv[] =
    "f64c7f0a2dd79dd4b99453cd2a7303e236dfe8b29cf533887c40907b9bbe589";
constexpr char kHashRInv[] =
    "cda108a683cee0a865c8cddb2f1afd6bb2ee7ccc2c47686281bd2b2ef7c95ca";
constexpr char kHashTInv[] =
    "a86b2c31e0a90e3d8e072713def5f7d6efe4c6dbb160f11dd78d3bb45c599ea";
constexpr char kHashNDojang[] =
    "ce5569a8a97c410cdcb56c8c89ab9bcf3d552caa8759c360a82ddf725bf1dac";
constexpr char kHashRDojang[] =
    "dfbfbe8a06817f5bd8d8d58d99231c316e3f184217cfc63cab0a284932285a0";
constexpr char kHashTDojang[] =
    "e3298588bcd96aa6e129f07ecebd4ba57aafdfa7c79749bcfd679432791df51";
constexpr char kHashHitPeriodRemain[] =
    "a4b98d2767fc79577d42a02e7de66de11b19cef11d0257c66b72f51642e2bae";
constexpr char kHashLayerStateCounter[] =
    "dd4296d39345bd2ebdeca4651486d98e3fdaf60aaaf3063e2f169cc224d922c";
constexpr char kHashLogicalPos[] =
    "fbb96ebf96fbe214ee24fcbda14d147b6fe121817305ba2191c875f7367f88c";
// VisPos 在 User 祖先 edc85ce2…（MonoBehaviour 派生）@+0x64
constexpr char kHashVisPos[] =
    "dfe2e5f14ea04b0ca19e131b66bb4d53e06ca0c22744c299a991cd322e915e4";
constexpr char kHashSoftTickA[] =
    "bb14cecd6e35b74806d3df1b30f0d7cc392abe06fd2a8efe5382a6fe443b040";
constexpr char kHashSoftTickB[] =
    "ab67b975e252a0898e6954376dc1b0bb6a5f6895218dd287cb806ace9098d25";

constexpr size_t kFbWmSecondaryStat = 0xF0;

constexpr size_t kFbNInv = 0xEC;
constexpr size_t kFbRInv = 0xF0;
constexpr size_t kFbTInv = 0xF4;
constexpr size_t kFbNDojang = 0x2C4;
constexpr size_t kFbRDojang = 0x2C8;
constexpr size_t kFbTDojang = 0x2CC;
constexpr size_t kFbHitPeriodRemain = 0x270;
constexpr size_t kFbLayerStateCounter = 0x280;
constexpr size_t kFbVisPos = 0x64;
constexpr size_t kFbLogicalPos = 0x2B0;
constexpr size_t kFbSoftTickA = 0x20C;
constexpr size_t kFbSoftTickB = 0x210;

size_t gOffNInv = kFbNInv;
size_t gOffRInv = kFbRInv;
size_t gOffTInv = kFbTInv;
size_t gOffNDojang = kFbNDojang;
size_t gOffRDojang = kFbRDojang;
size_t gOffTDojang = kFbTDojang;
size_t gOffHitPeriodRemain = kFbHitPeriodRemain;
size_t gOffLayerStateCounter = kFbLayerStateCounter;
size_t gOffVisPos = kFbVisPos;
size_t gOffLogicalPos = kFbLogicalPos;
size_t gOffSoftTickA = kFbSoftTickA;
size_t gOffSoftTickB = kFbSoftTickB;
bool gInvFieldTried = false;

#define kOffWmMyUser (x::ui::player::OffWmMyUser())
#define kOffNInv (gOffNInv)
#define kOffRInv (gOffRInv)
#define kOffTInv (gOffTInv)
#define kOffNDojang (gOffNDojang)
#define kOffRDojang (gOffRDojang)
#define kOffTDojang (gOffTDojang)
#define kOffHitPeriodRemain (gOffHitPeriodRemain)
#define kOffLayerStateCounter (gOffLayerStateCounter)
#define kOffVisPos (gOffVisPos)
#define kOffLogicalPos (gOffLogicalPos)
constexpr uint32_t kLayerCounterOpaque = 2;
constexpr float kMinPosAbs = 1.0f;
constexpr size_t kOffCachedPtr = 0x10;

constexpr int kNInv = 1;
constexpr int kRInv = 1010;
constexpr int kNDojang = 1;
constexpr int kRDojang = 1010;
constexpr int kHitPeriodKeep = 5000;
constexpr DWORD kGateRefreshMs = 100;
// Hybrid anti-blink: frame tick is primary; worker backup covers Update races.
constexpr DWORD kAntiBlinkBackupMs = 8;
// FindAll fallback throttle (steady MapScene). WM.MyUser path is unthrottled when unbound.
constexpr DWORD kRebindMs = 400;
// 仅 IsPlayReady 内短暂无绑：加快 FindAll。非玩法场景禁止 FindAll（见 SkipFindAllTransit）。
constexpr DWORD kRebindFastMs = 80;
// FindAll 失败路径刷屏节流（重试仍按 kRebind*；日志 10s 一条）。
constexpr DWORD kBindFailLogMs = 10000;
// After ACCEPT, spawn pos may be (0,0) briefly — keep bind so hit gate can arm.
constexpr DWORD kBindGraceMs = 1500;
constexpr DWORD kWorkerSleepOnMs = 8;
constexpr DWORD kWorkerSleepOffMs = 16;
constexpr DWORD kProbeMs = 1000;
constexpr DWORD kPumpRetryMs = 2000;
// BIN 02:18：刚回 Field 立刻 FindAll → job timeout + pump idle。
// 只挡 FindAll；WM.MyUser 快路径不受影响。2026-08-09：2000→500 加快兜底重绑。
constexpr DWORD kLandQuietMs = 500;
DWORD gLandQuietUntilMs = 0;
// soft hold：整段停写（大厅 / 未进图）。
// soft land quiet：已回图，允许 WM.MyUser 急钉+维持；仍禁 FindAll/帧钉与 Combat 齐抢泵。
// quiet 结束后再 arm 帧钉（pending）；不再用 400ms stagger 拖无敌。
bool gPendingSoftLandResume = false;
DWORD gSoftInvulnStaggerUntilMs = 0;  // 保留字段清零兼容；不再武装错峰延钉
// BIN 02:48：手动换图 MapId 已闪仍 PlayReady 时 Invuln 还写 LU → InterStage 永卡。
// MapId 变即静默禁写；回场由 resume 清 quiet（勿在 !PlayReady 闪断时提前放开）。
constexpr DWORD kMapIdQuietMs = 1000;
DWORD gMapIdQuietUntilMs = 0;
int gQuietTrackMapId = 0;

bool InMapIdQuiet(DWORD now) {
    return gMapIdQuietUntilMs != 0 && static_cast<int>(now - gMapIdQuietUntilMs) < 0;
}
// CMS-named TW candidates (read-only; never write)
#define kOffSoftTickA (gOffSoftTickA)
#define kOffSoftTickB (gOffSoftTickB)
#define kOffCurPosY (gOffLogicalPos + 4)


bool PlausibleInvOff(size_t off) { return off >= 0x10 && off < 0x800; }

bool InvFieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
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
            if (PlausibleInvOff(off)) {
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

void EnsureInvulnFieldOff() {
    if (gInvFieldTried) return;
    gInvFieldTried = true;
    void* ss = x::runtime::il2cpp::FindClass("", kSecondaryStatClass);
    void* user = x::runtime::il2cpp::FindClass("", kUserClass);
    int hits = 0;
    auto hit = [&](void* klass, const char* hash, size_t fb, size_t* slot) {
        if (InvFieldOffHit(klass, hash, fb, slot)) ++hits;
    };
    hit(ss, kHashNInv, kFbNInv, &gOffNInv);
    hit(ss, kHashRInv, kFbRInv, &gOffRInv);
    hit(ss, kHashTInv, kFbTInv, &gOffTInv);
    hit(ss, kHashNDojang, kFbNDojang, &gOffNDojang);
    hit(ss, kHashRDojang, kFbRDojang, &gOffRDojang);
    hit(ss, kHashTDojang, kFbTDojang, &gOffTDojang);
    hit(user, kHashHitPeriodRemain, kFbHitPeriodRemain, &gOffHitPeriodRemain);
    hit(user, kHashLayerStateCounter, kFbLayerStateCounter, &gOffLayerStateCounter);
    hit(user, kHashLogicalPos, kFbLogicalPos, &gOffLogicalPos);
    hit(user, kHashVisPos, kFbVisPos, &gOffVisPos);
    hit(user, kHashSoftTickA, kFbSoftTickA, &gOffSoftTickA);
    hit(user, kHashSoftTickB, kFbSoftTickB, &gOffSoftTickB);
    x::runtime::LogI("Invuln", "fields path=%s hits=%d/12 nInv=0x%zX hit=0x%zX curPos=0x%zX",
                     hits == 12 ? "meta" : (hits ? "meta-partial" : "fallback"), hits, gOffNInv,
                     gOffHitPeriodRemain, gOffLogicalPos);
}

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;

void* gLuType = nullptr;
void* gLocalUser = nullptr;
DWORD gLuBoundTick = 0;  // GetTickCount at MyUser ACCEPT (grace for spawn pos)
std::vector<void*> gSecondaryStats;  // all WM SS pointers

std::atomic<bool> gDesired{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<bool> gFrameBlink{false};
HANDLE gLog = INVALID_HANDLE_VALUE;
HANDLE gLogTemp = INVALID_HANDLE_VALUE;
DWORD gTickCount = 0;

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

void WriteLogHandle(HANDLE h, const char* buf, int n) {
    if (h == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, nullptr);
    FlushFileBuffers(h);
}

void Log(const char* fmt, ...) {
    char body[900];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[1024];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond,
                     body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteLogHandle(gLog, buf, n);
    WriteLogHandle(gLogTemp, buf, n);
    OutputDebugStringA(buf);
    if (std::strncmp(body, "heartbeat ", 10) == 0 || std::strncmp(body, "refresh ", 8) == 0 ||
        std::strncmp(body, "  ss[", 5) == 0) {
        x::runtime::LogIThrottled(2, 30000, "Invuln", "%s", body);
        return;
    }
    x::runtime::LogI("Invuln", "%s", body);
}

bool DirExists(const std::wstring& dir) {
    const DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &self) ||
        !self)
        return L".";
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L".";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return s.substr(0, slash);
}

void OpenLogs() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"invuln.log");
    wchar_t tmp[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tmp)) {
        std::wstring t(tmp);
        while (!t.empty() && t.back() == L'\\') t.pop_back();
        gLogTemp = x::runtime::OpenRotatingDbgLog(t, L"xcat_invuln.log");
    }
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32(void* obj, size_t off, int v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteU32(void* obj, size_t off, uint32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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

float ReadF32(void* obj, size_t off) {
    const uint32_t bits = ReadU32(obj, off);
    float f = 0.f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

bool ReadIl2CppString(void* str, char* out, size_t outCap) {
    if (!str || !out || outCap < 2) return false;
    __try {
        const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const auto* chars = reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            out[n++] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetGoName(void* comp, char* out, size_t outCap) {
    out[0] = 0;
    if (!comp || !gCompGo || !gObjName) return false;
    void* go = nullptr;
    __try {
        go = gCompGo(comp, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!go) return false;
    void* nameObj = nullptr;
    __try {
        nameObj = gObjName(go, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ReadIl2CppString(nameObj, out, outCap);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

bool BindApis() {
    if (!x::runtime::il2cpp::Ensure()) {
        Log("BindApis: no GameAssembly");
        return false;
    }
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gCompGo = e.compGo;
    gObjName = e.objName;
    if (!gFindAll) {
        Log("BindApis: missing il2cpp export / RVA");
        return false;
    }
    Log("BindApis ok GA=%p FindAll=%p", gGA, gFindAll);
    return true;
}

// tInvincible_ / tDojangShield_ 是「游戏钟绝对到期」，判定见 SecondaryStat.CheckByTime
// (RVA 0xD63990)：tCur - t > -100 即清除，tCur = WorldManager.GetUpdateTime()。
// 旧实现用 GetTickCount（开机毫秒）与该钟差 1~2 个数量级，纯属侥幸没被判到期。
// 本函数每 8~100ms 被调用，游戏钟按 1s 缓存，避免高频进 il2cpp。
constexpr int kInvExpireAheadMs = 3600 * 1000;
constexpr int kInvExpireFallback = 0x0FFFFFFF;  // ≈74h，游戏钟不可用时的保守远期值

int ExpireOrRemainMs() {
    static DWORD lastTryAt = 0;
    static int cachedGameMs = 0;
    const DWORD now = GetTickCount();
    // 节流「尝试」而非「成功」：未绑定时若按成功节流，会以 8ms 频率反复进 il2cpp 解析。
    if (!lastTryAt || now - lastTryAt >= 1000) {
        lastTryAt = now ? now : 1;
        const int t = x::features::ports::skill::GetGameUpdateTimeMs();
        if (t > 0) cachedGameMs = t;
    }
    if (cachedGameMs <= 0) return kInvExpireFallback;
    const long long expire = static_cast<long long>(cachedGameMs) + kInvExpireAheadMs;
    return expire > kInvExpireFallback ? kInvExpireFallback : static_cast<int>(expire);
}

void WriteSsFields(void* ss, bool on) {
    EnsureInvulnFieldOff();
    if (!ss) return;
    if (on) {
        const int t = ExpireOrRemainMs();
        WriteI32(ss, kOffNInv, kNInv);
        WriteI32(ss, kOffRInv, kRInv);
        WriteI32(ss, kOffTInv, t);
        WriteI32(ss, kOffNDojang, kNDojang);
        WriteI32(ss, kOffRDojang, kRDojang);
        WriteI32(ss, kOffTDojang, t);
    } else {
        WriteI32(ss, kOffNInv, 0);
        WriteI32(ss, kOffRInv, 0);
        WriteI32(ss, kOffTInv, 0);
        WriteI32(ss, kOffNDojang, 0);
        WriteI32(ss, kOffRDojang, 0);
        WriteI32(ss, kOffTDojang, 0);
    }
}

bool TryResolveWorldManagers() {
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    void* wm = x::features::ports::world::GetWorldManager();
    if (!wm) return false;
    // SSOT：WM+0xF0（fda0a837…）。勿用 +0xB8（08-06 dump 为嵌套小 struct）。
    void* ss = x::ui::player::LocalSecondaryStat();
    if (!LooksLikeHeapPtr(ss)) ss = ReadPtr(wm, kFbWmSecondaryStat);
    gSecondaryStats.clear();
    if (LooksLikeHeapPtr(ss)) {
        gSecondaryStats.push_back(ss);
        Log("WM via world_port wm=%p ss@F0=%p", wm, ss);
    }
    return !gSecondaryStats.empty();
}

bool PosLooksAliveXY(float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    return std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs;
}

bool ProbeSsAlive(void* ss) {
    if (!ss) return false;
    __try {
        if (!*reinterpret_cast<void**>(ss)) return false;
        (void)*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ss) + kOffNInv);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SecondaryStatsAlive() {
    if (gSecondaryStats.empty()) return false;
    for (void* ss : gSecondaryStats) {
        if (!ProbeSsAlive(ss)) return false;
    }
    return true;
}

bool InBindGrace() {
    if (!gLuBoundTick) return false;
    return (GetTickCount() - gLuBoundTick) < kBindGraceMs;
}

bool LocalUserStillAlive() {
    if (!gLocalUser) return false;
    __try {
        if (!*reinterpret_cast<void**>(gLocalUser)) return false;
        const intptr_t cached =
            *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffCachedPtr);
        // Do NOT call GetGoName here — it is managed/GC and this runs on a worker.
        // Name was verified at bind time; pos/cachedPtr catch teardown shells.
        // Spawn settle: allow brief (0,0)/cached=0 window after MyUser ACCEPT.
        if (cached == 0 && !InBindGrace()) return false;
        const float visX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos);
        const float visY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffVisPos + 4);
        const float logX =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos);
        const float logY =
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffLogicalPos + 4);
        if (!PosLooksAliveXY(visX, visY) && !PosLooksAliveXY(logX, logY)) {
            if (InBindGrace()) return true;
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ClearLocalUser(const char* why) {
    if (gLocalUser) Log("drop LocalUser (%s) lu=%p", why, gLocalUser);
    gLocalUser = nullptr;
    gLuBoundTick = 0;
}

void ClearSecondaryStats(const char* why) {
    if (!gSecondaryStats.empty())
        Log("drop SS (%s) ssN=%zu", why, gSecondaryStats.size());
    gSecondaryStats.clear();
}

bool IsMyUserGo(void* user) {
    char name[96]{};
    return GetGoName(user, name, sizeof(name)) && _stricmp(name, "MyUser") == 0;
}

// Unity Component alive: klass + m_CachedPtr. Spawn may briefly have cached=0 in Field.
bool UnityUserAlive(void* user, bool allowZeroCached) {
    if (!LooksLikeHeapPtr(user)) return false;
    __try {
        if (!*reinterpret_cast<void**>(user)) return false;
        const intptr_t cached =
            *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(user) + kOffCachedPtr);
        if (cached == 0 && !allowZeroCached) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* PeekWmMyUser() {
    using x::features::ports::world::PeekWorldManager;
    void* wm = PeekWorldManager();
    if (!wm) return nullptr;
    return ReadPtr(wm, kOffWmMyUser);
}

void ApplyInvuln(bool on);  // AcceptLocalUser 急钉

bool AcceptLocalUser(void* lu, const char* how) {
    if (!lu) return false;
    gLocalUser = lu;
    gLuBoundTick = GetTickCount();
    Log("LocalUser ACCEPT %s lu=%p hit298=%d grace=%ums", how, gLocalUser,
        ReadI32(gLocalUser, kOffHitPeriodRemain), (unsigned)kBindGraceMs);
    x::runtime::managed_main::SetLoginFreeze(false);
    // 冷启/重绑：ACCEPT 当拍急钉，勿等下一轮 gate（BIN 00:28：ACCEPT hit298=330 → 2s 后才 refresh）。
    if (gDesired.load() && x::features::ports::world::IsPlayReady()) {
        ApplyInvuln(true);
        Log("land pin hit298=%d (ACCEPT %s)", ReadI32(gLocalUser, kOffHitPeriodRemain),
            how ? how : "?");
    }
    return true;
}

// Fast path: WM.MyUser SSOT (same as DropPort / SkillPort). No FindAll.
bool TryBindWmMyUser() {
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    // 拍卖/商城/过渡：不写 hit、也不为校 GO 名占 MainPump。
    if (!x::features::ports::world::IsPlayReady()) return false;
    void* mu = PeekWmMyUser();
    if (!LooksLikeHeapPtr(mu)) return false;

    const bool field =
        x::features::ports::world::GetSceneState() == x::features::ports::world::SceneState::Field;
    if (!UnityUserAlive(mu, /*allowZeroCached=*/field)) return false;

    // Field：WM 指针即权威，跳过 GetGoName/MainPump（换图空窗里 pump 排队会拖到 400ms+）。
    if (field) {
        return AcceptLocalUser(mu, "wm.MyUser");
    }

    // 非地图但仍 PlayReady（极少）：校 GO 名，防登录壳误绑。
    struct Ctx {
        void* mu = nullptr;
        bool ok = false;
    } ctx;
    ctx.mu = mu;
    auto job = [](void* user) {
        auto* c = reinterpret_cast<Ctx*>(user);
        c->ok = IsMyUserGo(c->mu);
    };
    if (!x::runtime::managed_main::Call(+job, &ctx, 800)) return false;
    if (!ctx.ok) return false;
    return AcceptLocalUser(mu, "wm.MyUser");
}

// 非玩法就绪：Unity FindObjects 会拖黑屏 / 堵拍卖·商城主线程；只靠 WM.MyUser，禁止 FindAll。
// SSOT = IsPlayReady（覆盖 InterStage/None/Login/CashShop/GlobalMarket）。
bool SkipFindAllTransit() {
    return !x::features::ports::world::IsPlayReady();
}

bool TryResolveLocalUserFindAll() {
    if (x::runtime::managed_main::IsLoginFrozen()) return false;
    if (SkipFindAllTransit()) return false;
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType || !gFindAll) return false;

    struct Ctx {
        bool ok = false;
        bool logNoise = false;
    } ctx;
    {
        static DWORD s_lastNoise = 0;
        const DWORD now = GetTickCount();
        if (!s_lastNoise || now - s_lastNoise >= kBindFailLogMs) {
            s_lastNoise = now;
            ctx.logNoise = true;
        }
    }
    auto job = [](void* user) {
        auto* c = reinterpret_cast<Ctx*>(user);
        // 裸 gFindAll 绕过 managed_main::MapTransitBlock —— 泵内必须自检仓级闸。
        if (x::runtime::managed_main::IsLoginFrozen() ||
            x::runtime::managed_main::IsMapTransitBlocked() ||
            !x::features::ports::world::IsPlayReady()) {
            c->ok = false;
            return;
        }
        void* arr = nullptr;
        __try {
            arr = gFindAll(gLuType, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (c->logNoise) Log("LocalUser FindAll SEH");
            c->ok = false;
            return;
        }
        const uintptr_t n = ArrayLen(arr);
        if (c->logNoise) Log("LocalUser FindAll count=%llu", (unsigned long long)n);
        void* best = nullptr;
        for (uintptr_t i = 0; i < n && i < 64; ++i) {
            void* obj = ArrayAt(arr, i);
            if (!obj) continue;
            char name[96]{};
            GetGoName(obj, name, sizeof(name));
            if (c->logNoise)
                Log("LocalUser[%llu]=%p name=\"%s\" hit298=%d", (unsigned long long)i, obj, name,
                    ReadI32(obj, kOffHitPeriodRemain));
            if (name[0] && _stricmp(name, "MyUser") == 0) {
                best = obj;
                break;
            }
        }
        if (!best) {
            if (c->logNoise) Log("LocalUser REJECT (no MyUser)");
            c->ok = false;
            return;
        }
        c->ok = AcceptLocalUser(best, "FindAll");
    };
    if (!x::runtime::managed_main::Call(+job, &ctx, 2500)) {
        static DWORD s_lastPumpFail = 0;
        const DWORD now = GetTickCount();
        if (!s_lastPumpFail || now - s_lastPumpFail >= kBindFailLogMs) {
            s_lastPumpFail = now;
            Log("LocalUser Resolve: main pump fail");
        }
        return false;
    }
    return ctx.ok;
}

bool TryResolveLocalUser() {
    if (TryBindWmMyUser()) return true;
    return TryResolveLocalUserFindAll();
}

// True when WM.MyUser drifted or cleared — old cache must not keep writing.
bool WmMyUserDrifted() {
    void* mu = PeekWmMyUser();
    if (!gLocalUser) return false;
    if (!LooksLikeHeapPtr(mu)) return true;  // mid-hop empty → drop cache
    return mu != gLocalUser;
}

DWORD RebindIntervalMs() {
    // 图内无绑才加速；卸图空窗走 SkipFindAllTransit，不再误用 80ms 狂扫。
    if (!gLocalUser) return kRebindFastMs;
    return kRebindMs;
}

void ApplySsOnly(bool on) {
    if (on && !SecondaryStatsAlive()) gSecondaryStats.clear();
    if (!on && !SecondaryStatsAlive()) gSecondaryStats.clear();
    for (void* ss : gSecondaryStats) WriteSsFields(ss, on);
}

void ApplyHitGate(bool on) {
    if (!gLocalUser) return;
    WriteI32(gLocalUser, kOffHitPeriodRemain, on ? kHitPeriodKeep : 0);
}

void ApplyAntiBlink() {
    if (!gLocalUser) return;
    // InterStage / 卸图：禁写 layer；帧回调也可能晚到一拍。
    if (!x::features::ports::world::IsPlayReady()) return;
    if (InMapIdQuiet(GetTickCount())) return;
    // Lightweight: no StillAlive / FindAll — safe for main-thread frame tick.
    WriteU32(gLocalUser, kOffLayerStateCounter, kLayerCounterOpaque);
}

// MainPump sticky tick (after SendWill/Update). Data-plane only.
void AntiBlinkFrameTick(void*) {
    if (!gDesired.load(std::memory_order_relaxed)) return;
    if (!x::features::ports::world::IsPlayReady()) return;
    if (InMapIdQuiet(GetTickCount())) return;
    ApplyAntiBlink();
}

bool TryArmFrameBlink(const char* why) {
    if (!x::features::ports::world::IsPlayReady()) {
        gFrameBlink.store(false);
        return false;
    }
    if (!x::runtime::main_thread::Ensure()) {
        gFrameBlink.store(false);
        return false;
    }
    x::runtime::main_thread::SetFrameTick(&AntiBlinkFrameTick, nullptr);
    gFrameBlink.store(true);
    Log("anti-blink frame-tick armed (%s)", why ? why : "?");
    return true;
}

void DisarmFrameBlink() {
    x::runtime::main_thread::SetFrameTick(nullptr, nullptr);
    gFrameBlink.store(false);
}

bool ProbeEnabled() {
    char buf[16]{};
    if (GetEnvironmentVariableA("XCAT_INVULN_PROBE", buf, sizeof(buf)) == 0) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

// Read-only: interpret +0x20C/+0x210 as both int tick and float; compare CurPos@+0x2B0.
void ProbeSoftSlots(const char* tag) {
    if (!gLocalUser || !LocalUserStillAlive()) return;
    static float sLastCx = 0.f, sLastCy = 0.f;
    static bool sHaveLast = false;

    const int i228 = ReadI32(gLocalUser, kOffSoftTickA);
    const int i22c = ReadI32(gLocalUser, kOffSoftTickB);
    const float f228 = ReadF32(gLocalUser, kOffSoftTickA);
    const float f22c = ReadF32(gLocalUser, kOffSoftTickB);
    const float cx = ReadF32(gLocalUser, kOffLogicalPos);
    const float cy = ReadF32(gLocalUser, kOffCurPosY);
    const float vx = ReadF32(gLocalUser, kOffVisPos);
    const float vy = ReadF32(gLocalUser, kOffVisPos + 4);
    const int hit = ReadI32(gLocalUser, kOffHitPeriodRemain);
    const uint32_t layer = ReadU32(gLocalUser, kOffLayerStateCounter);

    const char* motion = "idle";
    if (sHaveLast) {
        const float dx = cx - sLastCx;
        const float dy = cy - sLastCy;
        if (dx * dx + dy * dy > 1.0f) motion = "move";
    }
    sLastCx = cx;
    sLastCy = cy;
    sHaveLast = true;

    Log("probe[%s] %s 228 i=%d f=%.6g | 22c i=%d f=%.6g | cur240=(%.2f,%.2f) vis64=(%.2f,%.2f) "
        "hit298=%d layer2A8=%u",
        tag, motion, i228, (double)f228, i22c, (double)f22c, (double)cx, (double)cy, (double)vx,
        (double)vy, hit, layer);
}

void ApplyInvuln(bool on) {
    // BIN 7ae984：InterStage/卸图禁写 SS+hit，避免迁频窗污染角色态；回 Field 再钉。
    if (!x::features::ports::world::IsPlayReady()) return;
    // LU teardown must not wipe SecondaryStat — SS is independent while in-map.
    if (gLocalUser && !LocalUserStillAlive()) {
        ClearLocalUser(on ? "dead before invuln write" : "dead on disable");
    }
    // soft 过图会 drop SS；急钉前先重绑，否则 n/r/t 空窗挨打（BIN 00:40 ss=0/0/0）。
    if (on && !SecondaryStatsAlive()) {
        gSecondaryStats.clear();
        TryResolveWorldManagers();
    }
    ApplySsOnly(on);
    if (!gLocalUser) return;
    ApplyHitGate(on);
    if (on) ApplyAntiBlink();
}

void LogReadback(const char* tag) {
    Log("%s lu=%p gate=hit hit298=%d layer2A8=%u ssN=%zu desired=%d alive=%d", tag, gLocalUser,
        gLocalUser ? ReadI32(gLocalUser, kOffHitPeriodRemain) : -1,
        gLocalUser ? ReadU32(gLocalUser, kOffLayerStateCounter) : 0u, gSecondaryStats.size(),
        gDesired.load() ? 1 : 0, LocalUserStillAlive() ? 1 : 0);
    for (size_t i = 0; i < gSecondaryStats.size() && i < 4; ++i) {
        void* ss = gSecondaryStats[i];
        Log("  ss[%zu]=%p n/r/t=%d/%d/%d dojang_n=%d", i, ss, ReadI32(ss, kOffNInv),
            ReadI32(ss, kOffRInv), ReadI32(ss, kOffTInv), ReadI32(ss, kOffNDojang));
    }
}

bool EnvWantsOn() {
    char buf[16]{};
    if (GetEnvironmentVariableA("XCAT_INVULN", buf, sizeof(buf)) > 0) {
        if (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T')
            return true;
    }
    return false;
}

void WarnIfSoftEnvRequested() {
    char buf[32]{};
    if (GetEnvironmentVariableA("XCAT_INVULN_GATE", buf, sizeof(buf)) == 0) return;
    if (_stricmp(buf, "soft") == 0 || _stricmp(buf, "228") == 0) {
        Log("XCAT_INVULN_GATE=%s ignored — soft +0x228 writes vanish avatar; using hit gate", buf);
    }
}

// FindAll only when unbound / dead. While StillAlive, reuse the cached binding.
void EnsureBindings() {
    if (!SecondaryStatsAlive()) {
        ClearSecondaryStats("ensure");
        TryResolveWorldManagers();
    }
    if (gLocalUser && WmMyUserDrifted()) ClearLocalUser("wm.MyUser drift");
    if (!LocalUserStillAlive()) {
        ClearLocalUser("ensure");
        TryResolveLocalUser();
    }
}

DWORD WINAPI InvulnThread(LPVOID) {
    Beep(740, 80);
    WarnIfSoftEnvRequested();
    Log("Invuln worker v2.6.8 start (hit=+0x270; anti-blink=frame+backup8ms; "
        "bind=wm.MyUser+FindAll; rebind=%ums/%ums grace=%ums; "
        "FindAll=PlayReady+TransitBlock; write=PlayReady-only; "
        "map_quiet=pre-InterStage-only; probe228 %s)",
        (unsigned)kRebindFastMs, (unsigned)kRebindMs, (unsigned)kBindGraceMs,
        ProbeEnabled() ? "on" : "off");

    for (int i = 0; i < 200 && !GetModuleHandleW(L"GameAssembly.dll") && !gWorkerStop.load(); ++i)
        Sleep(50);
    if (gWorkerStop.load()) {
        DisarmFrameBlink();
        return 0;
    }
    if (!BindApis()) {
        Beep(400, 300);
        DisarmFrameBlink();
        return 1;
    }

    TryArmFrameBlink("boot");

    if (EnvWantsOn()) {
        gDesired.store(true);
        Log("env XCAT_INVULN → desired=1");
    }

    // 冷启：勿再 Sleep(1500) — 落地空窗会挨打（BIN 00:28 ACCEPT 前空等）。
    // desired 可能稍后才从 PayloadControl 来；先绑，desired 到了 SetDesired/ACCEPT 急钉。
    EnsureBindings();
    if (gDesired.load() && x::features::ports::world::IsPlayReady() && gLocalUser) {
        ApplyInvuln(true);
        Log("boot pin hit298=%d", ReadI32(gLocalUser, kOffHitPeriodRemain));
    }
    if (ProbeEnabled() && gLocalUser) ProbeSoftSlots("boot");

    DWORD lastGate = 0;
    DWORD lastBlink = 0;
    DWORD lastFindAll = 0;
    DWORD lastPoll = 0;
    DWORD lastProbe = 0;
    DWORD lastPumpTry = 0;
    DWORD lastHb = GetTickCount();
    DWORD lastTransitLog = 0;
    bool wasPlayReady = x::features::ports::world::IsPlayReady();

    while (!gWorkerStop.load()) {
        const DWORD now = GetTickCount();
        const bool on = gDesired.load();
        const bool play = x::features::ports::world::IsPlayReady();

        if (now - lastPoll >= 200) {
            lastPoll = now;
            x::ipc::PayloadControl_Poll();
        }

        // 卸图/InterStage：卸帧钉、丢旧 LU/SS 缓存，本拍不写不重绑。
        if (on && !play) {
            if (gFrameBlink.load(std::memory_order_acquire)) {
                DisarmFrameBlink();
                if (!lastTransitLog || now - lastTransitLog > 2000) {
                    lastTransitLog = now;
                    Log("transit hold: disarm frame-tick + skip SS/hit writes scene=%d",
                        static_cast<int>(x::features::ports::world::GetSceneState()));
                }
            }
            if (gLocalUser) ClearLocalUser("transit !PlayReady");
            if (!gSecondaryStats.empty()) ClearSecondaryStats("transit !PlayReady");
            wasPlayReady = false;
            gPendingSoftLandResume = false;
            gSoftInvulnStaggerUntilMs = 0;
            gQuietTrackMapId = 0;  // 下一张图重新建 track
            // 注意：此处**不**清 gMapIdQuietUntilMs。
            // quiet 只服务「MapId 已闪仍 PlayReady」的进门脏窗（BIN 02:48）；
            // 回场急钉靠下面「resume 先于 quiet continue」恢复，勿在 !PlayReady 闪断时提前放开写闸。
            if (now - lastHb >= 5000) {
                lastHb = now;
                Log("heartbeat n=%lu desired=%d transit=1 blink=off lu=%p alive=0", gTickCount,
                    on ? 1 : 0, gLocalUser);
            }
            Sleep(kWorkerSleepOnMs);
            continue;
        }

        // 刚回 Field：必须先于 map_id quiet 的 continue。
        // soft hold：只记 pending，不急钉（LU 常空）。
        // soft land quiet / 已 play-ready：立刻 MyUser 急钉（用户要 quiet 期也有无敌）。
        if (on && play && !wasPlayReady) {
            gMapIdQuietUntilMs = 0;  // Field 已回，结束 map_id 静默
            lastGate = 0;
            const bool softHold = x::features::soft_login_probe::IsHoldActive();
            const bool landQuiet = x::features::soft_login_probe::IsLandQuiet();
            if (softHold) {
                gPendingSoftLandResume = true;
                gSoftInvulnStaggerUntilMs = 0;
                Log("transit resume soft_hold play-ready — light pin (no FindAll)");
                // reenter 等待窗：人已 play-ready 但 hold 未放 → 旧逻辑整段停写会挨打（BIN 00:39:56→58）。
                if (!SecondaryStatsAlive()) TryResolveWorldManagers();
                if (TryBindWmMyUser()) {
                    lastGate = now;
                    Log("land pin hit298=%d (soft_hold play-ready)",
                        ReadI32(gLocalUser, kOffHitPeriodRemain));
                } else {
                    Log("land pin defer: MyUser not ready yet (soft_hold)");
                }
            } else {
                gPendingSoftLandResume = landQuiet;  // quiet 结束再 arm 帧钉
                gSoftInvulnStaggerUntilMs = 0;
                gLandQuietUntilMs = now + kLandQuietMs;
                if (gLandQuietUntilMs == 0) gLandQuietUntilMs = 1;
                if (!landQuiet) TryArmFrameBlink("play_ready");
                Log("transit resume: play-ready → rebind+write (softLandQuiet=%d land_quiet=%ums)",
                    landQuiet ? 1 : 0, (unsigned)kLandQuietMs);
                if (TryBindWmMyUser()) {
                    ApplyInvuln(true);
                    lastGate = now;
                    Log("land pin hit298=%d (resume MyUser%s)",
                        ReadI32(gLocalUser, kOffHitPeriodRemain),
                        landQuiet ? " soft_land_quiet" : "");
                } else {
                    gPendingSoftLandResume = true;
                    Log("land pin defer: MyUser not ready yet");
                }
            }
        }
        wasPlayReady = play;

        // soft hold 结束 / land quiet 中：补 MyUser 急钉；quiet 结束后再 arm 帧钉。
        if (on && play && gPendingSoftLandResume &&
            !x::features::soft_login_probe::IsHoldActive()) {
            if (!LocalUserStillAlive() && TryBindWmMyUser()) {
                ApplyInvuln(true);
                lastGate = now;
                Log("land pin hit298=%d (soft_land_quiet MyUser)",
                    ReadI32(gLocalUser, kOffHitPeriodRemain));
            } else if (LocalUserStillAlive() &&
                       (!lastGate || now - lastGate >= kGateRefreshMs)) {
                ApplyInvuln(true);
                lastGate = now;
            }
            if (!x::features::soft_login_probe::IsLandQuiet()) {
                gPendingSoftLandResume = false;
                gSoftInvulnStaggerUntilMs = 0;
                gLandQuietUntilMs = now + kLandQuietMs;
                if (gLandQuietUntilMs == 0) gLandQuietUntilMs = 1;
                gMapIdQuietUntilMs = 0;
                lastGate = 0;
                TryArmFrameBlink("play_ready");
                Log("transit resume soft_land_quiet end → arm frame-tick");
                if (TryBindWmMyUser()) {
                    ApplyInvuln(true);
                    lastGate = now;
                    Log("land pin hit298=%d (post soft_land_quiet)",
                        ReadI32(gLocalUser, kOffHitPeriodRemain));
                }
            }
        }
        // MapId 闪变仍 PlayReady：立刻停写（手动/Travel 进门脏窗，BIN 02:48）。
        // 语义不变：只挡进门脏窗；真正回场由上面的 resume 清 quiet 并急钉。
        if (on && play) {
            const int mid = x::features::ports::world::GetMapId();
            if (mid > 0) {
                if (gQuietTrackMapId > 0 && mid != gQuietTrackMapId) {
                    gMapIdQuietUntilMs = now + kMapIdQuietMs;
                    if (gMapIdQuietUntilMs == 0) gMapIdQuietUntilMs = 1;
                    if (gFrameBlink.load(std::memory_order_acquire)) DisarmFrameBlink();
                    if (gLocalUser) ClearLocalUser("map_id change quiet");
                    if (!gSecondaryStats.empty()) ClearSecondaryStats("map_id change quiet");
                    Log("map_id quiet: %d->%d skip writes %ums (pre-InterStage dirty window)",
                        gQuietTrackMapId, mid, (unsigned)kMapIdQuietMs);
                }
                gQuietTrackMapId = mid;
            }
            if (InMapIdQuiet(now)) {
                static DWORD sLastMapQuietLog = 0;
                if (!sLastMapQuietLog || now - sLastMapQuietLog > 1500) {
                    sLastMapQuietLog = now;
                    Log("map_id quiet active remain=%ums mid=%d",
                        (unsigned)(gMapIdQuietUntilMs - now), mid);
                }
                Sleep(kWorkerSleepOnMs);
                continue;
            }
        }

        // soft hold：已 play-ready 则轻钉（禁 FindAll）；大厅未进图才整段停写。
        // BIN 00:39：hold 挡写到 RESULT 约 2s，落地空窗挨打。
        if (on && play && x::features::soft_login_probe::IsHoldActive()) {
            if (gFrameBlink.load(std::memory_order_acquire)) DisarmFrameBlink();
            if (!SecondaryStatsAlive()) TryResolveWorldManagers();
            if (!LocalUserStillAlive()) {
                if (TryBindWmMyUser()) {
                    lastGate = now;
                    static DWORD sLastHoldPin = 0;
                    if (!sLastHoldPin || now - sLastHoldPin > 1500) {
                        sLastHoldPin = now;
                        Log("soft_hold pin hit298=%d",
                            ReadI32(gLocalUser, kOffHitPeriodRemain));
                    }
                }
            } else if (!lastGate || now - lastGate >= kGateRefreshMs) {
                ApplyInvuln(true);
                lastGate = now;
            }
            if (now - lastBlink >= kAntiBlinkBackupMs) {
                lastBlink = now;
                ApplyAntiBlink();
            }
            Sleep(kWorkerSleepOnMs);
            continue;
        }
        if (on && play && x::features::soft_login_probe::IsLandQuiet()) {
            // 维持急钉（不 FindAll）；帧钉等 quiet 结束再 arm。
            if (gFrameBlink.load(std::memory_order_acquire)) DisarmFrameBlink();
            if (!SecondaryStatsAlive()) TryResolveWorldManagers();
            if (!LocalUserStillAlive()) {
                if (TryBindWmMyUser()) {
                    ApplyInvuln(true);
                    lastGate = now;
                    static DWORD sLastQuietPin = 0;
                    if (!sLastQuietPin || now - sLastQuietPin > 1500) {
                        sLastQuietPin = now;
                        Log("soft_land_quiet pin hit298=%d",
                            ReadI32(gLocalUser, kOffHitPeriodRemain));
                    }
                }
            } else if (!lastGate || now - lastGate >= kGateRefreshMs) {
                ApplyInvuln(true);
                lastGate = now;
            }
            if (now - lastBlink >= kAntiBlinkBackupMs) {
                lastBlink = now;
                ApplyAntiBlink();
            }
            Sleep(kWorkerSleepOnMs);
            continue;
        }

        if (on && play && !gFrameBlink.load() && now - lastPumpTry >= kPumpRetryMs) {
            lastPumpTry = now;
            TryArmFrameBlink("retry");
        }

        if (on && play) {
            // 换图：WM.MyUser 指针变了/清空 → 立刻丢旧绑，勿继续写死对象。
            if (gLocalUser && WmMyUserDrifted()) ClearLocalUser("wm.MyUser drift");

            const bool luOk = LocalUserStillAlive();
            const bool ssOk = SecondaryStatsAlive();
            if (!luOk || !ssOk) {
                if (!luOk && gLocalUser) ClearLocalUser("stillAlive false");
                if (!ssOk) ClearSecondaryStats("ss dead");

                if (!SecondaryStatsAlive()) TryResolveWorldManagers();

                // WM.MyUser：无绑时每 tick 试（不限流）—— scene=3 后通常同窗可 ACCEPT。
                // 非 IsPlayReady（InterStage/拍卖/商城）：禁 FindAll，防主线程堵泵。
                if (!LocalUserStillAlive()) {
                    if (TryBindWmMyUser()) {
                        ApplyInvuln(true);
                        lastGate = now;
                    } else if (!SkipFindAllTransit() &&
                               !(gLandQuietUntilMs &&
                                 static_cast<int>(now - gLandQuietUntilMs) < 0) &&
                               now - lastFindAll >= RebindIntervalMs()) {
                        lastFindAll = now;
                        if (TryResolveLocalUserFindAll()) {
                            ApplyInvuln(true);
                            lastGate = now;
                        }
                    } else if (gLandQuietUntilMs &&
                               static_cast<int>(now - gLandQuietUntilMs) < 0) {
                        static DWORD sLastQuietLog = 0;
                        if (!sLastQuietLog || now - sLastQuietLog > 1500) {
                            sLastQuietLog = now;
                            Log("land quiet: skip FindAll remain=%ums",
                                (unsigned)(gLandQuietUntilMs - now));
                        }
                    }
                }
            }
            if (now - lastGate >= kGateRefreshMs) {
                lastGate = now;
                ApplyInvuln(true);
            }
            // Always backup-pin while on (covers Update racing past a single frame tick).
            if (now - lastBlink >= kAntiBlinkBackupMs) {
                lastBlink = now;
                ApplyAntiBlink();
            }
            if ((++gTickCount % 200) == 0) LogReadback("refresh");
        }

        // C: read-only layout probe (no FindAll — reuse existing LocalUser bind)
        if (ProbeEnabled() && play && now - lastProbe >= kProbeMs) {
            lastProbe = now;
            if (gLocalUser && LocalUserStillAlive()) {
                ProbeSoftSlots(on ? "on" : "off");
            } else if (gLocalUser) {
                ClearLocalUser("probe stillAlive false");
            }
        }

        if (now - lastHb >= 5000) {
            lastHb = now;
            Log("heartbeat n=%lu desired=%d gate=hit blink=%s+8ms lu=%p hit298=%d alive=%d", gTickCount,
                on ? 1 : 0, gFrameBlink.load() ? "frame" : "worker", gLocalUser,
                gLocalUser ? ReadI32(gLocalUser, kOffHitPeriodRemain) : -1,
                LocalUserStillAlive() ? 1 : 0);
        }
        Sleep(on ? kWorkerSleepOnMs : kWorkerSleepOffMs);
    }
    if (gLocalUser && !gDesired.load()) ApplyInvuln(false);
    DisarmFrameBlink();
    Log("Invuln worker stop");
    return 0;
}

}  // namespace

void Init() {
    EnsureInvulnFieldOff();
    OpenLogs();
    Log("Invuln Init pid=%lu", GetCurrentProcessId());
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gWorkerThread.load() != nullptr) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, InvulnThread, nullptr, 0, nullptr);
    if (!th) {
        Log("CreateThread FAILED err=%lu", GetLastError());
        return;
    }
    gWorkerThread.store(th);
    Log("CreateThread ok");
}

void StopWorker() {
    gWorkerStop.store(true);
    DisarmFrameBlink();
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) CloseHandle(th);
}

void SetDesired(bool on) {
    const bool prev = gDesired.exchange(on);
    if (prev == on) return;
    Log("SetDesired %d", on ? 1 : 0);
    // InterStage：只记 desired；回 Field 由 worker transit resume 再绑再写。
    if (on && x::features::ports::world::IsPlayReady()) EnsureBindings();
    ApplyInvuln(on);
}

bool IsDesired() { return gDesired.load(); }
bool IsEnabled() { return gDesired.load() && LocalUserStillAlive(); }

}  // namespace invuln
}  // namespace features
}  // namespace x
