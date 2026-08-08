// Classic TWMS — soft login ConnectLogin try-connect probe (default OFF).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "soft_login_probe.h"

#include "../auto_enter/auto_enter.h"
#include "../galaxy_token_probe/galaxy_token_probe.h"
#include "../notify/notify.h"
#include "../ports/world_port.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace x::features::soft_login_probe {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr wchar_t kMarkerName[] = L"soft_login_probe.on";
constexpr wchar_t kLogDirDev[] = L"C:\\Users\\kras\\Desktop\\xcat_for_TWMS\\Dumps\\runtime";

// SceneLogin public void() — starts ConnectLoginServer IEnumerator via StartCoroutine.
constexpr uint32_t kRvaSceneLoginGet = 0xC02B40;
constexpr uint32_t kRvaConnectLoginStart = 0xC039D0;
constexpr char kHashSceneLoginGet[] =
    "f93d472ee14aaf9f1bfe05c7a5a5a2ce4e2942a8fdca404defb5d1a1637ef88";
constexpr char kHashConnectLoginStart[] =
    "df9624adb7823429c61a527eb2015ae2370ec841f8dafa9ec07c66edd732c9c";

// settle 用墙钟截止（见 Worker）：Call 耗时曾未计入 waited，实机 1500ms 常被拉成 2.5–3s+。
// Notice 多在断线瞬间弹出；Connecting 早退即可，不必死等满窗。
constexpr DWORD kSettleMs = 600;
constexpr DWORD kPollMs = 400;
constexpr int kPollRounds = 25;  // ~10s to Connected（invoke 已成功后）
constexpr DWORD kReenterPollMs = 350;
// 墙钟总预算（每轮含 dismiss/SamplePlayReady Call，旧「130×350≈45s」严重低估；dcaf08 卡死约 114s）。
// 成功样本 armed→playReady 约 10–37s；Done→playReady 可达 ~32s。
constexpr DWORD kReenterBudgetMs = 90000;
// Done(=left char)→play-ready 成功样本常 4–32s；满窗再 RequestRestart 并重置墙钟预算。
// 仅当泵采样确认仍 !inMap 时再启——已回图却因泵堵误判 !playReady 时再启会冻死主线程（E216）。
constexpr DWORD kDoneNoPlayRestartMs = 40000;
constexpr DWORD kDoneNoPlayFailMs = 40000;
// 单次 soft cycle 内 Done+!playReady 最多再拉几次选区（弱网多给机会，满额才 fail）。
constexpr int kDoneNoPlayMaxRestarts = 3;
// 同一断线边沿：ConnectLogin→重进 整轮可重试几次；仍失败才放 hold 交守护。
constexpr int kSoftCycleMax = 5;
constexpr DWORD kSoftCycleRetryGapMs = 800;
// 重进等待：playReady 采样宜短；泵堵时 1500ms 等满只会叠 job timeout（E216）。
constexpr DWORD kPlayReadySampleMs = 400;
// 泵心跳：与 MainPump::IsPumpTicking 默认 1500ms 对齐。BIN 02:23：软重连在 pump idle
// 上仍每 350ms SoftPumpCall → job timeout 螺旋；先等活再动，死透则交守护。
constexpr DWORD kPumpAliveMaxAgeMs = 1500;
constexpr DWORD kPumpWaitBeforeSoftMs = 8000;
constexpr int kReenterPumpFailMax = 16;       // 连续泵失败后 fail soft（约数秒～十余秒，视退避）
constexpr DWORD kReenterPumpFailSleepMinMs = 500;
constexpr DWORD kReenterPumpFailSleepMaxMs = 2500;
// connect-wait：InterStage 卡死时泵已死，再 SoftPumpCall 只会拖住 hold（BIN 02:30：20s×Call fail）。
constexpr int kConnectPumpFailMax = 8;
// 大厅期偶发关窗即可；进图后每轮 FindAll dismiss = 主线程死螺旋（E216 13:01:56～41）。
constexpr int kReenterHallDismissEvery = 6;  // ~2.1s @ 350ms，仅 !IsDone
// 进图后断线 SceneLogin 常晚于 settle 才重建；sl_null 时 hold 内重试，顺带吃游戏自连。
constexpr DWORD kConnectWaitMs = 250;
constexpr int kConnectWaitRounds = 80;  // ~20s 等 SL / Connecting / Connected
constexpr int kConnectHardFailGrace = 3;  // 非 sl_null 硬错也先重试几轮再放弃

constexpr int kStateDisconnecting = 0;
constexpr int kStateDisconnected = 1;
constexpr int kStateConnecting = 2;
constexpr int kStateConnected = 3;

// SceneLogin 登录 UI 槽（与 auto_enter / il2cpp_shape 同口径；软登录只读判定用）
constexpr size_t kOffSlChannelUi = 0xC0;
constexpr size_t kOffSlWorldUi = 0xC8;

// UIUtilDialog（非 Ex）— 与 worldmap_marker_travel 同源
constexpr char kUtilDialogClass[] =
    "b91dd9a7ee32ddf1538501f7a23119b0ad38634f3237d3dd148e6e986d70c69";
// UIDialog 基类（仅解析 Close；禁止 FindAll 基类——子树含 UIMiniMap 等 HUD）
constexpr char kUiDialogClass[] =
    "b386e7e275c5b13fd8250c343b276b1f5e8854ca7084cf2927620d34ecff375";
// UIMiniMap : UIDialog — 纵深防护（白名单路径本不应扫到）
constexpr char kMiniMapClass[] =
    "a1fd496912b2f43c1899ca58048196eeff7df9cbd3c9af0bb88ee57d89ce6c8";
// scanBase 白名单：断线/踢线 Notice 族（显式 FindAll 各类，永不扫 UIDialog 基类）
constexpr char kNoticeDialogClass[] =
    "b7a6a2bc4199c58a811194d6fc612b2bcc684255c179edacac286fc65bfcd33";
constexpr char kLoginUtilDialogClass[] =
    "da0d34cec7b10f6279cf26289e7b639b7cc86d345027040e15c9e6a56e71306";
constexpr char kSlideNoticeClass[] =
    "a3475df7d8de1269027d99e86fe21ce66d847134c3f6ad5a2cedb9443e17f91";
constexpr char kMultiLineNoticeClass[] =
    "a7beddf802ebba8f45f59c0e495e1175d084cf78b3f3d5a7fdf6b183202c1e4";
constexpr char kAntiMacroNoticeClass[] =
    "a1ed9cf37c8348c48b7e42018585f9e2a52d81d0ac86721244910465aa139fb";
// UIUtilDialogEx — 与 shop_port 同源
constexpr char kUtilDialogExClass[] =
    "f38993609fdcd5d4329046a4fea16805d838d5855315efe7fe2a8c5b05bc042";
// 官方关窗（CMS CloseDialog→UIDialog.Close）；不走 OnClickYes/Ok，避免踢线「確認」
constexpr uint32_t kRvaCloseDialog = 0x778CC0;   // UIUtilDialog.CloseDialog
constexpr uint32_t kRvaUiDialogClose = 0x117A290;  // UIDialog.Close（shop_port 同源）
constexpr char kHashCloseDialog[] =
    "de9ac0fd03b2844bad25ca20166c27d327514761da7bc4b9dca3ba858666441";
constexpr uint32_t kRvaCompGetGo = x::runtime::il2cpp::kRvaCompGetGo;
constexpr uint32_t kRvaGoSetActive = 0x4E5CAD0;
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E5CC70;
constexpr size_t kOffCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr
constexpr int kDismissMissRetries = 2;
constexpr DWORD kDismissMissGapMs = 80;
constexpr DWORD kDismissCallMs = 500;  // 关窗泵调用；实机通常 <50ms，勿再卡 3s
constexpr DWORD kEarlyDismissGapMs = 120;  // settle / connect-wait 内提前扫窗

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

using FnSceneLoginGet = void* (*)(const void* method);
using FnConnectLoginStart = void(__fastcall*)(void* self, const void* method);
using FnGetGameObject = void* (*)(void* self, const void* method);
using FnGoSetActive = void (*)(void* self, bool value, const void* method);
using FnGoGetActiveSelf = bool (*)(void* self, const void* method);
using FnObjectDestroy = void (*)(void* obj, const void* method);
using FnDialogClose = void (*)(void* self, const void* method);  // CloseDialog / UIDialog.Close

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};
std::atomic<bool> gPending{false};
std::atomic<bool> gBusy{false};
std::atomic<bool> gHold{false};
std::atomic<unsigned> gResult{0};  // 0 none 1 ok 2 fail
std::atomic<bool> gUiEnabled{false};
char gWhy[64]{};

struct PumpCtx {
    int ok = 0;
    char detail[160]{};
};

struct SampleCtx {
    int state = -1;
    int err = -1;
    int nmOk = 0;
    int slOk = 0;       // SceneLogin 实例可读
    int worldUi = 0;    // SL+0xC8 非空
    int channelUi = 0;  // SL+0xC0 非空
};

// InterStage quiesce（map-transit && !freeze）只放行 High；soft 系统短探对齐 channel_hop。
bool SoftPumpCall(x::runtime::main_thread::JobFn fn, void* user, DWORD timeoutMs) {
    return x::runtime::main_thread::InvokeAndWait(fn, user, timeoutMs,
                                                 x::runtime::main_thread::JobPrio::High);
}

// 已 Connected 且世界/频道选单已挂上：游戏已自回登录大厅，勿再 ConnectLogin。
bool LoginUiReady(const SampleCtx& s) {
    return s.nmOk != 0 && s.state == kStateConnected && (s.worldUi != 0 || s.channelUi != 0);
}

struct DismissCtx {
    // in: 1=强卸；Close 后必 SetActive；Destroy 仅仍可见；绝不点 Yes/Ok
    int aggressive = 0;
    // in: 1=额外扫断线 Notice 白名单（非 UIDialog 基类 FindAll）
    int scanBase = 0;
    int scanned = 0;
    int skippedDead = 0;
    int skippedInactive = 0;
    int closed = 0;       // CloseDialog / UIDialog.Close 成功次数
    int clickedOk = 0;    // 恒 0（禁止 OnClickYes/Ok）
    int inactivated = 0;
    int destroyed = 0;
    int skippedHud = 0;  // UIMiniMap 等图内 HUD（UIDialog 子类，禁止 Close）
    char detail[192]{};
};

struct PlayReadyCtx {
    int ready = 0;
    int inMap = 0;
    int sampled = 0;  // 1=泵上跑完；0=未进泵 / Assert 失败
};

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&DirExists), &self) ||
        !self)
        return {};
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring s(path, n);
    const size_t cut = s.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring() : s.substr(0, cut);
}

std::wstring ResolveLogDir() {
    std::wstring dir = DirExists(kLogDirDev) ? kLogDirDev : ModuleDir();
    if (!DirExists(kLogDirDev) && !dir.empty()) {
        const std::wstring logs = dir + L"\\logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        if (DirExists(logs)) dir = logs;
    }
    return dir;
}

bool EnvOn(const char* name) {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && buf[0] == '1';
}

bool MarkerArmed() {
    const std::wstring logDir = ResolveLogDir();
    const std::wstring modDir = ModuleDir();
    return (!logDir.empty() && FileExists(logDir + L"\\" + kMarkerName)) ||
           (!modDir.empty() && FileExists(modDir + L"\\" + kMarkerName));
}

void LogLine(const char* fmt, ...) {
    char body[1400]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char buf[1600]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const int n =
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds, body);
    if (n <= 0) return;
    const std::wstring dir = ResolveLogDir();
    if (!dir.empty())
        (void)x::runtime::AppendDbgLog(dir + L"\\soft_login.log", buf, static_cast<DWORD>(n));
    x::runtime::LogI("SoftLoginProbe", "%s", body);
}

void KickLogLine(const char* fmt, ...) {
    char body[900]{};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char buf[1100]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const int n =
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u [soft_login] %s\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    const std::wstring dir = ResolveLogDir();
    if (!dir.empty())
        (void)x::runtime::AppendDbgLog(dir + L"\\kick.log", buf, static_cast<DWORD>(n));
}

const char* StateName(int st) {
    switch (st) {
        case kStateDisconnecting:
            return "Disconnecting";
        case kStateDisconnected:
            return "Disconnected";
        case kStateConnecting:
            return "Connecting";
        case kStateConnected:
            return "Connected";
        default:
            return "?";
    }
}

MethodInfoHead* AsMi(void* raw) { return reinterpret_cast<MethodInfoHead*>(raw); }

int32_t ReadI32(void* p, size_t off) {
    if (!p) return -1;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* TryLazyValue(void* lazy) {
    if (!lazy || !LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classStaticData) {
        __try {
            void* p = e.classStaticData(klass);
            if (LooksLikeHeapPtr(p)) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

void* ResolveNmFacadeOnPump() {
    void* facade = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    if (!facade) return nullptr;
    void* staticsKlass = facade;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classParent) {
        __try {
            void* parent = e.classParent(facade);
            if (parent) staticsKlass = parent;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(facade);
    if (!statics) return nullptr;
    for (size_t s = 0; s < 8; ++s) {
        void* slot = ReadPtr(statics, s * sizeof(void*));
        if (!LooksLikeHeapPtr(slot)) continue;
        void* viaLazy = TryLazyValue(slot);
        if (viaLazy && LooksLikeHeapPtr(viaLazy)) return viaLazy;
        if (LooksLikeHeapPtr(slot)) return slot;
    }
    return nullptr;
}

void* PeekSceneLoginOnPump() {
    void* slKlass = x::runtime::il2cpp_shape::ResolveSceneLoginKlass();
    if (!slKlass) return nullptr;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    const MethodShape kGet{0, TypeKind::Ptr, true, true};
    auto getRes = x::runtime::il2cpp_method::FindMethodResolved(
        slKlass, kRvaSceneLoginGet, kGet, "get_Instance", kHashSceneLoginGet);
    MethodInfoHead* miGet = AsMi(getRes.method);
    if (!miGet || !miGet->methodPointer) return nullptr;
    void* sl = nullptr;
    __try {
        sl = reinterpret_cast<FnSceneLoginGet>(miGet->methodPointer)(miGet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sl = nullptr;
    }
    return (sl && LooksLikeHeapPtr(sl)) ? sl : nullptr;
}

void SampleNmOnPump(void* user) {
    auto* ctx = static_cast<SampleCtx*>(user);
    if (!ctx) return;
    ctx->state = -1;
    ctx->err = -1;
    ctx->nmOk = 0;
    ctx->slOk = 0;
    ctx->worldUi = 0;
    ctx->channelUi = 0;
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginSample")) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    x::runtime::il2cpp_network::Ensure();
    void* nm = ResolveNmFacadeOnPump();
    if (!nm || !LooksLikeHeapPtr(nm)) return;
    ctx->nmOk = 1;
    void* session = ReadPtr(nm, x::runtime::il2cpp_network::OffNmSession());
    if (session && !LooksLikeHeapPtr(session)) session = nullptr;
    if (session) {
        ctx->state = ReadI32(session, x::runtime::il2cpp_network::OffSessionState());
        ctx->err = ReadI32(session, x::runtime::il2cpp_network::OffSessionPendingError());
    } else {
        ctx->state = ReadI32(nm, x::runtime::il2cpp_network::OffNmSessionState());
        ctx->err = -1;
    }
    // 登录 UI：与 NM 同拍采样，供「已在世界页则跳过 ConnectLogin」。
    void* sl = PeekSceneLoginOnPump();
    if (!sl) return;
    ctx->slOk = 1;
    void* ch = ReadPtr(sl, kOffSlChannelUi);
    void* wu = ReadPtr(sl, kOffSlWorldUi);
    if (ch && LooksLikeHeapPtr(ch)) ctx->channelUi = 1;
    if (wu && LooksLikeHeapPtr(wu)) ctx->worldUi = 1;
}

void DoConnectOnPump(void* user) {
    auto* ctx = static_cast<PumpCtx*>(user);
    if (!ctx) return;
    ctx->ok = 0;
    ctx->detail[0] = '\0';
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginProbe")) {
        snprintf(ctx->detail, sizeof(ctx->detail), "not_on_pump");
        return;
    }
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(ctx->detail, sizeof(ctx->detail), "il2cpp");
        return;
    }

    void* slKlass = x::runtime::il2cpp_shape::ResolveSceneLoginKlass();
    if (!slKlass) {
        snprintf(ctx->detail, sizeof(ctx->detail), "sl_klass");
        return;
    }

    void* sl = PeekSceneLoginOnPump();
    if (!sl) {
        snprintf(ctx->detail, sizeof(ctx->detail), "sl_null");
        return;
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    const MethodShape kVoid0{0, TypeKind::Void, true, true};

    auto startRes = x::runtime::il2cpp_method::FindMethodResolved(
        slKlass, kRvaConnectLoginStart, kVoid0, nullptr, kHashConnectLoginStart);
    MethodInfoHead* miStart = AsMi(startRes.method);
    if (!miStart || !miStart->methodPointer) {
        snprintf(ctx->detail, sizeof(ctx->detail), "start_mi");
        return;
    }

    __try {
        reinterpret_cast<FnConnectLoginStart>(miStart->methodPointer)(sl, miStart);
        ctx->ok = 1;
        snprintf(ctx->detail, sizeof(ctx->detail), "sl=%p path=%s", sl,
                 x::runtime::il2cpp_method::PathName(startRes.path));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(ctx->detail, sizeof(ctx->detail), "exn");
    }
}

void* ClassTypeObjectOnPump(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetType || !e.typeGetObject) return nullptr;
    void* ty = nullptr;
    __try {
        ty = e.classGetType(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!ty) return nullptr;
    __try {
        return e.typeGetObject(ty);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool UnityAlive(void* obj) {
    if (!obj || !LooksLikeHeapPtr(obj)) return false;
    __try {
        return ReadPtr(obj, kOffCachedPtr) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// UIMiniMap 等常驻 HUD 也挂在 UIDialog 继承树上；基类 FindAll+Close 会把小地图卸掉。
bool IsProtectedFieldHud(void* dlg) {
    if (!dlg || !LooksLikeHeapPtr(dlg)) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.objectGetClass) return false;
    void* klass = nullptr;
    __try {
        klass = e.objectGetClass(dlg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!klass) return false;
    static void* sMiniKlass = nullptr;
    static bool sTried = false;
    if (!sTried) {
        sTried = true;
        sMiniKlass = x::runtime::il2cpp::FindClass("", kMiniMapClass);
        if (!sMiniKlass) sMiniKlass = x::runtime::il2cpp::FindClass("Msc.UI", "UIMiniMap");
        if (!sMiniKlass) sMiniKlass = x::runtime::il2cpp::FindClass("", "UIMiniMap");
    }
    return sMiniKlass && klass == sMiniKlass;
}

bool GoActiveSelf(void* go, MethodInfoHead* miGetActive) {
    if (!go || !miGetActive || !miGetActive->methodPointer) return true;
    bool active = true;
    __try {
        active = reinterpret_cast<FnGoGetActiveSelf>(miGetActive->methodPointer)(go, miGetActive);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active = true;
    }
    return active;
}

// activeSelf=false 时父节点仍可能撑着可见层；有 activeInHierarchy 则优先用它判「是否还在画」。
bool GoActiveInHierarchy(void* go, MethodInfoHead* miHier) {
    if (!go || !miHier || !miHier->methodPointer) return true;
    bool active = true;
    __try {
        active = reinterpret_cast<FnGoGetActiveSelf>(miHier->methodPointer)(go, miHier);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active = true;
    }
    return active;
}

void SetGoActive(void* go, bool on, MethodInfoHead* miSetActive) {
    if (!go || !LooksLikeHeapPtr(go) || !miSetActive || !miSetActive->methodPointer) return;
    __try {
        reinterpret_cast<FnGoSetActive>(miSetActive->methodPointer)(go, on, miSetActive);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 优先官方 CloseDialog（→ UIDialog.Close）；Close 后必 SetActive(false)（对齐 shop_port）。
// 绝不点 OnClickYes/Ok（踢线 YesNo 点確認=认踢；断线 Notice 的確認另议）。
void HideDialogVisual(void* dlg, MethodInfoHead* miCloseDialog, MethodInfoHead* miUiClose,
                      MethodInfoHead* miGetGo, MethodInfoHead* miSetActive,
                      MethodInfoHead* miGetActive, MethodInfoHead* miGetHier,
                      MethodInfoHead* miDestroy, bool forceInactive, DismissCtx* ctx) {
    if (!dlg || !ctx) return;
    if (IsProtectedFieldHud(dlg)) {
        ++ctx->skippedHud;
        return;
    }
    if (!UnityAlive(dlg)) {
        ++ctx->skippedDead;
        return;
    }

    void* go = nullptr;
    if (miGetGo && miGetGo->methodPointer) {
        __try {
            go = reinterpret_cast<FnGetGameObject>(miGetGo->methodPointer)(dlg, miGetGo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            go = nullptr;
        }
    }

    if (go && LooksLikeHeapPtr(go)) {
        const bool selfOn = GoActiveSelf(go, miGetActive);
        const bool hierOn = miGetHier ? GoActiveInHierarchy(go, miGetHier) : selfOn;
        // FindAll 含池/资源：起点已不可见则跳过（含激进）。935fae 是「Close 后误判已关
        // 而跳过 SetActive」；对起点仍可见的实例下面仍 Close+必 SetActive。
        if (!selfOn && !hierOn) {
            ++ctx->skippedInactive;
            return;
        }
    }

    ++ctx->scanned;

    bool closed = false;
    auto tryClose = [&](MethodInfoHead* mi) {
        if (closed || !mi || !mi->methodPointer) return;
        __try {
            reinterpret_cast<FnDialogClose>(mi->methodPointer)(dlg, mi);
            closed = true;
            ++ctx->closed;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    };
    tryClose(miCloseDialog);
    tryClose(miUiClose);

    // 935fae：close=2 后因 alreadyOff 跳过 SetActive → 断线「確認」窗仍在画。
    // shop_port：Close 后仍强 SetActive(false)（Close 常留残层）。此处同样必藏本 GO。
    // Destroy 仅在 SetActive 后仍 hier 可见时（避免对 FindAll 资源/池实例乱 Destroy→b2558a）。
    if (go && LooksLikeHeapPtr(go)) {
        SetGoActive(go, false, miSetActive);
        ++ctx->inactivated;
        if (forceInactive && miDestroy && miDestroy->methodPointer) {
            const bool selfOn = GoActiveSelf(go, miGetActive);
            const bool hierOn = miGetHier ? GoActiveInHierarchy(go, miGetHier) : selfOn;
            if (selfOn || hierOn) {
                __try {
                    reinterpret_cast<FnObjectDestroy>(miDestroy->methodPointer)(go, miDestroy);
                    ++ctx->destroyed;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }
}

void DismissDialogsOfKlass(void* klass, bool forceInactive, DismissCtx* ctx,
                           MethodInfoHead* miCloseDialog, MethodInfoHead* miUiClose,
                           MethodInfoHead* miGetGo, MethodInfoHead* miSetActive,
                           MethodInfoHead* miGetActive, MethodInfoHead* miGetHier,
                           MethodInfoHead* miDestroy) {
    if (!klass || !ctx) return;
    void* typeObj = ClassTypeObjectOnPump(klass);
    if (!typeObj) return;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.findAll) return;
    void* arr = nullptr;
    __try {
        arr = e.findAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        arr = nullptr;
    }
    if (!arr || !LooksLikeHeapPtr(arr)) return;

    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    int n = 0;
    __try {
        n = static_cast<int>(*reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + offLen));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    if (n <= 0) return;
    if (n > 16) n = 16;

    for (int i = 0; i < n; ++i) {
        void* o = nullptr;
        __try {
            o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                          static_cast<size_t>(i) * sizeof(void*));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            o = nullptr;
        }
        HideDialogVisual(o, miCloseDialog, miUiClose, miGetGo, miSetActive, miGetActive, miGetHier,
                         miDestroy, forceInactive, ctx);
    }
}

void* ResolveUiKlass(const char* hashName, const char* plainName) {
    void* k = nullptr;
    if (hashName && hashName[0]) k = x::runtime::il2cpp::FindClass("", hashName);
    if (!k && plainName && plainName[0]) {
        k = x::runtime::il2cpp::FindClass("Msc.UI", plainName);
        if (!k) k = x::runtime::il2cpp::FindClass("", plainName);
    }
    return k;
}

void DismissKickDialogOnPump(void* user) {
    auto* ctx = static_cast<DismissCtx*>(user);
    if (!ctx) return;
    const int aggressive = ctx->aggressive;
    int scanNotices = ctx->scanBase;
    *ctx = {};
    ctx->aggressive = aggressive;
    // 图内只关 Util/Ex；Notice 白名单也停（HUD 已上，断线窗应在进图前卸完）。
    if (scanNotices && x::features::ports::world::IsInMapScene()) {
        scanNotices = 0;
    }
    ctx->scanBase = scanNotices;
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginDismiss")) {
        snprintf(ctx->detail, sizeof(ctx->detail), "not_on_pump");
        return;
    }
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(ctx->detail, sizeof(ctx->detail), "il2cpp");
        return;
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    MethodInfoHead* miGetGo = nullptr;
    MethodInfoHead* miSetActive = nullptr;
    MethodInfoHead* miGetActive = nullptr;
    MethodInfoHead* miGetHier = nullptr;
    MethodInfoHead* miDestroy = nullptr;
    MethodInfoHead* miCloseDialog = nullptr;
    MethodInfoHead* miUiClose = nullptr;
    void* compKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    void* objKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Object");
    if (compKlass) {
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true};
        auto r = x::runtime::il2cpp_method::FindMethodResolved(compKlass, kRvaCompGetGo, kGo,
                                                               "get_gameObject", nullptr);
        miGetGo = AsMi(r.method);
    }
    if (goKlass) {
        // 与 shop_port 同形：unique=false（SetActive/set_active 撞车）+ walkParents + RVA。
        constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Bool}};
        auto r = x::runtime::il2cpp_method::FindMethodResolved(goKlass, kRvaGoSetActive, kSet,
                                                               "SetActive", nullptr);
        miSetActive = AsMi(r.method);
        constexpr MethodShape kAct{0, TypeKind::Bool, true, true, {}};
        auto ra = x::runtime::il2cpp_method::FindMethodResolved(goKlass, kRvaGoGetActiveSelf, kAct,
                                                                "get_activeSelf", nullptr);
        miGetActive = AsMi(ra.method);
        void* byHier =
            x::runtime::il2cpp_method::FindMethodByName(goKlass, "get_activeInHierarchy", 0, false);
        miGetHier = AsMi(byHier);
    }
    if (objKlass) {
        constexpr MethodShape kDes{1, TypeKind::Void, true, true, {TypeKind::Ptr}};
        auto rd =
            x::runtime::il2cpp_method::FindMethodResolved(objKlass, 0, kDes, "Destroy", nullptr);
        miDestroy = AsMi(rd.method);
        if (!miDestroy) {
            void* byName = x::runtime::il2cpp_method::FindMethodByName(objKlass, "Destroy", 1, true);
            miDestroy = AsMi(byName);
        }
    }

    void* util = ResolveUiKlass(kUtilDialogClass, "UIUtilDialog");
    void* utilEx = ResolveUiKlass(kUtilDialogExClass, "UIUtilDialogEx");
    void* uiDlg = ResolveUiKlass(kUiDialogClass, "UIDialog");

    constexpr MethodShape kClose0{0, TypeKind::Void, true, false};
    if (util) {
        auto r = x::runtime::il2cpp_method::FindMethodResolved(
            util, kRvaCloseDialog, kClose0, "CloseDialog", kHashCloseDialog);
        miCloseDialog = AsMi(r.method);
    }
    void* closeKlass = uiDlg ? uiDlg : util;
    if (closeKlass) {
        auto r = x::runtime::il2cpp_method::FindMethodResolved(closeKlass, kRvaUiDialogClose, kClose0,
                                                              "Close", nullptr);
        miUiClose = AsMi(r.method);
    }

    const bool force = aggressive != 0;
    MethodInfoHead* destroyMi = force ? miDestroy : nullptr;
    // 优雅路径：只扫 Util/Ex +（可选）Notice 白名单；永不 FindAll(UIDialog 基类)。
    // 旧 scanBase=基类 FindAll 会误关 UIMiniMap/任务闹钟/组队血条等 121 个子类中的 HUD。
    DismissDialogsOfKlass(utilEx, force, ctx, miCloseDialog, miUiClose, miGetGo, miSetActive,
                          miGetActive, miGetHier, destroyMi);
    DismissDialogsOfKlass(util, force, ctx, miCloseDialog, miUiClose, miGetGo, miSetActive,
                          miGetActive, miGetHier, destroyMi);
    int noticeHits = 0;
    if (scanNotices && force) {
        struct NoticeSpec {
            const char* hash;
            const char* plain;
        };
        static const NoticeSpec kNotices[] = {
            {kNoticeDialogClass, "UINoticeDialog"},
            {kLoginUtilDialogClass, "UILoginUtilDialog"},
            {kSlideNoticeClass, "UISlideNotice"},
            {kMultiLineNoticeClass, "UIMultiLineNotice"},
            {kAntiMacroNoticeClass, "UIAntiMacroNotice"},
            {nullptr, "UIMultiLine"},
        };
        for (const auto& spec : kNotices) {
            void* klass = ResolveUiKlass(spec.hash, spec.plain);
            if (!klass || klass == util || klass == utilEx) continue;
            const int before = ctx->scanned + ctx->closed + ctx->inactivated;
            DismissDialogsOfKlass(klass, true, ctx, miCloseDialog, miUiClose, miGetGo, miSetActive,
                                  miGetActive, miGetHier, destroyMi);
            if (ctx->scanned + ctx->closed + ctx->inactivated > before) ++noticeHits;
        }
    }

    snprintf(ctx->detail, sizeof(ctx->detail),
             "agg=%d notices=%d noticeKinds=%d scan=%d close=%d ok=0 inactive=%d destroy=%d "
             "dead=%d alreadyOff=%d skipHud=%d miClose=%d miUiClose=%d hier=%d",
             aggressive, scanNotices, noticeHits, ctx->scanned, ctx->closed, ctx->inactivated,
             ctx->destroyed, ctx->skippedDead, ctx->skippedInactive, ctx->skippedHud,
             miCloseDialog ? 1 : 0, miUiClose ? 1 : 0, miGetHier ? 1 : 0);
}

void SamplePlayReadyOnPump(void* user) {
    auto* ctx = static_cast<PlayReadyCtx*>(user);
    if (!ctx) return;
    ctx->ready = 0;
    ctx->inMap = 0;
    ctx->sampled = 0;
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginPlayReady")) return;
    // 仅在 Unity 泵上调（副作用 SetPumpPhase/Transit 与 publisher 同线程）。
    ctx->inMap = x::features::ports::world::IsInMapScene() ? 1 : 0;
    ctx->ready = x::features::ports::world::IsPlayReady() ? 1 : 0;
    ctx->sampled = 1;
}

void SetHold(bool on) { gHold.store(on, std::memory_order_release); }

void PublishSoftLoginNotify(unsigned result, const char* line) {
    using x::features::notify::NotificationEvent;
    using x::features::notify::NotificationKind;
    using x::features::notify::PublishNotification;

    char body[240]{};
    if (line && line[0]) {
        // 去掉 RESULT 前缀，气泡更短
        const char* p = line;
        if (std::strncmp(p, "RESULT success ", 15) == 0) p += 15;
        else if (std::strncmp(p, "RESULT fail ", 12) == 0) p += 12;
        snprintf(body, sizeof(body), "%s", p);
    } else if (gWhy[0]) {
        snprintf(body, sizeof(body), "why=%s", gWhy);
    }

    if (result == 1) {
        PublishNotification(NotificationEvent{NotificationKind::Success, "soft-login-ok",
                                              "软重连成功", body[0] ? body : "已重新进图", 6500});
    } else if (result == 2) {
        PublishNotification(NotificationEvent{NotificationKind::Warning, "soft-login-fail",
                                              "软重连失败",
                                              body[0] ? body : "将交由守护干净重拉", 8000});
    }
}

void Finish(unsigned result, const char* line) {
    gResult.store(result, std::memory_order_release);
    if (line) LogLine("%s", line);
    PublishSoftLoginNotify(result, line);
    // 成功/失败都解冻：失败若留 freeze=1，InterStage 不 quiesce、play 功能一直 defer，
    // 再叠 SoftPumpCall 会把已死泵打成 job timeout 螺旋（BIN 02:23）。
    x::runtime::managed_main::SetLoginFreeze(false);
    SetHold(false);
    gBusy.store(false);
}

// 等 Unity drain-host 心跳；超时返回 false（hold 仍由调用方决定）。
bool WaitPumpAlive(DWORD waitMs, const char* tag) {
    const DWORD deadline = GetTickCount() + waitMs;
    if (x::runtime::main_thread::IsPumpTicking(kPumpAliveMaxAgeMs)) return true;
    LogLine("pump_wait begin tag=%s max=%ums age=%ums", tag ? tag : "?",
            static_cast<unsigned>(waitMs),
            static_cast<unsigned>(x::runtime::main_thread::LastRealTickAgeMs()));
    KickLogLine("pump_wait begin tag=%s", tag ? tag : "?");
    while (!gStop.load()) {
        if (x::runtime::main_thread::IsPumpTicking(kPumpAliveMaxAgeMs)) {
            LogLine("pump_wait ok tag=%s", tag ? tag : "?");
            KickLogLine("pump_wait ok tag=%s", tag ? tag : "?");
            return true;
        }
        if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
        Sleep(200);
    }
    LogLine("pump_wait timeout tag=%s age=%ums", tag ? tag : "?",
            static_cast<unsigned>(x::runtime::main_thread::LastRealTickAgeMs()));
    KickLogLine("pump_wait timeout tag=%s", tag ? tag : "?");
    return false;
}

// 泵已死 / InterStage 卡死：再 soft cycle 只会空等 pump_wait（BIN 02:30 cycle2/3）。
// 立刻 Finish 放 hold，交守护干净重拉。
bool SoftFailIsFatalNoRetry(const char* failLine) {
    if (!failLine || !failLine[0]) return false;
    if (std::strstr(failLine, "pump_dead")) return true;
    if (std::strstr(failLine, "reenter_pump_dead")) return true;
    if (std::strstr(failLine, "reenter_pump_fail")) return true;
    if (std::strstr(failLine, "connect_wait_timeout") && std::strstr(failLine, "last=pump"))
        return true;
    if (std::strstr(failLine, "connect_wait_pump_dead")) return true;
    return false;
}

// 弱网可恢复失败：未用尽 soft cycle 则保持 hold/busy，回跳再跑 ConnectLogin→重进。
// true = 调用方 ++softCycle 后 goto soft_cycle_begin；false = 已 Finish(2) 或应停。
bool SoftFailOrRetry(int softCycle, const char* failLine) {
    if (!SoftFailIsFatalNoRetry(failLine) && softCycle < kSoftCycleMax && !gStop.load()) {
        // 勿把 retry 行写成 RESULT fail（避免统计虚高）；正式 fail 只走下方 Finish(2)。
        LogLine("soft_cycle_retry %d/%d after %ums | %s", softCycle, kSoftCycleMax,
                static_cast<unsigned>(kSoftCycleRetryGapMs), failLine ? failLine : "?");
        KickLogLine("soft_cycle_retry %d/%d", softCycle, kSoftCycleMax);
        Sleep(kSoftCycleRetryGapMs);
        return true;
    }
    if (SoftFailIsFatalNoRetry(failLine)) {
        LogLine("soft_fatal_no_retry | %s", failLine ? failLine : "?");
        KickLogLine("soft_fatal_no_retry");
    }
    Finish(2, failLine);
    return false;
}

bool DetailIsSlNull(const char* detail) {
    return detail && std::strncmp(detail, "sl_null", 7) == 0;
}

bool DetailIsTransientConnectMiss(const char* detail) {
    if (!detail || !detail[0]) return true;
    if (DetailIsSlNull(detail)) return true;
    if (std::strcmp(detail, "not_on_pump") == 0) return true;
    if (std::strcmp(detail, "il2cpp") == 0) return true;
    return false;
}

DWORD WINAPI Worker(LPVOID) {
    LogLine("worker start");
    while (!gStop.load()) {
        if (!gPending.exchange(false)) {
            Sleep(50);
            continue;
        }
        if (!IsArmed()) {
            LogLine("skip: not armed why=%s — clear hold", gWhy);
            SetHold(false);
            continue;
        }
        if (gBusy.exchange(true)) {
            // 已在试连中：保留 hold，pending 已消费；进行中的 attempt 会覆盖本轮。
            LogLine("skip: busy why=%s (hold kept)", gWhy);
            continue;
        }

        gResult.store(0, std::memory_order_release);
        SetHold(true);
        LogLine("attempt begin why=%s settle=%ums hold=1 softCycles=%d doneRestarts=%d", gWhy,
                static_cast<unsigned>(kSettleMs), kSoftCycleMax, kDoneNoPlayMaxRestarts);
        KickLogLine("attempt begin why=%s hold=1 softCycles=%d", gWhy, kSoftCycleMax);
        {
            char body[160]{};
            snprintf(body, sizeof(body), "why=%s · 推迟守护重拉", gWhy[0] ? gWhy : "disconnect");
            x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
                x::features::notify::NotificationKind::Info, "soft-login-try", "软重连试连中",
                body, 5000});
        }
        x::features::galaxy_token_probe::RequestSample("pre_soft_login");

        int softCycle = 1;
    soft_cycle_begin:
        LogLine("soft cycle begin %d/%d why=%s", softCycle, kSoftCycleMax, gWhy);
        KickLogLine("soft_cycle begin %d/%d", softCycle, kSoftCycleMax);
        // 上轮 RequestRestart 可能留下 freeze；周期开头先解，等真正重进再冻。
        x::runtime::managed_main::SetLoginFreeze(false);
        // 提前声明：后面 pump_dead 的 goto next 不得跳过带初始化的局部（MSVC C2362）。
        bool invokeOk = false;
        bool sawConnecting = false;
        int hardMiss = 0;
        int connectPumpFail = 0;
        char lastDetail[160] = "none";
        if (!WaitPumpAlive(kPumpWaitBeforeSoftMs, "soft_cycle")) {
            char fail[200]{};
            snprintf(fail, sizeof(fail),
                     "RESULT fail pump_dead before_settle why=%s soft_cycle=%d — release hold",
                     gWhy, softCycle);
            KickLogLine("RESULT fail pump_dead before_settle cycle=%d", softCycle);
            if (SoftFailOrRetry(softCycle, fail)) {
                ++softCycle;
                goto soft_cycle_begin;
            }
            goto next;
        }

        // 断线 Notice 往往立刻弹出；勿等 Connected 才关。
        // 墙钟截止：旧逻辑只把 Sleep 计入 waited，Call 一慢 settle 就拖成 2.5–3s+（302081）。
        // Connecting/Connected 早退：游戏已自连时不必耗满 settle。
        // scanBase=1：额外扫 Notice 白名单（UINoticeDialog 等）；永不 FindAll(UIDialog 基类)。
        {
            const DWORD settleDeadline = GetTickCount() + kSettleMs;
            bool settleEarlyNm = false;
            while (!gStop.load() && static_cast<int>(settleDeadline - GetTickCount()) > 0) {
                DismissCtx early{};
                early.aggressive = 1;
                early.scanBase = 1;
                if (SoftPumpCall(&DismissKickDialogOnPump, &early,
                                                   kDismissCallMs) &&
                    (early.closed > 0 || early.inactivated > 0)) {
                    LogLine("settle_dismiss %s", early.detail);
                    KickLogLine("settle_dismiss %s", early.detail);
                }
                const int remainBeforeSample = static_cast<int>(settleDeadline - GetTickCount());
                if (remainBeforeSample <= 0) break;
                // Sample 超时勿超过剩余墙钟，否则单轮 Call 仍能把 settle 拖过 deadline。
                const DWORD sampleMs =
                    remainBeforeSample < 800 ? static_cast<DWORD>(remainBeforeSample) : 800u;
                SampleCtx sample{};
                if (SoftPumpCall(&SampleNmOnPump, &sample, sampleMs) &&
                    sample.nmOk &&
                    (sample.state == kStateConnecting || sample.state == kStateConnected)) {
                    LogLine("settle early-exit nm=%s(%d) remain≈%dms", StateName(sample.state),
                            sample.state, static_cast<int>(settleDeadline - GetTickCount()));
                    KickLogLine("settle early_nm state=%s(%d)", StateName(sample.state),
                                sample.state);
                    settleEarlyNm = true;
                    break;
                }
                const DWORD now = GetTickCount();
                const int remain = static_cast<int>(settleDeadline - now);
                if (remain <= 0) break;
                const DWORD gap =
                    remain < static_cast<int>(kEarlyDismissGapMs) ? static_cast<DWORD>(remain)
                                                                  : kEarlyDismissGapMs;
                Sleep(gap);
            }
            if (settleEarlyNm) {
                // 已进入自连：再补一枪关窗，然后直接进 connect-wait。
                DismissCtx extra{};
                extra.aggressive = 1;
                extra.scanBase = 1;
                if (SoftPumpCall(&DismissKickDialogOnPump, &extra,
                                                   kDismissCallMs) &&
                    (extra.closed > 0 || extra.inactivated > 0)) {
                    LogLine("settle_dismiss %s", extra.detail);
                    KickLogLine("settle_dismiss %s", extra.detail);
                }
            }
        }
        if (gStop.load()) {
            Finish(2, "abort: stop during settle");
            break;
        }

        // 等 SceneLogin / 游戏自连：进图后断线常 sl_null，立刻 Finish 会放 hold → 守护杀进程。
        invokeOk = false;
        sawConnecting = false;
        hardMiss = 0;
        connectPumpFail = 0;
        strncpy_s(lastDetail, "none", _TRUNCATE);

        // ③ 已在世界/频道 UI 且 NM Connected：游戏自回大厅，跳过 ConnectLogin → dismiss+reenter。
        {
            SampleCtx peek{};
            if (SoftPumpCall(&SampleNmOnPump, &peek, 1500) && LoginUiReady(peek)) {
                LogLine("login_ui_ready post-settle world=%d ch=%d — skip ConnectLogin arm reenter",
                        peek.worldUi, peek.channelUi);
                KickLogLine("resume_login_ui skip_connect world=%d ch=%d where=post_settle",
                            peek.worldUi, peek.channelUi);
                invokeOk = true;
                goto connected_path;
            }
        }

        connectPumpFail = 0;
        for (int t = 0; t < kConnectWaitRounds && !gStop.load(); ++t) {
            // InterStage/load 泵死：禁止再 Sample/Dismiss/ConnectLogin（BIN 02:30 空打 ~20s）。
            if (!x::runtime::main_thread::IsPumpTicking(kPumpAliveMaxAgeMs)) {
                ++connectPumpFail;
                snprintf(lastDetail, sizeof(lastDetail), "pump");
                if ((connectPumpFail % 2) == 1) {
                    LogLine("connect-wait try=%d pump_idle age=%ums streak=%d — no Call", t,
                            static_cast<unsigned>(
                                x::runtime::main_thread::LastRealTickAgeMs()),
                            connectPumpFail);
                    KickLogLine("connect_wait pump_idle streak=%d", connectPumpFail);
                }
                if (connectPumpFail >= kConnectPumpFailMax) {
                    char fail[220]{};
                    snprintf(fail, sizeof(fail),
                             "RESULT fail connect_wait_pump_dead why=%s streak=%d try=%d — "
                             "release hold for guardian",
                             gWhy, connectPumpFail, t);
                    KickLogLine("RESULT fail connect_wait_pump_dead streak=%d", connectPumpFail);
                    if (SoftFailOrRetry(softCycle, fail)) {
                        ++softCycle;
                        goto soft_cycle_begin;
                    }
                    goto next;
                }
                Sleep(kConnectWaitMs + 200);
                continue;
            }

            SampleCtx sample{};
            if (SoftPumpCall(&SampleNmOnPump, &sample, 1500) && sample.nmOk) {
                connectPumpFail = 0;
                if (sample.state == kStateConnected) {
                    if (LoginUiReady(sample)) {
                        LogLine("login_ui_ready world=%d ch=%d try=%d — skip ConnectLogin",
                                sample.worldUi, sample.channelUi, t);
                        KickLogLine("resume_login_ui skip_connect world=%d ch=%d try=%d",
                                    sample.worldUi, sample.channelUi, t);
                    } else {
                        LogLine("NM already Connected during connect-wait try=%d world=%d ch=%d "
                                "sl=%d — skip ConnectLogin",
                                t, sample.worldUi, sample.channelUi, sample.slOk);
                        KickLogLine("connect_wait already Connected try=%d world=%d ch=%d", t,
                                    sample.worldUi, sample.channelUi);
                    }
                    invokeOk = true;
                    // 直接走 Connected 后续（dismiss+reenter），不再二次 ConnectLogin。
                    goto connected_path;
                }
                if (sample.state == kStateConnecting) {
                    if (!sawConnecting) {
                        sawConnecting = true;
                        LogLine("NM Connecting during connect-wait try=%d — hold, no re-invoke", t);
                        KickLogLine("connect_wait Connecting try=%d", t);
                    }
                    // Connecting 时仍可卸残留 Notice（不点 Yes/Ok）；基类一并扫。
                    {
                        DismissCtx pre{};
                        pre.aggressive = 1;
                        pre.scanBase = 1;
                        (void)SoftPumpCall(&DismissKickDialogOnPump, &pre,
                                                             kDismissCallMs);
                    }
                    Sleep(kConnectWaitMs);
                    continue;
                }
            }

            // 每轮激进关窗：断线 Notice 单钮「確認」≠踢线 YesNo；Close+SetActive 安全。
            {
                DismissCtx pre{};
                pre.aggressive = 1;
                pre.scanBase = 1;
                if (SoftPumpCall(&DismissKickDialogOnPump, &pre, kDismissCallMs) &&
                    (pre.scanned > 0 || pre.closed > 0 || t == 0)) {
                    LogLine("pre_dismiss try=%d %s", t, pre.detail);
                }
            }

            if (invokeOk) {
                // ConnectLogin 已触发，等 NM 变 Connecting/Connected（本循环顶部采样）。
                Sleep(kConnectWaitMs);
                continue;
            }

            PumpCtx ctx{};
            if (!SoftPumpCall(&DoConnectOnPump, &ctx, 3000)) {
                snprintf(lastDetail, sizeof(lastDetail), "pump");
                ++connectPumpFail;
                if ((t % 5) == 0 || connectPumpFail <= 2) {
                    LogLine("connect-wait try=%d Call fail/timeout streak=%d", t, connectPumpFail);
                    KickLogLine("connect_wait pump_fail try=%d streak=%d", t, connectPumpFail);
                }
                if (connectPumpFail >= kConnectPumpFailMax ||
                    !x::runtime::main_thread::IsPumpTicking(kPumpAliveMaxAgeMs)) {
                    if (connectPumpFail >= kConnectPumpFailMax) {
                        char fail[220]{};
                        snprintf(fail, sizeof(fail),
                                 "RESULT fail connect_wait_pump_dead why=%s streak=%d try=%d "
                                 "last=pump — release hold for guardian",
                                 gWhy, connectPumpFail, t);
                        KickLogLine("RESULT fail connect_wait_pump_dead streak=%d",
                                    connectPumpFail);
                        if (SoftFailOrRetry(softCycle, fail)) {
                            ++softCycle;
                            goto soft_cycle_begin;
                        }
                        goto next;
                    }
                }
                Sleep(kConnectWaitMs);
                continue;
            }
            connectPumpFail = 0;
            snprintf(lastDetail, sizeof(lastDetail), "%s", ctx.detail);
            if (ctx.ok) {
                invokeOk = true;
                LogLine("connect invoke ok=1 detail=%s why=%s try=%d", ctx.detail, gWhy, t);
                KickLogLine("connect ok=1 detail=%s try=%d", ctx.detail, t);
                // 进入下方 poll 等 Connected（与旧路径一致）
                break;
            }

            if (DetailIsTransientConnectMiss(ctx.detail)) {
                hardMiss = 0;
                if ((t % 5) == 0) {
                    LogLine("connect-wait try=%d detail=%s — retry while hold", t, ctx.detail);
                    KickLogLine("connect_wait retry detail=%s try=%d", ctx.detail, t);
                }
                Sleep(kConnectWaitMs);
                continue;
            }

            ++hardMiss;
            LogLine("connect-wait try=%d hard detail=%s miss=%d", t, ctx.detail, hardMiss);
            KickLogLine("connect_wait hard detail=%s try=%d", ctx.detail, t);
            if (hardMiss >= kConnectHardFailGrace) {
                char fail[192]{};
                snprintf(fail, sizeof(fail), "RESULT fail invoke detail=%s why=%s", ctx.detail,
                         gWhy);
                KickLogLine("RESULT fail detail=%s", ctx.detail);
                if (SoftFailOrRetry(softCycle, fail)) {
                    ++softCycle;
                    goto soft_cycle_begin;
                }
                goto next;
            }
            Sleep(kConnectWaitMs);
        }

        if (gStop.load()) {
            Finish(2, "abort: stop during connect-wait");
            goto next;
        }
        if (!invokeOk && !sawConnecting) {
            char fail[220]{};
            snprintf(fail, sizeof(fail),
                     "RESULT fail connect_wait_timeout last=%s why=%s — release hold for guardian",
                     lastDetail, gWhy);
            KickLogLine("RESULT fail connect_wait_timeout last=%s", lastDetail);
            if (SoftFailOrRetry(softCycle, fail)) {
                ++softCycle;
                goto soft_cycle_begin;
            }
            goto next;
        }
        if (!invokeOk && sawConnecting) {
            LogLine("connect-wait ended while NM Connecting — poll for Connected last=%s",
                    lastDetail);
            KickLogLine("connect_wait fallthrough Connecting");
        }

        {
        int best = -1;
        int lastErr = -1;
        bool sawNm = false;
        for (int i = 0; i < kPollRounds && !gStop.load(); ++i) {
            {
                DismissCtx mid{};
                mid.aggressive = 1;
                mid.scanBase = 1;
                if (SoftPumpCall(&DismissKickDialogOnPump, &mid, kDismissCallMs) &&
                    (mid.closed > 0 || mid.inactivated > 0)) {
                    LogLine("poll_dismiss[%d] %s", i, mid.detail);
                    KickLogLine("poll_dismiss[%d] %s", i, mid.detail);
                }
            }
            Sleep(kPollMs);
            SampleCtx sample{};
            if (!SoftPumpCall(&SampleNmOnPump, &sample, 1500)) {
                LogLine("poll[%d] sample_pump_fail", i);
                continue;
            }
            if (!sample.nmOk) {
                if (i == 0 || (i % 5) == 0) LogLine("poll[%d] nm=null", i);
                continue;
            }
            sawNm = true;
            if (sample.state != best || sample.err != lastErr) {
                LogLine("poll[%d] nm=1 state=%s(%d) err=%d", i, StateName(sample.state), sample.state,
                        sample.err);
                KickLogLine("poll[%d] state=%s(%d) err=%d", i, StateName(sample.state), sample.state,
                            sample.err);
                best = sample.state;
                lastErr = sample.err;
            }
            if (sample.state == kStateConnected) {
                KickLogLine("RESULT connected rounds=%d world=%d ch=%d — dismiss+reenter", i + 1,
                            sample.worldUi, sample.channelUi);
                if (LoginUiReady(sample)) {
                    LogLine("NM Connected+login_ui after soft path why=%s rounds=%d world=%d ch=%d",
                            gWhy, i + 1, sample.worldUi, sample.channelUi);
                } else {
                    LogLine("NM Connected after soft ConnectLogin why=%s rounds=%d world=%d ch=%d "
                            "sl=%d — dismiss dialogs",
                            gWhy, i + 1, sample.worldUi, sample.channelUi, sample.slOk);
                }
                goto connected_path;
            }
        }
        {
            char fail[220]{};
            snprintf(fail, sizeof(fail),
                     "RESULT fail final_state=%s(%d) err=%d nm_seen=%d why=%s", StateName(best),
                     best, lastErr, sawNm ? 1 : 0, gWhy);
            KickLogLine("RESULT fail state=%s(%d) err=%d", StateName(best), best, lastErr);
            if (SoftFailOrRetry(softCycle, fail)) {
                ++softCycle;
                goto soft_cycle_begin;
            }
        }
        goto next;
        }

    connected_path: {
                DismissCtx dismiss{};
                int dismissHits = 0;
                for (int d = 0; d < kDismissMissRetries && !gStop.load(); ++d) {
                    dismiss = {};
                    // 断线 Notice：Util/Ex + Notice 白名单；禁止 UIDialog 基类 FindAll。
                    dismiss.aggressive = 1;
                    dismiss.scanBase = 1;
                    if (!SoftPumpCall(&DismissKickDialogOnPump, &dismiss,
                                                        kDismissCallMs)) {
                        LogLine("dismiss Call fail/timeout try=%d", d);
                        KickLogLine("dismiss_fail reason=pump try=%d", d);
                    } else {
                        LogLine("dismiss try=%d %s", d, dismiss.detail);
                        KickLogLine("dismiss try=%d %s", d, dismiss.detail);
                    }
                    dismissHits += dismiss.scanned + dismiss.inactivated + dismiss.closed;
                    if (dismiss.closed > 0 || dismiss.inactivated > 0) break;
                    if (d + 1 < kDismissMissRetries) Sleep(kDismissMissGapMs);
                }
                if (gStop.load()) {
                    Finish(2, "abort: stop during dismiss");
                    goto next;
                }
                if (dismissHits <= 0) {
                    LogLine("dismiss_miss — no active UIUtilDialog; continue reenter anyway");
                    KickLogLine("dismiss_miss");
                }

                if (x::features::auto_enter::IsDesired()) {
                    x::features::auto_enter::RequestRestart("soft_login");
                    LogLine("auto_enter RequestRestart armed — wait play-ready up to %us wall",
                            static_cast<unsigned>(kReenterBudgetMs / 1000));
                    KickLogLine("reenter armed hold=1");

                    bool playOk = false;
                    bool earlyFail = false;
                    DWORD doneSinceMs = 0;
                    int doneRestartCount = 0;
                    bool sawInMapWhileDone = false;
                    int pumpFailStreak = 0;
                    DWORD budgetStartMs = GetTickCount();
                    DWORD budgetMs = kReenterBudgetMs;
                    for (int r = 0; !gStop.load(); ++r) {
                        if (GetTickCount() - budgetStartMs >= budgetMs) break;

                        // 泵已死：禁止再 SoftPumpCall / hall dismiss（只会叠 job timeout）。
                        if (!x::runtime::main_thread::IsPumpTicking(kPumpAliveMaxAgeMs)) {
                            ++pumpFailStreak;
                            DWORD sleepMs = kReenterPumpFailSleepMinMs +
                                            static_cast<DWORD>(pumpFailStreak) * 150u;
                            if (sleepMs > kReenterPumpFailSleepMaxMs)
                                sleepMs = kReenterPumpFailSleepMaxMs;
                            if ((pumpFailStreak % 4) == 1) {
                                LogLine("reenter wait[%d] pump_idle age=%ums streak=%d — no Call",
                                        r,
                                        static_cast<unsigned>(
                                            x::runtime::main_thread::LastRealTickAgeMs()),
                                        pumpFailStreak);
                                KickLogLine("reenter pump_idle streak=%d", pumpFailStreak);
                            }
                            if (pumpFailStreak >= kReenterPumpFailMax) {
                                char fail[220]{};
                                snprintf(fail, sizeof(fail),
                                         "RESULT fail reenter_pump_dead why=%s streak=%d "
                                         "round=%d — release hold for guardian",
                                         gWhy, pumpFailStreak, r + 1);
                                KickLogLine("RESULT fail reenter_pump_dead streak=%d",
                                            pumpFailStreak);
                                if (SoftFailOrRetry(softCycle, fail)) {
                                    ++softCycle;
                                    goto soft_cycle_begin;
                                }
                                earlyFail = true;
                                break;
                            }
                            Sleep(sleepMs);
                            if (gStop.load()) break;
                            continue;
                        }

                        // 大厅期偶发关窗；进图后禁止每轮 FindAll dismiss（E216 主线程死螺旋）。
                        const bool stillInHall = !x::features::auto_enter::IsDone() &&
                                                 !x::features::auto_enter::IsFailed();
                        if (stillInHall && (r % kReenterHallDismissEvery) == 0) {
                            DismissCtx again{};
                            again.aggressive = 1;
                            // 重进大厅只卸 Util/Ex；Notice 白名单已在 Connected 前扫过，勿再叠 FindAll。
                            again.scanBase = 0;
                            if (SoftPumpCall(&DismissKickDialogOnPump, &again,
                                                               kDismissCallMs) &&
                                (again.closed > 0 || again.inactivated > 0 || r == 0)) {
                                LogLine("dismiss hall %s", again.detail);
                                if (again.closed > 0 || again.inactivated > 0)
                                    KickLogLine("dismiss hall %s", again.detail);
                            }
                        }

                        if (x::features::auto_enter::IsFailed()) {
                            char fail[220]{};
                            snprintf(fail, sizeof(fail),
                                     "RESULT fail auto_enter Failed early why=%s reenter_round=%d",
                                     gWhy, r + 1);
                            KickLogLine("RESULT fail auto_enter_Failed round=%d", r + 1);
                            if (SoftFailOrRetry(softCycle, fail)) {
                                ++softCycle;
                                goto soft_cycle_begin;
                            }
                            earlyFail = true;
                            break;
                        }

                        PlayReadyCtx play{};
                        const bool sampleOk = SoftPumpCall(
                            &SamplePlayReadyOnPump, &play, kPlayReadySampleMs);
                        if (!sampleOk) {
                            // 泵堵：勿叠 dismiss、勿当 !playReady 去 Done 再启。
                            ++pumpFailStreak;
                            DWORD sleepMs = kReenterPumpFailSleepMinMs +
                                            static_cast<DWORD>(pumpFailStreak) * 100u;
                            if (sleepMs > kReenterPumpFailSleepMaxMs)
                                sleepMs = kReenterPumpFailSleepMaxMs;
                            if ((r % 10) == 0 || (pumpFailStreak % 4) == 1) {
                                LogLine("reenter wait[%d] playReady sample pump_fail streak=%d "
                                        "— backoff %ums",
                                        r, pumpFailStreak, static_cast<unsigned>(sleepMs));
                            }
                            if (pumpFailStreak >= kReenterPumpFailMax) {
                                char fail[220]{};
                                snprintf(fail, sizeof(fail),
                                         "RESULT fail reenter_pump_fail why=%s streak=%d "
                                         "round=%d — release hold for guardian",
                                         gWhy, pumpFailStreak, r + 1);
                                KickLogLine("RESULT fail reenter_pump_fail streak=%d",
                                            pumpFailStreak);
                                if (SoftFailOrRetry(softCycle, fail)) {
                                    ++softCycle;
                                    goto soft_cycle_begin;
                                }
                                earlyFail = true;
                                break;
                            }
                            Sleep(sleepMs);
                            if (gStop.load()) break;
                            continue;
                        }
                        pumpFailStreak = 0;
                        if (play.ready) {
                            playOk = true;
                            // 进图后禁止 FindAll(UIDialog) 收尾：UIMiniMap : UIDialog，Close 会弄丢小地图。
                            // Util/Ex 关踢线窗在大厅期已做过；图内残窗交给玩家/后续非基类路径。
                            if (play.inMap) {
                                LogLine("play-ready — skip post-dismiss (inMap; protect UIMiniMap)");
                                KickLogLine("play-ready skip_post_dismiss inMap=1");
                            } else {
                                LogLine("play-ready — light post-dismiss (inMap=%d)", play.inMap);
                                KickLogLine("play-ready post_dismiss_light");
                                DismissCtx post{};
                                post.aggressive = 1;
                                post.scanBase = 0;
                                if (SoftPumpCall(&DismissKickDialogOnPump, &post,
                                                                    kDismissCallMs)) {
                                    LogLine("post_dismiss %s", post.detail);
                                    if (post.closed > 0 || post.inactivated > 0)
                                        KickLogLine("post_dismiss %s", post.detail);
                                }
                            }
                            x::runtime::managed_main::SetLoginFreeze(false);
                            if (gStop.load()) {
                                Finish(2, "abort: stop during post_dismiss");
                                earlyFail = true;
                                break;
                            }
                            char ok[220]{};
                            snprintf(ok, sizeof(ok),
                                     "RESULT success play-ready after soft reenter why=%s "
                                     "reenter_rounds=%d soft_cycle=%d/%d",
                                     gWhy, r + 1, softCycle, kSoftCycleMax);
                            KickLogLine("RESULT success play-ready rounds=%d soft_cycle=%d", r + 1,
                                        softCycle);
                            Finish(1, ok);
                            break;
                        }

                        // Done 却迟迟不进图：仅确认仍在大厅 (!inMap) 才 RequestRestart。
                        // 已 inMap 而 !ready / 泵曾堵：再启会冻主线程（E216）。
                        if (x::features::auto_enter::IsDone()) {
                            const DWORD now = GetTickCount();
                            if (play.sampled && play.inMap) {
                                if (!sawInMapWhileDone) {
                                    sawInMapWhileDone = true;
                                    LogLine("reenter: Done+inMap playReady=0 — wait alive, "
                                            "no RequestRestart");
                                    KickLogLine("reenter done_in_map wait_alive");
                                }
                                doneSinceMs = 0;  // 不走 Done 再启时钟
                            } else if (play.sampled && !play.inMap) {
                                if (!doneSinceMs) {
                                    doneSinceMs = now;
                                    LogLine("reenter: auto_enter Done but still !inMap — watch "
                                            "%ums (restarts %d/%d)",
                                            static_cast<unsigned>(kDoneNoPlayRestartMs),
                                            doneRestartCount, kDoneNoPlayMaxRestarts);
                                } else if (doneRestartCount < kDoneNoPlayMaxRestarts &&
                                           now - doneSinceMs >= kDoneNoPlayRestartMs) {
                                    ++doneRestartCount;
                                    LogLine("reenter: Done+!inMap %ums — RequestRestart %d/%d",
                                            static_cast<unsigned>(now - doneSinceMs),
                                            doneRestartCount, kDoneNoPlayMaxRestarts);
                                    KickLogLine("reenter done_no_play restart %d/%d",
                                                doneRestartCount, kDoneNoPlayMaxRestarts);
                                    x::features::auto_enter::RequestRestart("soft_login");
                                    doneSinceMs = now;
                                    sawInMapWhileDone = false;
                                    budgetStartMs = now;
                                    budgetMs = kReenterBudgetMs;
                                } else if (doneRestartCount >= kDoneNoPlayMaxRestarts &&
                                           now - doneSinceMs >= kDoneNoPlayFailMs) {
                                    char fail[240]{};
                                    snprintf(fail, sizeof(fail),
                                             "RESULT fail done_no_play why=%s reenter_round=%d "
                                             "restarted=%d — release hold for guardian",
                                             gWhy, r + 1, doneRestartCount);
                                    KickLogLine("RESULT fail done_no_play round=%d restarted=%d",
                                                r + 1, doneRestartCount);
                                    if (SoftFailOrRetry(softCycle, fail)) {
                                        ++softCycle;
                                        goto soft_cycle_begin;
                                    }
                                    earlyFail = true;
                                    break;
                                }
                            }
                        } else {
                            doneSinceMs = 0;
                            sawInMapWhileDone = false;
                        }

                        if ((r % 10) == 0) {
                            LogLine("reenter wait[%d] playReady=0 inMap=%d wall=%ums/%ums "
                                    "soft_cycle=%d done_restarts=%d",
                                    r, play.inMap,
                                    static_cast<unsigned>(GetTickCount() - budgetStartMs),
                                    static_cast<unsigned>(budgetMs), softCycle, doneRestartCount);
                        }
                        Sleep(kReenterPollMs);
                        if (gStop.load()) break;
                    }
                    if (earlyFail) {
                        goto next;
                    }
                    if (gStop.load()) {
                        Finish(2, "abort: stop during reenter");
                        goto next;
                    }
                    if (!playOk) {
                        // 已 Done+inMap：人在图内，勿再 soft cycle ConnectLogin（会重引入 E216 泵堵）。
                        // IsPlayReady 假/泵抖 → 降级成功解 hold，交回挂机。
                        if (sawInMapWhileDone) {
                            LogLine("reenter_timeout Done+inMap playReady=0 — degrade success "
                                    "(no soft_cycle ConnectLogin) why=%s soft_cycle=%d/%d",
                                    gWhy, softCycle, kSoftCycleMax);
                            KickLogLine("RESULT success degrade_in_map_timeout soft_cycle=%d",
                                        softCycle);
                            x::runtime::managed_main::SetLoginFreeze(false);
                            // 已 inMap：禁止基类 UIDialog FindAll 收尾（同 play-ready 路径）。
                            LogLine("degrade — skip post-dismiss (Done+inMap; protect UIMiniMap)");
                            KickLogLine("degrade skip_post_dismiss inMap=1");
                            char ok[240]{};
                            snprintf(ok, sizeof(ok),
                                     "RESULT success degrade Done+inMap timeout playReady=0 "
                                     "why=%s soft_cycle=%d/%d",
                                     gWhy, softCycle, kSoftCycleMax);
                            Finish(1, ok);
                        } else {
                            char fail[220]{};
                            snprintf(fail, sizeof(fail),
                                     "RESULT fail reenter_timeout playReady=0 why=%s "
                                     "soft_cycle=%d/%d — release hold for guardian relaunch",
                                     gWhy, softCycle, kSoftCycleMax);
                            KickLogLine("RESULT fail reenter_timeout soft_cycle=%d", softCycle);
                            if (SoftFailOrRetry(softCycle, fail)) {
                                ++softCycle;
                                goto soft_cycle_begin;
                            }
                        }
                    }
                } else {
                    char ok[192]{};
                    snprintf(ok, sizeof(ok),
                             "RESULT success Connected (autoEnter off) why=%s", gWhy);
                    KickLogLine("RESULT success Connected autoEnter=0");
                    Finish(1, ok);
                }
                goto next;
    }
    next:;
    }
    SetHold(false);
    LogLine("worker stop");
    return 0;
}

}  // namespace

void Init() {
    if (IsArmed()) {
        LogLine("armed — will try SceneLogin ConnectLogin after disconnect (hold defers guardian)");
        KickLogLine("armed");
    } else {
        LogLine("idle (home 「软重连试连」 / soft_login_probe.on / SOFT_LOGIN_PROBE=1)");
    }
}

void Shutdown() { StopWorker(); }

void SetEnabled(bool on) {
    const bool prev = gUiEnabled.exchange(on);
    if (prev == on) return;
    if (on) {
        LogLine("UI enable — try ConnectLogin after disconnect (hold defers guardian)");
        KickLogLine("armed ui");
    } else if (!MarkerArmed() && !EnvOn("SOFT_LOGIN_PROBE")) {
        LogLine("UI disable — idle (marker/env still override if present)");
    }
}

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        LogLine("CreateThread fail");
        return;
    }
    gThread.store(th);
    LogLine("CreateThread ok");
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 8000);
        CloseHandle(th);
    }
    SetHold(false);
}

bool IsArmed() {
    return gUiEnabled.load(std::memory_order_acquire) || MarkerArmed() ||
           EnvOn("SOFT_LOGIN_PROBE");
}

bool IsHoldActive() { return gHold.load(std::memory_order_acquire); }

unsigned ResultCode() { return gResult.load(std::memory_order_acquire); }

void RequestAttempt(const char* why) {
    if (!IsArmed()) return;
    memset(gWhy, 0, sizeof(gWhy));
    strncpy_s(gWhy, why ? why : "disconnect", _TRUNCATE);
    // 必须在 kick_sniff 抬 disconnectSeq / 宿主读 status 之前同步 hold。
    // 若只靠 worker 50ms 轮询再 SetHold，守护会先看到 seq 上涨且 hold=0 → 立刻干净重拉
    // （upload 9fee22：10:38:37 disconnect → 10:38:39 kill，soft_login 无 attempt begin）。
    SetHold(true);
    gPending.store(true);
    LogLine("request why=%s hold=1 (sync before worker)", gWhy);
    KickLogLine("request why=%s hold=1", gWhy);
}

void RequestManualDismiss() {
    LogLine("manual dismiss — CloseDialog+SetActive (no Yes/Ok)");
    KickLogLine("manual dismiss begin");
    DismissCtx ctx{};
    ctx.aggressive = 1;
    ctx.scanBase = 1;
    if (!SoftPumpCall(&DismissKickDialogOnPump, &ctx, kDismissCallMs)) {
        LogLine("manual dismiss Call fail/timeout");
        KickLogLine("manual dismiss fail pump");
        x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
            x::features::notify::NotificationKind::Warning, "soft-dismiss-manual", "关断线弹窗失败",
            "泵调用超时/失败 · 确认已注入且在游戏内", 5000});
        return;
    }
    LogLine("manual dismiss %s", ctx.detail);
    KickLogLine("manual dismiss %s", ctx.detail);
    char body[192]{};
    snprintf(body, sizeof(body), "scan=%d close=%d inactive=%d destroy=%d", ctx.scanned, ctx.closed,
             ctx.inactivated, ctx.destroyed);
    const bool hit = ctx.scanned > 0 || ctx.closed > 0 || ctx.inactivated > 0;
    x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
        hit ? x::features::notify::NotificationKind::Success
            : x::features::notify::NotificationKind::Info,
        "soft-dismiss-manual", hit ? "已尝试关闭断线弹窗" : "未扫到活动弹窗", body, 4500});
}

}  // namespace x::features::soft_login_probe
