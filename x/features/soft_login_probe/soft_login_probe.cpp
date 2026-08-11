// Classic TWMS — soft login ConnectLogin try-connect probe (default OFF).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "soft_login_probe.h"

#include "../auto_enter/auto_enter.h"
#include "../galaxy_token_probe/galaxy_token_probe.h"
#include "../notify/notify.h"
#include "../ports/foothold_port.h"
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

// Session.Disconnect / CloseSession（与 kick_sniff 同口径）。
// BIN 01:53：Disconnect 只把 Connected→Connecting→Connected，WorldItems 仍空（书页大厅）。
// 空大厅改 CloseSession 硬拆，再等 Disconnected 后才 ConnectLogin。
constexpr uint32_t kRvaNmDisconnect = 0x1CC5F20;
constexpr uint32_t kRvaNmCloseSession = 0x1CD50C0;
constexpr char kHashNmDisconnect[] =
    "b1d93da68c582074d5f57c0f056b4c22c5eea30ac0d09059a55ed7d41021109";
constexpr char kHashNmCloseSession[] =
    "c793578722e29984e2da932223b4d3f27403040237461ca3438083f0496b24d";

// settle 用墙钟截止（见 Worker）：Call 耗时曾未计入 waited，实机 1500ms 常被拉成 2.5–3s+。
// Notice 多在断线瞬间弹出；Connecting 早退即可，不必死等满窗。
constexpr DWORD kSettleMs = 600;
constexpr DWORD kPollMs = 400;
constexpr int kPollRounds = 40;  // ~16s；空大厅会提前在 kEmptyHallPollRounds 拆线
// BIN 01:53：Connected items=0 空等满 poll≈16s 体感卡死；满 8 轮(~3.2s) 即 CloseSession。
constexpr int kEmptyHallPollRounds = 8;
constexpr DWORD kWaitDiscAfterCloseMs = 4000;
constexpr int kEmptyHallSoftCycleMax = 3;  // 连续空大厅满额放 hold → 守护干净重拉
constexpr DWORD kReenterPollMs = 350;
// 墙钟总预算（每轮含 dismiss/SamplePlayReady Call，旧「130×350≈45s」严重低估；dcaf08 卡死约 114s）。
// 成功样本 armed→playReady 约 10–37s；Done→playReady 可达 ~32s；弱网再给裕量。
constexpr DWORD kReenterBudgetMs = 150000;
// Done(=left char)→play-ready 成功样本常 4–32s；满窗再 RequestRestart 并重置墙钟预算。
// 仅当泵采样确认仍 !inMap 时再启——已回图却因泵堵误判 !playReady 时再启会冻死主线程（E216）。
constexpr DWORD kDoneNoPlayRestartMs = 50000;
constexpr DWORD kDoneNoPlayFailMs = 50000;
// 单次 soft cycle 内 Done+!playReady 最多再拉几次选区（弱网多给机会，满额才 fail）。
constexpr int kDoneNoPlayMaxRestarts = 5;
// 同一断线边沿：ConnectLogin→重进 整轮可重试；多给机会再放 hold 交守护（B9B 体感乱杀）。
constexpr int kSoftCycleMax = 10;
constexpr DWORD kSoftCycleRetryGapMs = 1200;
// 重进等待：playReady 采样宜短；泵堵时 1500ms 等满只会叠 job timeout（E216）。
constexpr DWORD kPlayReadySampleMs = 400;
// 已 playReady+inMap 但 curFh=0（悬空/掉落，ec1fe7 heli 半空软重进）：再等挂台再 RESULT。
// 覆盖同图热重载数轮（约 3s/次）；满窗降级成功，勿再 ConnectLogin soft cycle。
constexpr DWORD kStandReadyWaitMs = 15000;
// 泵心跳：与 MainPump::IsPumpTicking 默认 1500ms 对齐。断线 InterStage 常短暂 idle
// （B9B 08:32：~2.5s idle 后游戏自连）；先等活再动，勿立刻放 hold 交守护。
constexpr DWORD kPumpAliveMaxAgeMs = 1500;
// SoftPumpCall 新鲜度：对齐泵 quiesce idle 闸（kTransitIdleFailMs=400）略严。
// 仍用 1500 判「可 Call」会在 Bootstrap 半死时空打 High Sample/Dismiss（B9B 9865c3）。
constexpr DWORD kSoftPumpFreshMs = 300;
constexpr DWORD kSoftSampleCallMs = 400;  // 对齐 kTransitInvokeCapMs；勿再等满 1500
// 成功进图后玩法错峰：Finish 放 hold，但仍压 Combat/Invuln 急钉，避免 q=8 齐开（B9B）。
// ce6797：quiet 一结束立刻 BAN ON + |v|~7k Impact → 再软断。
// land quiet = 整段停刀/停旋翼；post_air_gate ≥ quiet，防 quiet 被提前清掉后仍空中开打。
// BIN 01:17：arm=3s/air=4.5s → RESULT 后 ~4.7s 才首刀，体感静默偏长；无敌已 hold/quiet 急钉，压到 1.5s/2.5s。
constexpr DWORD kSoftLandQuietMs = 1500;
constexpr DWORD kSoftPostAirGateMs = 2500;
// Connected 后等分区列表刷出再 RequestRestart（BIN 21:44：壳指针先到、WorldItems=0）。
// BIN 00:46：Connected 空列表卡书页大厅；12s×2 体感「没连世界」。缩短单次等待，空列表靠 soft cycle + ConnectLogin 刷新。
constexpr DWORD kHallReadyWaitMs = 6000;
constexpr DWORD kHallReadyPollMs = 200;
// softFast 卡空 WorldItems：早于 150s 墙钟 soft cycle。
constexpr DWORD kWorldItemsStarveMs = 10000;
constexpr DWORD kPumpWaitBeforeSoftMs = 20000;   // soft cycle 开头等泵
constexpr DWORD kPumpWaitConnectMs = 20000;      // connect-wait 判死前再等一轮
constexpr int kReenterPumpFailMax = 48;          // 重进连续泵失败（退避睡眠，约半分钟级）
constexpr DWORD kReenterPumpFailSleepMinMs = 500;
constexpr DWORD kReenterPumpFailSleepMaxMs = 2500;
// connect-wait：连续 idle/Call 失败阈值；满额先 WaitPumpAlive，仍死才 soft cycle / 放 hold。
constexpr int kConnectPumpFailMax = 40;  // ~18–25s @ 450ms；再等泵窗，勿早交守护
// 进图后断线 SceneLogin 常晚于 settle 才重建；sl_null 时 hold 内重试，顺带吃游戏自连。
constexpr DWORD kConnectWaitMs = 250;
constexpr int kConnectWaitRounds = 160;  // ~40s 等 SL / Connecting / Connected
constexpr int kConnectHardFailGrace = 8;  // 非 sl_null 硬错也先多轮重试再放弃

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
// UIUtilDialog.Notice — 直 call 入口 Abs trampoline（非 MI 换针；≥400 处 bypass methodPointer）
// 默认关闭：代码保留待测稳后再开；现行 dismiss 只走 FindAll。
constexpr bool kNoticeAbsEnabled = false;
constexpr uint32_t kRvaNotice = 0x74ACD0;
// 序言：push r15/r14/r12/rsi/rdi/rbp/rbx ; sub rsp,80h（17B，指令边界；IDA 运行时 dump）
constexpr size_t kNoticeSteal = 17;
constexpr uint8_t kNoticeSig[kNoticeSteal] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57,
                                              0x55, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00,
                                              0x00};
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
using FnNmVoid0 = void(__fastcall*)(void* self, const void* method);
using FnGetGameObject = void* (*)(void* self, const void* method);
using FnGoSetActive = void (*)(void* self, bool value, const void* method);
using FnGoGetActiveSelf = bool (*)(void* self, const void* method);
using FnObjectDestroy = void (*)(void* obj, const void* method);
using FnDialogClose = void (*)(void* self, const void* method);  // CloseDialog / UIDialog.Close
using FnNotice = void* (*)(void* sMsg, void* sSub, uint8_t a, uint8_t b, uint8_t c, void* ok,
                           const void* method);

struct AbsHook {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};
std::atomic<bool> gPending{false};
std::atomic<bool> gBusy{false};
std::atomic<bool> gHold{false};
std::atomic<DWORD> gLandQuietUntilMs{0};     // GetTickCount 截止；0=无静默
std::atomic<DWORD> gPostSoftAirUntilMs{0};  // 禁止 F5 空中；≥ land quiet
std::atomic<unsigned> gResult{0};  // 0 none 1 ok 2 fail
std::atomic<bool> gUiEnabled{false};
char gWhy[64]{};

AbsHook gNoticeAbs{};
std::atomic<void*> gLastNoticeDlg{nullptr};   // Abs hook 截到的最近 Notice 返回值
std::atomic<unsigned> gNoticeAbsHits{0};      // 进程内累计捕获次数
std::atomic<int> gNoticeAbsInstall{0};        // 0=未试 1=ok 2=fail

struct PumpCtx {
    int ok = 0;
    char detail[160]{};
};

struct SampleCtx {
    int state = -1;
    int err = -1;
    int nmOk = 0;
    int slOk = 0;        // SceneLogin 实例可读
    int worldUi = 0;     // SoftHall：分区 UI
    int channelUi = 0;   // SoftHall：频道 UI
    int worldItems = 0;  // WorldItems.Count
    int hallReady = 0;   // 列表非空或频道页可续进（禁止仅壳指针）
};

// InterStage quiesce（map-transit && !freeze）只放行 High；soft 系统短探对齐 channel_hop。
bool SoftPumpCall(x::runtime::main_thread::JobFn fn, void* user, DWORD timeoutMs) {
    return x::runtime::main_thread::InvokeAndWait(fn, user, timeoutMs,
                                                 x::runtime::main_thread::JobPrio::High);
}

// 泵心跳不够新鲜则禁 SoftPumpCall（Sleep 等活）。禁止用 IsMapTransitBlocked /
// IsCongested：quiesce 期恒真，登录大厅会饿死 ConnectLogin。
bool SoftShouldDeferPumpWork() {
    return !x::runtime::main_thread::IsPumpTicking(kSoftPumpFreshMs);
}

// 已 Connected 且大厅可点分区（WorldItems>0 或频道页可续进）：勿再 ConnectLogin。
// BIN 21:44：仅 world/ch 壳指针非空会过早 RequestRestart → waiting WorldItems? 卡死。
bool LoginUiReady(const SampleCtx& s) {
    return s.nmOk != 0 && s.state == kStateConnected && s.hallReady != 0;
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
    int cacheTried = 0;  // Abs 捕获实例本枪尝试关
    char detail[224]{};
};

struct PlayReadyCtx {
    int ready = 0;
    int inMap = 0;
    int sampled = 0;  // 1=泵上跑完；0=未进泵 / Assert 失败
    uint32_t curFh = 0;  // PeekCurFhId；inMap 时采，悬空=0
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
    ctx->worldItems = 0;
    ctx->hallReady = 0;
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
    void* sl = PeekSceneLoginOnPump();
    if (sl) ctx->slOk = 1;
    // 大厅真就绪：同泵 SoftHall（分区列表 / 频道续进），勿只认壳指针。
    x::features::auto_enter::SoftHallCtx hall{};
    x::features::auto_enter::SoftHallSampleOnPump(&hall);
    if (hall.ok) {
        ctx->worldUi = hall.worldUi;
        ctx->channelUi = hall.channelUi;
        ctx->worldItems = hall.worldItems;
        ctx->hallReady = hall.ready;
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

// Connected 但 WorldItems 空：Disconnect 只会自连回 Connected（BIN 01:53）；用 CloseSession 硬拆。
void DoNmCloseSessionOnPump(void* user) {
    auto* ctx = static_cast<PumpCtx*>(user);
    if (!ctx) return;
    ctx->ok = 0;
    ctx->detail[0] = '\0';
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginNmClose")) {
        snprintf(ctx->detail, sizeof(ctx->detail), "not_on_pump");
        return;
    }
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(ctx->detail, sizeof(ctx->detail), "il2cpp");
        return;
    }
    x::runtime::il2cpp_network::Ensure();
    void* nm = ResolveNmFacadeOnPump();
    if (!nm || !LooksLikeHeapPtr(nm)) {
        snprintf(ctx->detail, sizeof(ctx->detail), "nm_null");
        return;
    }
    void* session = ReadPtr(nm, x::runtime::il2cpp_network::OffNmSession());
    if (!session || !LooksLikeHeapPtr(session)) {
        snprintf(ctx->detail, sizeof(ctx->detail), "session_null");
        return;
    }
    void* sessKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    if (!sessKlass) {
        snprintf(ctx->detail, sizeof(ctx->detail), "sess_klass");
        return;
    }
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    const MethodShape kVoid0{0, TypeKind::Void, true, true};
    auto closeRes = x::runtime::il2cpp_method::FindMethodResolved(
        sessKlass, kRvaNmCloseSession, kVoid0, "CloseSession", kHashNmCloseSession);
    MethodInfoHead* mi = AsMi(closeRes.method);
    if (!mi || !mi->methodPointer) {
        // CloseSession 解析失败时退 Disconnect（弱于 Close，但总好过空转 ConnectLogin）。
        auto discRes = x::runtime::il2cpp_method::FindMethodResolved(
            sessKlass, kRvaNmDisconnect, kVoid0, "Disconnect", kHashNmDisconnect);
        mi = AsMi(discRes.method);
        if (!mi || !mi->methodPointer) {
            snprintf(ctx->detail, sizeof(ctx->detail), "close_mi");
            return;
        }
        __try {
            reinterpret_cast<FnNmVoid0>(mi->methodPointer)(session, mi);
            ctx->ok = 1;
            snprintf(ctx->detail, sizeof(ctx->detail), "Disconnect session=%p path=%s", session,
                     x::runtime::il2cpp_method::PathName(discRes.path));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            snprintf(ctx->detail, sizeof(ctx->detail), "exn");
        }
        return;
    }
    __try {
        reinterpret_cast<FnNmVoid0>(mi->methodPointer)(session, mi);
        ctx->ok = 1;
        snprintf(ctx->detail, sizeof(ctx->detail), "CloseSession session=%p path=%s", session,
                 x::runtime::il2cpp_method::PathName(closeRes.path));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snprintf(ctx->detail, sizeof(ctx->detail), "exn");
    }
}

// CloseSession 后等掉到 Disconnected / session 空，避免立刻又 ConnectLogin 打在 Connected 上。
bool WaitNmNotConnected(DWORD waitMs, const char* tag) {
    const DWORD deadline = GetTickCount() + waitMs;
    while (!gStop.load() && static_cast<int>(deadline - GetTickCount()) > 0) {
        if (SoftShouldDeferPumpWork()) {
            Sleep(100);
            continue;
        }
        SampleCtx s{};
        if (!SoftPumpCall(&SampleNmOnPump, &s, kSoftSampleCallMs)) {
            Sleep(100);
            continue;
        }
        if (!s.nmOk || s.state == kStateDisconnected || s.state == kStateDisconnecting ||
            s.state < 0) {
            LogLine("wait_disc ok tag=%s nmOk=%d state=%s(%d)", tag ? tag : "?", s.nmOk,
                    StateName(s.state), s.state);
            KickLogLine("wait_disc ok tag=%s state=%d", tag ? tag : "?", s.state);
            return true;
        }
        Sleep(150);
    }
    LogLine("wait_disc timeout tag=%s %ums", tag ? tag : "?", static_cast<unsigned>(waitMs));
    KickLogLine("wait_disc timeout tag=%s", tag ? tag : "?");
    return false;
}

// true=已发起硬拆（或 session 已空）；调用方清 invokeOk，等 Disconnected 再 ConnectLogin。
bool SoftForceNmTeardown(const char* why) {
    PumpCtx ctx{};
    if (!SoftPumpCall(&DoNmCloseSessionOnPump, &ctx, 3000)) {
        LogLine("force_nm_close Call fail/timeout why=%s", why ? why : "?");
        KickLogLine("force_nm_close pump_fail");
        return false;
    }
    LogLine("force_nm_close ok=%d detail=%s why=%s", ctx.ok, ctx.detail, why ? why : "?");
    KickLogLine("force_nm_close ok=%d why=%s", ctx.ok, why ? why : "?");
    const bool closed = ctx.ok != 0 || std::strcmp(ctx.detail, "session_null") == 0 ||
                        std::strcmp(ctx.detail, "nm_null") == 0;
    if (closed) (void)WaitNmNotConnected(kWaitDiscAfterCloseMs, why);
    return closed;
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

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

void* __fastcall HookNoticeAbs(void* sMsg, void* sSub, uint8_t a, uint8_t b, uint8_t c, void* ok,
                               const void* method) {
    auto* orig = reinterpret_cast<FnNotice>(gNoticeAbs.trampoline);
    void* dlg = nullptr;
    if (orig) {
        __try {
            dlg = orig(sMsg, sSub, a, b, c, ok, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            dlg = nullptr;
        }
    }
    // 软登录 hold 期才记；其它 Notice（旅行 tip 等）不冲断线窗指针。
    if (dlg && gHold.load(std::memory_order_acquire)) {
        gLastNoticeDlg.store(dlg, std::memory_order_release);
        gNoticeAbsHits.fetch_add(1, std::memory_order_relaxed);
    }
    return dlg;
}

bool InstallNoticeAbs(void* target) {
    if (gNoticeAbs.active) return true;
    if (!target) return false;
    auto* bytes = reinterpret_cast<uint8_t*>(target);
    for (size_t i = 0; i < kNoticeSteal; ++i) {
        if (bytes[i] != kNoticeSig[i]) {
            LogLine("Notice Abs refuse: sig mismatch @%p b0=%02X want=%02X", target, bytes[0],
                    kNoticeSig[0]);
            return false;
        }
    }
    void* tramp =
        VirtualAlloc(nullptr, kNoticeSteal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(gNoticeAbs.saved, target, kNoticeSteal);
    memcpy(tramp, target, kNoticeSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kNoticeSteal,
                reinterpret_cast<uint8_t*>(target) + kNoticeSteal);
    DWORD old = 0;
    if (!VirtualProtect(target, kNoticeSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, reinterpret_cast<void*>(&HookNoticeAbs));
    for (size_t i = 12; i < kNoticeSteal; ++i) reinterpret_cast<uint8_t*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, kNoticeSteal);
    VirtualProtect(target, kNoticeSteal, old, &old);
    gNoticeAbs.target = target;
    gNoticeAbs.trampoline = tramp;
    gNoticeAbs.stolen = kNoticeSteal;
    gNoticeAbs.active = true;
    return true;
}

// 进程级一次：直 call Notice 入口 Abs（对齐 travel_port Send Abs），截返回实例。
// kNoticeAbsEnabled=false 时为空操作（实现保留，等测稳再开）。
void EnsureNoticeAbsHook() {
    if (!kNoticeAbsEnabled) return;
    if (gNoticeAbs.active) {
        gNoticeAbsInstall.store(1, std::memory_order_relaxed);
        return;
    }
    const int st = gNoticeAbsInstall.load(std::memory_order_relaxed);
    if (st == 2) return;  // 签名失败等硬拒，勿重试
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (!ga) ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) return;  // 未加载：保持 st=0，下次再试
    void* target = reinterpret_cast<uint8_t*>(ga) + kRvaNotice;
    if (!InstallNoticeAbs(target)) {
        gNoticeAbsInstall.store(2, std::memory_order_relaxed);
        LogLine("Notice Abs install fail rva=0x%X", kRvaNotice);
        return;
    }
    gNoticeAbsInstall.store(1, std::memory_order_relaxed);
    LogLine("Notice Abs install ok rva=0x%X steal=%zu (direct-call capture)", kRvaNotice,
            kNoticeSteal);
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
    EnsureNoticeAbsHook();

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

    // Abs 捕获实例优先关一枪（仅 kNoticeAbsEnabled）；仍强制 FindAll（69c8f9）。
    int cacheTried = 0;
    if (kNoticeAbsEnabled && scanNotices) {
        void* cached = gLastNoticeDlg.exchange(nullptr, std::memory_order_acq_rel);
        if (cached && LooksLikeHeapPtr(cached)) {
            cacheTried = 1;
            HideDialogVisual(cached, miCloseDialog, miUiClose, miGetGo, miSetActive, miGetActive,
                             miGetHier, destroyMi, force, ctx);
        }
    }
    ctx->cacheTried = cacheTried;

    // 正式关窗：CloseDialog(+SetActive) + FindAll(Util/Ex[/Notice 白名单])。
    // 永不 FindAll(UIDialog 基类)。
    int noticeHits = 0;
    DismissDialogsOfKlass(utilEx, force, ctx, miCloseDialog, miUiClose, miGetGo, miSetActive,
                          miGetActive, miGetHier, destroyMi);
    DismissDialogsOfKlass(util, force, ctx, miCloseDialog, miUiClose, miGetGo, miSetActive,
                          miGetActive, miGetHier, destroyMi);
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
             "agg=%d notices=%d noticeKinds=%d cache=%d abs=%d hits=%u scan=%d close=%d ok=0 "
             "inactive=%d destroy=%d dead=%d alreadyOff=%d skipHud=%d miClose=%d miUiClose=%d "
             "hier=%d",
             aggressive, scanNotices, noticeHits, cacheTried, gNoticeAbs.active ? 1 : 0,
             gNoticeAbsHits.load(std::memory_order_relaxed), ctx->scanned, ctx->closed,
             ctx->inactivated, ctx->destroyed, ctx->skippedDead, ctx->skippedInactive,
             ctx->skippedHud, miCloseDialog ? 1 : 0, miUiClose ? 1 : 0, miGetHier ? 1 : 0);
}

void SamplePlayReadyOnPump(void* user) {
    auto* ctx = static_cast<PlayReadyCtx*>(user);
    if (!ctx) return;
    ctx->ready = 0;
    ctx->inMap = 0;
    ctx->sampled = 0;
    ctx->curFh = 0;
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginPlayReady")) return;
    // 仅在 Unity 泵上调（副作用 SetPumpPhase/Transit 与 publisher 同线程）。
    ctx->inMap = x::features::ports::world::IsInMapScene() ? 1 : 0;
    ctx->ready = x::features::ports::world::IsPlayReady() ? 1 : 0;
    if (ctx->inMap) {
        __try {
            ctx->curFh = x::features::ports::foothold::PeekCurFhId();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ctx->curFh = 0;
        }
    }
    ctx->sampled = 1;
}

void SetHold(bool on) { gHold.store(on, std::memory_order_release); }

void ArmLandQuiet(DWORD ms) {
    const DWORD dur = ms ? ms : kSoftLandQuietMs;
    const DWORD now = GetTickCount();
    DWORD until = now + dur;
    if (until == 0) until = 1;
    gLandQuietUntilMs.store(until, std::memory_order_release);
    DWORD airUntil = now + kSoftPostAirGateMs;
    if (airUntil == 0) airUntil = 1;
    // 若调用方传入更长 quiet，空中闸至少盖住 quiet。
    if (static_cast<int>(airUntil - until) < 0) airUntil = until;
    gPostSoftAirUntilMs.store(airUntil, std::memory_order_release);
    LogLine("land_quiet arm=%ums post_air_gate=%ums", static_cast<unsigned>(dur),
            static_cast<unsigned>(airUntil - now));
    KickLogLine("land_quiet arm=%ums post_air=%ums", static_cast<unsigned>(dur),
                static_cast<unsigned>(airUntil - now));
}

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

// Connected 后等分区列表（或频道续进）就绪再 RequestRestart。
// true=可进；false=超时/停止（调用方 soft cycle，勿空等 WorldItems）。
bool WaitHallPickReady(DWORD waitMs) {
    const DWORD deadline = GetTickCount() + waitMs;
    int lastItems = -1;
    int lastReady = -1;
    while (!gStop.load()) {
        if (SoftShouldDeferPumpWork()) {
            Sleep(kHallReadyPollMs);
            if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
            continue;
        }
        x::features::auto_enter::SoftHallCtx hall{};
        if (SoftPumpCall(&x::features::auto_enter::SoftHallSampleOnPump, &hall,
                         kSoftSampleCallMs) &&
            hall.ok) {
            if (hall.worldItems != lastItems || hall.ready != lastReady) {
                LogLine("hall_ready poll world=%d ch=%d items=%d selWorld=%d ready=%d",
                        hall.worldUi, hall.channelUi, hall.worldItems, hall.selectedWorld,
                        hall.ready);
                lastItems = hall.worldItems;
                lastReady = hall.ready;
            }
            if (hall.ready) {
                KickLogLine("hall_ready ok items=%d selWorld=%d", hall.worldItems,
                            hall.selectedWorld);
                return true;
            }
        }
        if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
        Sleep(kHallReadyPollMs);
    }
    LogLine("hall_ready timeout %ums lastItems=%d", static_cast<unsigned>(waitMs), lastItems);
    KickLogLine("hall_ready timeout items=%d", lastItems);
    return false;
}

// 弱网 / 泵暂死可恢复：未用尽 soft cycle 则保持 hold/busy，回跳再跑 ConnectLogin→重进。
// true = 调用方 ++softCycle 后 goto soft_cycle_begin；false = 已 Finish(2) 或应停。
// 泵短暂 idle 允许 soft cycle（B9B 08:32：connect_wait_pump_dead 后 ~1s 自连，
// 旧 soft_fatal_no_retry 立刻放 hold → 守护干净重拉，体感「乱杀」）。
// emptyHallStreak：连续空大厅满 kEmptyHallSoftCycleMax 放 hold（BIN 01:53 书页空转）。
bool SoftFailOrRetry(int softCycle, const char* failLine, int* emptyHallStreak) {
    const bool emptyHall =
        failLine && std::strstr(failLine, "hall_world_items_empty") != nullptr;
    if (emptyHall && emptyHallStreak) {
        ++(*emptyHallStreak);
        if (*emptyHallStreak >= kEmptyHallSoftCycleMax) {
            LogLine("empty_hall streak=%d/%d — release hold for guardian", *emptyHallStreak,
                    kEmptyHallSoftCycleMax);
            KickLogLine("empty_hall give_up streak=%d", *emptyHallStreak);
            Finish(2, failLine);
            return false;
        }
    } else if (emptyHallStreak) {
        *emptyHallStreak = 0;
    }
    if (softCycle < kSoftCycleMax && !gStop.load()) {
        const bool pumpish =
            failLine && (std::strstr(failLine, "pump_dead") || std::strstr(failLine, "pump_fail") ||
                         (std::strstr(failLine, "connect_wait_timeout") &&
                          std::strstr(failLine, "last=pump")));
        if (pumpish) {
            // 周期开头还会 WaitPumpAlive；此处先再等一窗，吃「自连前泵刚醒」窗口。
            (void)WaitPumpAlive(kPumpWaitConnectMs, "soft_cycle_retry");
        }
        LogLine("soft_cycle_retry %d/%d after %ums | %s", softCycle, kSoftCycleMax,
                static_cast<unsigned>(kSoftCycleRetryGapMs), failLine ? failLine : "?");
        KickLogLine("soft_cycle_retry %d/%d", softCycle, kSoftCycleMax);
        Sleep(kSoftCycleRetryGapMs);
        return true;
    }
    Finish(2, failLine);
    return false;
}

bool SoftFailOrRetry(int softCycle, const char* failLine) {
    return SoftFailOrRetry(softCycle, failLine, nullptr);
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
        EnsureNoticeAbsHook();
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
        int emptyHallStreak = 0;
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
                     "RESULT fail pump_dead before_settle why=%s soft_cycle=%d — soft cycle or release hold",
                     gWhy, softCycle);
            KickLogLine("RESULT fail pump_dead before_settle cycle=%d", softCycle);
            if (SoftFailOrRetry(softCycle, fail)) {
                ++softCycle;
                goto soft_cycle_begin;
            }
            goto next;
        }

        // 断线 Notice 往往立刻弹出；勿等 Connected 才关。
        // 墙钟截止 + Connecting/Connected 早退（302081）。
        // 大厅减负（d29b56 后）：settle 内 **最多 1 次** FindAll dismiss，其余轮只 Sample NM；
        // 勿每 120ms 叠 Util/Ex/Notice（Bootstrap 半死时易挤 ConnectLogin）。
        // scanBase=1：Notice 白名单；永不 FindAll(UIDialog 基类)。
        {
            const DWORD settleDeadline = GetTickCount() + kSettleMs;
            bool settleDismissDone = false;
            while (!gStop.load() && static_cast<int>(settleDeadline - GetTickCount()) > 0) {
                const int remain0 = static_cast<int>(settleDeadline - GetTickCount());
                if (remain0 <= 0) break;
                if (SoftShouldDeferPumpWork()) {
                    const DWORD gap =
                        remain0 < static_cast<int>(kEarlyDismissGapMs)
                            ? static_cast<DWORD>(remain0)
                            : kEarlyDismissGapMs;
                    Sleep(gap);
                    continue;
                }
                if (!settleDismissDone) {
                    DismissCtx early{};
                    early.aggressive = 1;
                    early.scanBase = 1;
                    if (SoftPumpCall(&DismissKickDialogOnPump, &early, kDismissCallMs)) {
                        LogLine("settle_dismiss %s", early.detail);
                        KickLogLine("settle_dismiss %s", early.detail);
                        // 关到窗才收工；空枪保留机会吃晚到的 Notice。
                        if (early.closed > 0 || early.inactivated > 0) settleDismissDone = true;
                    }
                }
                const int remainBeforeSample = static_cast<int>(settleDeadline - GetTickCount());
                if (remainBeforeSample <= 0) break;
                if (SoftShouldDeferPumpWork()) {
                    Sleep(remainBeforeSample < static_cast<int>(kEarlyDismissGapMs)
                              ? static_cast<DWORD>(remainBeforeSample)
                              : kEarlyDismissGapMs);
                    continue;
                }
                // Sample 超时勿超过剩余墙钟，且对齐 kSoftSampleCallMs（勿空等 1500）。
                const DWORD sampleMs =
                    remainBeforeSample < static_cast<int>(kSoftSampleCallMs)
                        ? static_cast<DWORD>(remainBeforeSample)
                        : kSoftSampleCallMs;
                SampleCtx sample{};
                if (SoftPumpCall(&SampleNmOnPump, &sample, sampleMs) && sample.nmOk &&
                    (sample.state == kStateConnecting ||
                     (sample.state == kStateConnected && LoginUiReady(sample)))) {
                    LogLine("settle early-exit nm=%s(%d) items=%d ready=%d remain≈%dms "
                            "dismiss_done=%d",
                            StateName(sample.state), sample.state, sample.worldItems,
                            sample.hallReady, static_cast<int>(settleDeadline - GetTickCount()),
                            settleDismissDone ? 1 : 0);
                    KickLogLine("settle early_nm state=%s(%d) ready=%d", StateName(sample.state),
                                sample.state, sample.hallReady);
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
            // 已 dismiss 过则不再 early-NM 补枪；窗仍在由 pre_dismiss / Connected dismiss 收。
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
        if (!SoftShouldDeferPumpWork()) {
            SampleCtx peek{};
            if (SoftPumpCall(&SampleNmOnPump, &peek, kSoftSampleCallMs) && LoginUiReady(peek)) {
                LogLine("login_ui_ready post-settle world=%d ch=%d items=%d — skip ConnectLogin "
                        "arm reenter",
                        peek.worldUi, peek.channelUi, peek.worldItems);
                KickLogLine("resume_login_ui skip_connect world=%d ch=%d items=%d where=post_settle",
                            peek.worldUi, peek.channelUi, peek.worldItems);
                invokeOk = true;
                goto connected_path;
            }
        }

        connectPumpFail = 0;
        for (int t = 0; t < kConnectWaitRounds && !gStop.load(); ++t) {
            // 泵 idle：禁止 Sample/Dismiss/ConnectLogin（BIN 02:30 空打 ~20s）。
            if (SoftShouldDeferPumpWork()) {
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
                    // 满额先再等泵一窗（B9B：idle 数秒后自连）；活了就清 streak 继续本 cycle。
                    if (WaitPumpAlive(kPumpWaitConnectMs, "connect_wait")) {
                        connectPumpFail = 0;
                        LogLine("connect-wait pump revived after idle — continue try=%d", t);
                        KickLogLine("connect_wait pump_revived try=%d", t);
                        continue;
                    }
                    char fail[220]{};
                    snprintf(fail, sizeof(fail),
                             "RESULT fail connect_wait_pump_dead why=%s streak=%d try=%d — "
                             "soft cycle or release hold",
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
            const bool sampleCallOk =
                SoftPumpCall(&SampleNmOnPump, &sample, kSoftSampleCallMs);
            if (!sampleCallOk) {
                // Call fail 同轮禁 pre_dismiss+Connect，下一轮再 Sample。
                snprintf(lastDetail, sizeof(lastDetail), "pump");
                ++connectPumpFail;
                if ((t % 5) == 0 || connectPumpFail <= 2) {
                    LogLine("connect-wait try=%d Sample Call fail/timeout streak=%d", t,
                            connectPumpFail);
                    KickLogLine("connect_wait sample_pump_fail try=%d streak=%d", t,
                                connectPumpFail);
                }
                if (connectPumpFail >= kConnectPumpFailMax) {
                    if (WaitPumpAlive(kPumpWaitConnectMs, "connect_wait_sample")) {
                        connectPumpFail = 0;
                        LogLine("connect-wait pump revived after Sample fail — continue try=%d",
                                t);
                        KickLogLine("connect_wait pump_revived_sample try=%d", t);
                        continue;
                    }
                    char fail[220]{};
                    snprintf(fail, sizeof(fail),
                             "RESULT fail connect_wait_pump_dead why=%s streak=%d try=%d "
                             "last=sample_pump — soft cycle or release hold",
                             gWhy, connectPumpFail, t);
                    KickLogLine("RESULT fail connect_wait_pump_dead streak=%d", connectPumpFail);
                    if (SoftFailOrRetry(softCycle, fail)) {
                        ++softCycle;
                        goto soft_cycle_begin;
                    }
                    goto next;
                }
                Sleep(kConnectWaitMs);
                continue;
            }
            if (sample.nmOk) {
                connectPumpFail = 0;
                if (sample.state == kStateConnected) {
                    if (LoginUiReady(sample)) {
                        LogLine("login_ui_ready world=%d ch=%d items=%d try=%d — skip ConnectLogin",
                                sample.worldUi, sample.channelUi, sample.worldItems, t);
                        KickLogLine("resume_login_ui skip_connect world=%d ch=%d items=%d try=%d",
                                    sample.worldUi, sample.channelUi, sample.worldItems, t);
                        invokeOk = true;
                        goto connected_path;
                    }
                    // BIN 01:53：Connected+items=0 时 ConnectLogin 空转；Disconnect 只自连回
                    // Connected。禁止在 Connected 上 ConnectLogin → CloseSession 等到掉线再连。
                    LogLine("NM Connected but hall not ready world=%d ch=%d items=%d try=%d — "
                            "CloseSession (no ConnectLogin while Connected)",
                            sample.worldUi, sample.channelUi, sample.worldItems, t);
                    KickLogLine("connect_wait Connected_close_session try=%d items=%d", t,
                                sample.worldItems);
                    SoftForceNmTeardown("connected_empty_hall");
                    invokeOk = false;
                    sawConnecting = false;
                    Sleep(kConnectWaitMs);
                    continue;
                }
                if (sample.state == kStateConnecting) {
                    if (!sawConnecting) {
                        sawConnecting = true;
                        LogLine("NM Connecting during connect-wait try=%d — hold, no dismiss", t);
                        KickLogLine("connect_wait Connecting try=%d", t);
                    }
                    // Connecting：自连优先，不抢泵做 dismiss。
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
                if (connectPumpFail >= kConnectPumpFailMax || SoftShouldDeferPumpWork()) {
                    if (connectPumpFail >= kConnectPumpFailMax) {
                        if (WaitPumpAlive(kPumpWaitConnectMs, "connect_wait_call")) {
                            connectPumpFail = 0;
                            LogLine("connect-wait pump revived after Call fail — continue try=%d",
                                    t);
                            KickLogLine("connect_wait pump_revived_call try=%d", t);
                            continue;
                        }
                        char fail[220]{};
                        snprintf(fail, sizeof(fail),
                                 "RESULT fail connect_wait_pump_dead why=%s streak=%d try=%d "
                                 "last=pump — soft cycle or release hold",
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
        int emptyHallPoll = 0;
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
            if (!SoftPumpCall(&SampleNmOnPump, &sample, kSoftSampleCallMs)) {
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
                KickLogLine("RESULT connected rounds=%d world=%d ch=%d items=%d ready=%d", i + 1,
                            sample.worldUi, sample.channelUi, sample.worldItems, sample.hallReady);
                if (LoginUiReady(sample)) {
                    LogLine("NM Connected+login_ui after soft path why=%s rounds=%d world=%d ch=%d "
                            "items=%d hallReady=1",
                            gWhy, i + 1, sample.worldUi, sample.channelUi, sample.worldItems);
                    goto connected_path;
                }
                // BIN 01:53：空等满 poll≈16s；满 kEmptyHallPollRounds 即 CloseSession soft cycle。
                ++emptyHallPoll;
                if ((emptyHallPoll % 2) == 1) {
                    LogLine("poll[%d] Connected hall not ready items=%d empty=%d/%d", i,
                            sample.worldItems, emptyHallPoll, kEmptyHallPollRounds);
                }
                if (emptyHallPoll >= kEmptyHallPollRounds) break;
                continue;
            }
            emptyHallPoll = 0;
        }
        {
            SampleCtx last{};
            if (SoftPumpCall(&SampleNmOnPump, &last, kSoftSampleCallMs) && last.nmOk &&
                last.state == kStateConnected && !LoginUiReady(last)) {
                LogLine("poll end Connected items=%d — CloseSession then soft cycle",
                        last.worldItems);
                KickLogLine("poll_end Connected_empty_hall close_session");
                SoftForceNmTeardown("poll_empty_hall");
                char failHall[200]{};
                snprintf(failHall, sizeof(failHall),
                         "RESULT fail hall_world_items_empty why=%s where=poll — soft cycle "
                         "or release hold",
                         gWhy);
                if (SoftFailOrRetry(softCycle, failHall, &emptyHallStreak)) {
                    ++softCycle;
                    goto soft_cycle_begin;
                }
                goto next;
            }
            char fail[220]{};
            snprintf(fail, sizeof(fail),
                     "RESULT fail final_state=%s(%d) err=%d nm_seen=%d why=%s", StateName(best),
                     best, lastErr, sawNm ? 1 : 0, gWhy);
            KickLogLine("RESULT fail state=%s(%d) err=%d", StateName(best), best, lastErr);
            if (SoftFailOrRetry(softCycle, fail, &emptyHallStreak)) {
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

                // 分区列表未刷出就 RequestRestart → auto_enter 卡 waiting WorldItems?（BIN 21:44）。
                if (!WaitHallPickReady(kHallReadyWaitMs)) {
                    SoftForceNmTeardown("hall_wait_timeout");
                    char fail[200]{};
                    snprintf(fail, sizeof(fail),
                             "RESULT fail hall_world_items_empty why=%s wait=%ums — soft cycle "
                             "or release hold",
                             gWhy, static_cast<unsigned>(kHallReadyWaitMs));
                    KickLogLine("RESULT fail hall_world_items_empty");
                    if (SoftFailOrRetry(softCycle, fail, &emptyHallStreak)) {
                        ++softCycle;
                        goto soft_cycle_begin;
                    }
                    goto next;
                }
                emptyHallStreak = 0;

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
                    bool awaitingStand = false;  // playReady+inMap 但 curFh=0
                    DWORD standSinceMs = 0;
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
                                if (WaitPumpAlive(kPumpWaitConnectMs, "reenter_idle")) {
                                    pumpFailStreak = 0;
                                    LogLine("reenter pump revived after idle — continue round=%d",
                                            r + 1);
                                    KickLogLine("reenter pump_revived round=%d", r + 1);
                                    continue;
                                }
                                char fail[220]{};
                                snprintf(fail, sizeof(fail),
                                         "RESULT fail reenter_pump_dead why=%s streak=%d "
                                         "round=%d — soft cycle or release hold",
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

                        // 大厅关窗仅重进首轮一枪；进图后禁止每轮 FindAll（E216）；勿再按 every-N 叠泵。
                        const bool stillInHall = !x::features::auto_enter::IsDone() &&
                                                 !x::features::auto_enter::IsFailed();
                        if (stillInHall && r == 0) {
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

                        // softFast 卡空分区列表：勿空耗 150s 墙钟。
                        if (x::features::auto_enter::IsWorldItemsStarve(kWorldItemsStarveMs)) {
                            char fail[220]{};
                            snprintf(fail, sizeof(fail),
                                     "RESULT fail world_items_starve why=%s age>=%ums "
                                     "reenter_round=%d — soft cycle or release hold",
                                     gWhy, static_cast<unsigned>(kWorldItemsStarveMs), r + 1);
                            KickLogLine("RESULT fail world_items_starve round=%d", r + 1);
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
                                if (WaitPumpAlive(kPumpWaitConnectMs, "reenter_call")) {
                                    pumpFailStreak = 0;
                                    LogLine("reenter pump revived after Call fail — continue "
                                            "round=%d",
                                            r + 1);
                                    KickLogLine("reenter pump_revived_call round=%d", r + 1);
                                    continue;
                                }
                                char fail[220]{};
                                snprintf(fail, sizeof(fail),
                                         "RESULT fail reenter_pump_fail why=%s streak=%d "
                                         "round=%d — soft cycle or release hold",
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
                            // 图内须挂台（curFh≠0）再 RESULT；悬空=掉落/热重载循环（ec1fe7）。
                            if (play.inMap && play.curFh == 0) {
                                awaitingStand = true;
                                sawInMapWhileDone = true;  // 热重载 !inMap 时勿 Done 再启
                                const DWORD now = GetTickCount();
                                if (!standSinceMs) {
                                    standSinceMs = now;
                                    LogLine("play-ready wait stand curFh=0 inMap=1 — up to %ums "
                                            "(heli/void land)",
                                            static_cast<unsigned>(kStandReadyWaitMs));
                                    KickLogLine("play-ready wait_stand curFh=0");
                                } else if (now - standSinceMs >= kStandReadyWaitMs) {
                                    LogLine("stand_wait timeout curFh=0 after %ums — degrade "
                                            "success (no soft_cycle ConnectLogin)",
                                            static_cast<unsigned>(now - standSinceMs));
                                    KickLogLine("RESULT success degrade_stand_timeout");
                                    playOk = true;
                                    LogLine("degrade stand — light post-dismiss (curFh=0)");
                                    DismissCtx post{};
                                    post.aggressive = 1;
                                    post.scanBase = 0;
                                    if (SoftPumpCall(&DismissKickDialogOnPump, &post,
                                                     kDismissCallMs)) {
                                        LogLine("post_dismiss %s", post.detail);
                                        if (post.closed > 0 || post.inactivated > 0)
                                            KickLogLine("post_dismiss %s", post.detail);
                                    }
                                    x::runtime::managed_main::SetLoginFreeze(false);
                                    char ok[240]{};
                                    snprintf(ok, sizeof(ok),
                                             "RESULT success degrade stand_timeout curFh=0 "
                                             "why=%s reenter_rounds=%d soft_cycle=%d/%d",
                                             gWhy, r + 1, softCycle, kSoftCycleMax);
                                    ArmLandQuiet(kSoftLandQuietMs);
                                    Finish(1, ok);
                                    break;
                                }
                                if ((r % 10) == 0 && standSinceMs) {
                                    LogLine("reenter wait[%d] playReady=1 inMap=1 curFh=0 "
                                            "stand=%ums/%ums soft_cycle=%d",
                                            r,
                                            static_cast<unsigned>(now - standSinceMs),
                                            static_cast<unsigned>(kStandReadyWaitMs), softCycle);
                                }
                                Sleep(kReenterPollMs);
                                if (gStop.load()) break;
                                continue;
                            }

                            playOk = true;
                            awaitingStand = false;
                            // 进图后禁止 FindAll(UIDialog 基类)；仍允许 Util/Ex 轻关，
                            // 收大厅漏网的断线「確認」窗（69c8f9 残屏）。UIMiniMap 不在 Util 树。
                            LogLine("play-ready — light post-dismiss (inMap=%d curFh=%u)",
                                    play.inMap, static_cast<unsigned>(play.curFh));
                            KickLogLine("play-ready post_dismiss_light inMap=%d curFh=%u",
                                        play.inMap, static_cast<unsigned>(play.curFh));
                            DismissCtx post{};
                            post.aggressive = 1;
                            post.scanBase = 0;  // 仅 Util/Ex，不扫 Notice 白名单/基类
                            if (SoftPumpCall(&DismissKickDialogOnPump, &post, kDismissCallMs)) {
                                LogLine("post_dismiss %s", post.detail);
                                if (post.closed > 0 || post.inactivated > 0)
                                    KickLogLine("post_dismiss %s", post.detail);
                            }
                            x::runtime::managed_main::SetLoginFreeze(false);
                            if (gStop.load()) {
                                Finish(2, "abort: stop during post_dismiss");
                                earlyFail = true;
                                break;
                            }
                            char ok[240]{};
                            snprintf(ok, sizeof(ok),
                                     "RESULT success play-ready after soft reenter why=%s "
                                     "reenter_rounds=%d soft_cycle=%d/%d curFh=%u",
                                     gWhy, r + 1, softCycle, kSoftCycleMax,
                                     static_cast<unsigned>(play.curFh));
                            KickLogLine("RESULT success play-ready rounds=%d soft_cycle=%d "
                                        "curFh=%u",
                                        r + 1, softCycle, static_cast<unsigned>(play.curFh));
                            ArmLandQuiet(kSoftLandQuietMs);
                            Finish(1, ok);
                            break;
                        }

                        // Done 却迟迟不进图：仅确认仍在大厅 (!inMap) 才 RequestRestart。
                        // 已 inMap 而 !ready / 泵曾堵：再启会冻主线程（E216）。
                        // awaitingStand 期间同图热重载会短暂 !inMap：禁止当「未进图」再启。
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
                                if (awaitingStand) {
                                    // 热重载 / Bootstrap：保持等挂台，勿 RequestRestart。
                                    doneSinceMs = 0;
                                    if ((r % 10) == 0) {
                                        LogLine("reenter wait[%d] stand_wait map_transit "
                                                "!inMap — no Done restart",
                                                r);
                                    }
                                } else if (!doneSinceMs) {
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
                            if (!awaitingStand) sawInMapWhileDone = false;
                        }

                        if ((r % 10) == 0) {
                            LogLine("reenter wait[%d] playReady=0 inMap=%d curFh=%u wall=%ums/%ums "
                                    "soft_cycle=%d done_restarts=%d",
                                    r, play.inMap, static_cast<unsigned>(play.curFh),
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
                            // 同 play-ready：仅 Util/Ex 轻关，禁基类 FindAll。
                            LogLine("degrade — light post-dismiss (Done+inMap)");
                            KickLogLine("degrade post_dismiss_light");
                            {
                                DismissCtx post{};
                                post.aggressive = 1;
                                post.scanBase = 0;
                                if (SoftPumpCall(&DismissKickDialogOnPump, &post, kDismissCallMs)) {
                                    LogLine("degrade_post_dismiss %s", post.detail);
                                    if (post.closed > 0 || post.inactivated > 0)
                                        KickLogLine("degrade_post_dismiss %s", post.detail);
                                }
                            }
                            char ok[240]{};
                            snprintf(ok, sizeof(ok),
                                     "RESULT success degrade Done+inMap timeout playReady=0 "
                                     "why=%s soft_cycle=%d/%d",
                                     gWhy, softCycle, kSoftCycleMax);
                            ArmLandQuiet(kSoftLandQuietMs);
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

bool IsLandQuiet() {
    DWORD until = gLandQuietUntilMs.load(std::memory_order_acquire);
    if (!until) return false;
    const DWORD now = GetTickCount();
    if (static_cast<int>(until - now) > 0) return true;
    (void)gLandQuietUntilMs.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
    return false;
}

bool IsPostSoftAirCombatBlocked() {
    DWORD until = gPostSoftAirUntilMs.load(std::memory_order_acquire);
    if (!until) return false;
    const DWORD now = GetTickCount();
    if (static_cast<int>(until - now) > 0) return true;
    (void)gPostSoftAirUntilMs.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
    return false;
}

bool IsGameplayQuiet() { return IsHoldActive() || IsLandQuiet(); }

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
    EnsureNoticeAbsHook();  // 断线 Notice 往往立刻直 call；须在弹窗前装好 Abs
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
