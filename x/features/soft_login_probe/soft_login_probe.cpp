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

constexpr DWORD kSettleMs = 1500;
constexpr DWORD kPollMs = 400;
constexpr int kPollRounds = 25;  // ~10s to Connected（invoke 已成功后）
constexpr DWORD kReenterPollMs = 500;
constexpr int kReenterRounds = 90;  // ~45s auto_enter → play-ready；超时则 fail → 守护重拉
// 进图后断线 SceneLogin 常晚于 settle 才重建；sl_null 时 hold 内重试，顺带吃游戏自连。
constexpr DWORD kConnectWaitMs = 400;
constexpr int kConnectWaitRounds = 50;  // ~20s 等 SL / Connecting / Connected
constexpr int kConnectHardFailGrace = 3;  // 非 sl_null 硬错也先重试几轮再放弃

constexpr int kStateDisconnecting = 0;
constexpr int kStateDisconnected = 1;
constexpr int kStateConnecting = 2;
constexpr int kStateConnected = 3;

// UIUtilDialog（非 Ex）— 与 worldmap_marker_travel 同源
constexpr char kUtilDialogClass[] =
    "b91dd9a7ee32ddf1538501f7a23119b0ad38634f3237d3dd148e6e986d70c69";
// UIUtilDialogEx — 与 shop_port 同源
constexpr char kUtilDialogExClass[] =
    "f38993609fdcd5d4329046a4fea16805d838d5855315efe7fe2a8c5b05bc042";
constexpr uint32_t kRvaCompGetGo = x::runtime::il2cpp::kRvaCompGetGo;
constexpr uint32_t kRvaGoSetActive = 0x4E5CAD0;
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E5CC70;
constexpr size_t kOffCachedPtr = 0x10;  // UnityEngine.Object.m_CachedPtr
constexpr int kDismissMissRetries = 3;
constexpr DWORD kDismissMissGapMs = 350;
// 进图后断线 Notice 常晚于 Connected 出现；play-ready 后再爆发关窗。
constexpr int kPostReadyDismissTries = 8;
constexpr DWORD kPostReadyDismissGapMs = 250;

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
};

struct DismissCtx {
    // in: 1=进图后强卸（含 alreadyOff 僵尸；本 GO SetActive + Destroy；绝不点 OK/Close/卸父）
    int aggressive = 0;
    int scanned = 0;
    int skippedDead = 0;
    int skippedInactive = 0;
    int closed = 0;      // 兼容日志；恒 0
    int clickedOk = 0;   // 恒 0
    int inactivated = 0;
    int destroyed = 0;
    char detail[192]{};
};

struct PlayReadyCtx {
    int ready = 0;
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

void SampleNmOnPump(void* user) {
    auto* ctx = static_cast<SampleCtx*>(user);
    if (!ctx) return;
    ctx->state = -1;
    ctx->err = -1;
    ctx->nmOk = 0;
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

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    const MethodShape kGet{0, TypeKind::Ptr, true, true};
    const MethodShape kVoid0{0, TypeKind::Void, true, true};

    auto getRes = x::runtime::il2cpp_method::FindMethodResolved(
        slKlass, kRvaSceneLoginGet, kGet, "get_Instance", kHashSceneLoginGet);
    MethodInfoHead* miGet = AsMi(getRes.method);
    if (!miGet || !miGet->methodPointer) {
        snprintf(ctx->detail, sizeof(ctx->detail), "get_mi");
        return;
    }

    void* sl = nullptr;
    __try {
        sl = reinterpret_cast<FnSceneLoginGet>(miGet->methodPointer)(miGet);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sl = nullptr;
    }
    if (!sl || !LooksLikeHeapPtr(sl)) {
        snprintf(ctx->detail, sizeof(ctx->detail), "sl_null");
        return;
    }

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

void SetGoActive(void* go, bool on, MethodInfoHead* miSetActive) {
    if (!go || !LooksLikeHeapPtr(go) || !miSetActive || !miSetActive->methodPointer) return;
    __try {
        reinterpret_cast<FnGoSetActive>(miSetActive->methodPointer)(go, on, miSetActive);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 只卸本弹窗 GO：SetActive(false) +（激进时）Destroy。不点 OK/Close，不向上卸父（防误关 HUD）。
void HideDialogVisual(void* dlg, MethodInfoHead* miGetGo, MethodInfoHead* miSetActive,
                      MethodInfoHead* miGetActive, MethodInfoHead* miDestroy, bool forceInactive,
                      DismissCtx* ctx) {
    if (!dlg || !ctx) return;
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
    if (!go || !LooksLikeHeapPtr(go)) {
        ++ctx->skippedDead;
        return;
    }

    const bool inactive = !GoActiveSelf(go, miGetActive);
    if (inactive && !forceInactive) {
        ++ctx->skippedInactive;
        return;
    }
    if (inactive) ++ctx->skippedInactive;

    ++ctx->scanned;

    SetGoActive(go, false, miSetActive);
    ++ctx->inactivated;

    if (forceInactive && miDestroy && miDestroy->methodPointer) {
        __try {
            reinterpret_cast<FnObjectDestroy>(miDestroy->methodPointer)(go, miDestroy);
            ++ctx->destroyed;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

void DismissDialogsOfKlass(void* klass, bool forceInactive, DismissCtx* ctx,
                           MethodInfoHead* miGetGo, MethodInfoHead* miSetActive,
                           MethodInfoHead* miGetActive, MethodInfoHead* miDestroy) {
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
        HideDialogVisual(o, miGetGo, miSetActive, miGetActive, miDestroy, forceInactive, ctx);
    }
}

void DismissKickDialogOnPump(void* user) {
    auto* ctx = static_cast<DismissCtx*>(user);
    if (!ctx) return;
    const int aggressive = ctx->aggressive;
    *ctx = {};
    ctx->aggressive = aggressive;
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
    MethodInfoHead* miDestroy = nullptr;
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
        constexpr MethodShape kSet{1, TypeKind::Void, true, false, {TypeKind::Bool}};
        auto r = x::runtime::il2cpp_method::FindMethodResolved(goKlass, kRvaGoSetActive, kSet,
                                                               "SetActive", nullptr);
        miSetActive = AsMi(r.method);
        constexpr MethodShape kAct{0, TypeKind::Bool, true, false};
        auto ra = x::runtime::il2cpp_method::FindMethodResolved(goKlass, kRvaGoGetActiveSelf, kAct,
                                                                "get_activeSelf", nullptr);
        miGetActive = AsMi(ra.method);
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

    void* util = x::runtime::il2cpp::FindClass("", kUtilDialogClass);
    if (!util) util = x::runtime::il2cpp::FindClass("Msc.UI", "UIUtilDialog");
    void* utilEx = x::runtime::il2cpp::FindClass("", kUtilDialogExClass);
    if (!utilEx) utilEx = x::runtime::il2cpp::FindClass("Msc.UI", "UIUtilDialogEx");

    const bool force = aggressive != 0;
    MethodInfoHead* destroyMi = force ? miDestroy : nullptr;
    DismissDialogsOfKlass(utilEx, force, ctx, miGetGo, miSetActive, miGetActive, destroyMi);
    DismissDialogsOfKlass(util, force, ctx, miGetGo, miSetActive, miGetActive, destroyMi);
    if (force) {
        void* uiDlg = x::runtime::il2cpp::FindClass("Msc.UI", "UIDialog");
        if (!uiDlg) uiDlg = x::runtime::il2cpp::FindClass("", "UIDialog");
        if (uiDlg && uiDlg != util && uiDlg != utilEx) {
            DismissDialogsOfKlass(uiDlg, true, ctx, miGetGo, miSetActive, miGetActive, miDestroy);
        }
    }

    snprintf(ctx->detail, sizeof(ctx->detail),
             "agg=%d scan=%d close=0 ok=0 inactive=%d destroy=%d dead=%d alreadyOff=%d", aggressive,
             ctx->scanned, ctx->inactivated, ctx->destroyed, ctx->skippedDead, ctx->skippedInactive);
}

void SamplePlayReadyOnPump(void* user) {
    auto* ctx = static_cast<PlayReadyCtx*>(user);
    if (!ctx) return;
    ctx->ready = 0;
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginPlayReady")) return;
    // 仅在 Unity 泵上调 IsPlayReady（副作用 SetPumpPhase/Transit 与 publisher 同线程）。
    ctx->ready = x::features::ports::world::IsPlayReady() ? 1 : 0;
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
    SetHold(false);
    gBusy.store(false);
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
            LogLine("skip: not armed why=%s", gWhy);
            continue;
        }
        if (gBusy.exchange(true)) {
            LogLine("skip: busy why=%s", gWhy);
            continue;
        }

        gResult.store(0, std::memory_order_release);
        SetHold(true);
        LogLine("attempt begin why=%s settle=%ums hold=1", gWhy, static_cast<unsigned>(kSettleMs));
        KickLogLine("attempt begin why=%s hold=1", gWhy);
        {
            char body[160]{};
            snprintf(body, sizeof(body), "why=%s · 推迟守护重拉", gWhy[0] ? gWhy : "disconnect");
            x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
                x::features::notify::NotificationKind::Info, "soft-login-try", "软重连试连中",
                body, 5000});
        }
        x::features::galaxy_token_probe::RequestSample("pre_soft_login");

        for (DWORD waited = 0; waited < kSettleMs && !gStop.load(); waited += 50) Sleep(50);
        if (gStop.load()) {
            Finish(2, "abort: stop during settle");
            break;
        }

        // 等 SceneLogin / 游戏自连：进图后断线常 sl_null，立刻 Finish 会放 hold → 守护杀进程。
        bool invokeOk = false;
        bool sawConnecting = false;
        int hardMiss = 0;
        char lastDetail[160] = "none";
        for (int t = 0; t < kConnectWaitRounds && !gStop.load(); ++t) {
            SampleCtx sample{};
            if (x::runtime::managed_main::Call(&SampleNmOnPump, &sample, 1500) && sample.nmOk) {
                if (sample.state == kStateConnected) {
                    LogLine("NM already Connected during connect-wait try=%d — skip/after invoke",
                            t);
                    KickLogLine("connect_wait already Connected try=%d", t);
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
                    Sleep(kConnectWaitMs);
                    continue;
                }
            }

            // 卸挡用非激进：此时可能仍有踢线 YesNo，Close≈取消退出。
            if ((t % 2) == 0) {
                DismissCtx pre{};
                pre.aggressive = 0;
                if (x::runtime::managed_main::Call(&DismissKickDialogOnPump, &pre, 2000) &&
                    (pre.scanned > 0 || t == 0)) {
                    LogLine("pre_dismiss try=%d %s", t, pre.detail);
                }
            }

            if (invokeOk) {
                // ConnectLogin 已触发，等 NM 变 Connecting/Connected（本循环顶部采样）。
                Sleep(kConnectWaitMs);
                continue;
            }

            PumpCtx ctx{};
            if (!x::runtime::managed_main::Call(&DoConnectOnPump, &ctx, 3000)) {
                snprintf(lastDetail, sizeof(lastDetail), "pump");
                if ((t % 5) == 0) {
                    LogLine("connect-wait try=%d Call fail/timeout", t);
                    KickLogLine("connect_wait pump_fail try=%d", t);
                }
                Sleep(kConnectWaitMs);
                continue;
            }
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
                Finish(2, fail);
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
            Finish(2, fail);
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
            Sleep(kPollMs);
            SampleCtx sample{};
            if (!x::runtime::managed_main::Call(&SampleNmOnPump, &sample, 1500)) {
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
                KickLogLine("RESULT connected rounds=%d — dismiss+reenter", i + 1);
                LogLine("NM Connected after soft ConnectLogin why=%s rounds=%d — dismiss dialogs",
                        gWhy, i + 1);
                goto connected_path;
            }
        }
        {
            char fail[220]{};
            snprintf(fail, sizeof(fail),
                     "RESULT fail final_state=%s(%d) err=%d nm_seen=%d why=%s", StateName(best),
                     best, lastErr, sawNm ? 1 : 0, gWhy);
            KickLogLine("RESULT fail state=%s(%d) err=%d", StateName(best), best, lastErr);
            Finish(2, fail);
        }
        goto next;
        }

    connected_path: {
                DismissCtx dismiss{};
                int dismissHits = 0;
                for (int d = 0; d < kDismissMissRetries && !gStop.load(); ++d) {
                    dismiss = {};
                    if (!x::runtime::managed_main::Call(&DismissKickDialogOnPump, &dismiss, 3000)) {
                        LogLine("dismiss Call fail/timeout try=%d", d);
                        KickLogLine("dismiss_fail reason=pump try=%d", d);
                    } else {
                        LogLine("dismiss try=%d %s", d, dismiss.detail);
                        KickLogLine("dismiss try=%d %s", d, dismiss.detail);
                    }
                    dismissHits += dismiss.scanned;
                    if (dismiss.scanned > 0) break;
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
                    LogLine("auto_enter RequestRestart armed — wait play-ready up to %ds",
                            (kReenterRounds * static_cast<int>(kReenterPollMs)) / 1000);
                    KickLogLine("reenter armed hold=1");

                    bool playOk = false;
                    bool earlyFail = false;
                    for (int r = 0; r < kReenterRounds && !gStop.load(); ++r) {
                        Sleep(kReenterPollMs);
                        if (gStop.load()) break;

                        if (x::features::auto_enter::IsFailed()) {
                            char fail[220]{};
                            snprintf(fail, sizeof(fail),
                                     "RESULT fail auto_enter Failed early why=%s reenter_round=%d",
                                     gWhy, r + 1);
                            KickLogLine("RESULT fail auto_enter_Failed round=%d", r + 1);
                            Finish(2, fail);
                            earlyFail = true;
                            break;
                        }

                        PlayReadyCtx play{};
                        if (x::runtime::managed_main::Call(&SamplePlayReadyOnPump, &play, 1500) &&
                            play.ready) {
                            playOk = true;
                            LogLine("play-ready — post-dismiss burst (Notice may appear late)");
                            KickLogLine("play-ready post_dismiss_burst");
                            for (int d = 0; d < kPostReadyDismissTries && !gStop.load(); ++d) {
                                DismissCtx post{};
                                post.aggressive = 1;
                                if (!x::runtime::managed_main::Call(&DismissKickDialogOnPump, &post,
                                                                    3000)) {
                                    LogLine("post_dismiss Call fail try=%d", d);
                                } else {
                                    LogLine("post_dismiss try=%d %s", d, post.detail);
                                    KickLogLine("post_dismiss try=%d %s", d, post.detail);
                                }
                                // 连续两轮无卸挡/Destroy → 认为清干净
                                if (d > 0 && post.inactivated == 0 && post.destroyed == 0 &&
                                    post.scanned == 0) {
                                    break;
                                }
                                if (d + 1 < kPostReadyDismissTries) Sleep(kPostReadyDismissGapMs);
                            }
                            if (gStop.load()) {
                                Finish(2, "abort: stop during post_dismiss");
                                earlyFail = true;
                                break;
                            }
                            char ok[220]{};
                            snprintf(ok, sizeof(ok),
                                     "RESULT success play-ready after soft reenter why=%s "
                                     "reenter_rounds=%d",
                                     gWhy, r + 1);
                            KickLogLine("RESULT success play-ready rounds=%d", r + 1);
                            Finish(1, ok);
                            break;
                        }

                        // 弹窗偶发再起：进图前每 ~1.5s 激进扫一次
                        if ((r % 3) == 2) {
                            DismissCtx again{};
                            again.aggressive = 1;
                            (void)x::runtime::managed_main::Call(&DismissKickDialogOnPump, &again,
                                                                 2000);
                            LogLine("dismiss retry %s", again.detail);
                            if (again.scanned > 0) KickLogLine("dismiss retry %s", again.detail);
                        }
                        if ((r % 10) == 0) LogLine("reenter wait[%d] playReady=0", r);
                    }
                    if (earlyFail) {
                        goto next;
                    }
                    if (gStop.load()) {
                        Finish(2, "abort: stop during reenter");
                        goto next;
                    }
                    if (!playOk) {
                        char fail[220]{};
                        snprintf(fail, sizeof(fail),
                                 "RESULT fail reenter_timeout playReady=0 why=%s — release hold "
                                 "for guardian relaunch",
                                 gWhy);
                        KickLogLine("RESULT fail reenter_timeout");
                        Finish(2, fail);
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
    gPending.store(true);
}

}  // namespace x::features::soft_login_probe
