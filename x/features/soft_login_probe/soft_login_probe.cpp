// Classic TWMS — soft login ConnectLogin try-connect probe (default OFF).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "soft_login_probe.h"

#include "../auto_enter/auto_enter.h"
#include "../channel_hop/channel_hop.h"
#include "../galaxy_token_probe/galaxy_token_probe.h"
#include "../kick_sniff/kick_sniff.h"
#include "../notify/notify.h"
#include "../simple_combat/simple_combat.h"
#include "../travel/travel.h"
#include "../ports/fly_fh_ban.h"
#include "../ports/foothold_port.h"
#include "../ports/mob_gather_port.h"
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

// SceneLogin public void() — starts ConnectLoginServer IEnumerator via StartCoroutine.
constexpr uint32_t kRvaSceneLoginGet = 0xC1B4F0;
constexpr uint32_t kRvaConnectLoginStart = 0xC1C400;
constexpr char kHashSceneLoginGet[] =
    "f6435272a9a666f019a810a4c597917b4d49d1b079bcd6026e97c23a027165c";
constexpr char kHashConnectLoginStart[] =
    "af853e3f8aac062296eaca644a801b3c7f1cb60efa604bf8398674598675da7";

// Session.Disconnect / CloseSession（与 kick_sniff 同口径）。
// BIN 01:53：Disconnect 只把 Connected→Connecting→Connected，WorldItems 仍空（书页大厅）。
// 空大厅改 CloseSession 硬拆，再等 Disconnected 后才 ConnectLogin。
constexpr uint32_t kRvaNmDisconnect = 0x1CEEB10;
constexpr uint32_t kRvaNmCloseSession = 0x1CFDCB0;
constexpr char kHashNmDisconnect[] =
    "e595f66e338bcffb24b9b99d1e3a8ad1a62b51804206c84cbccb1dcde3a64ee";
constexpr char kHashNmCloseSession[] =
    "dbba63c2fbb6391b5560399468aa256c9e870f8e0f2f2c71642c0c1920b70eb";

// settle 用墙钟截止（见 Worker）：Call 耗时曾未计入 waited，实机 1500ms 常被拉成 2.5–3s+。
// Notice 多在断线瞬间弹出；Connecting 早退即可，不必死等满窗。
constexpr DWORD kSettleMs = 600;
// 主动 CloseSession（hangup_timer）：BIN 05:12 Notice 在 dismiss 后 +132ms 才关到；
// 满 600ms 是空等。关到窗即走，空枪最多再等这么久吃晚到的窗。踢线仍用 kSettleMs。
constexpr DWORD kSettleProactiveMs = 200;
constexpr DWORD kPollMs = 400;
// BIN 04:20：Disconnected 空耗满 40 轮≈16s 才 soft cycle；总 cap 须盖住 empty-hall 等待窗。
constexpr int kPollRounds = 36;  // ≥ kEmptyHallPollRounds；~14s @ kPollMs
// Connected items=0：先等多等列表（BIN 书页大厅会晚刷）。
// BUILD118 后曾满 8 轮(~3.2s) 就 CloseSession → 登录会话作废 →「已登出登入的帳號」+ avatars=0。
// 现策略：只等 / soft cycle，**禁止**为 empty hall 硬拆（见 kAllowEmptyHallCloseSession）。
constexpr int kEmptyHallPollRounds = 30;  // ~12s @ kPollMs
// connect-wait 内 Connected+items=0：约 10s 仍空再 soft cycle（勿 CloseSession）。
constexpr int kEmptyHallConnectWaitRounds = 40;  // ~10s @ kConnectWaitMs
// BIN 04:20：ConnectLogin 后变 Disconnected，仍空等到 poll 末尾；满 4 轮(~1.6s+) 即 soft cycle。
constexpr int kDiscPollFailRounds = 4;
constexpr DWORD kWaitDiscAfterCloseMs = 4000;
constexpr int kEmptyHallSoftCycleMax = 3;  // 连续空大厅满额放 hold → 守护干净重拉
constexpr int kCharSelectStuckMax = 2;  // avatars>0 超时两次 → CloseSession；avatars=0 直接 Finish(2)
// skip/GoWorld 前：活 Notice 再探一次；仍在则禁止进世界（BIN 22:47:45 noticeKinds=1 → avatars=0）。
constexpr DWORD kDirtyHallNoticeRecheckMs = 250;
// 选角页连续空头像（busy 已刷完仍 0）→ 已登出；冷启 3s 内会刷出头像，满 5s 才交守护。
constexpr DWORD kEmptyAvatarFailMs = 5000;
// 空大厅 CloseSession：默认关。仅调试「真·书页死锁且确认非登录竞态」时可临时开。
constexpr bool kAllowEmptyHallCloseSession = false;
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
// BIN 04:20：cycle1 失败后还 Sleep 1200 才 cycle2；压到 400。
constexpr DWORD kSoftCycleRetryGapMs = 400;
// 重进等待：playReady 采样宜短；泵堵时 1500ms 等满只会叠 job timeout（E216）。
constexpr DWORD kPlayReadySampleMs = 400;
// 已 playReady+inMap 但 curFh=0（悬空/掉落，ec1fe7 heli 半空软重进）：再等挂台再 RESULT。
// 空中贴怪本来就不挂台：等满 15s 才弹「软重连成功」，人早已在打（BIN 7F43 17:01:58→17:02:14）。
// 覆盖同图热重载数轮（约 3s/次）；满窗降级成功，勿再 ConnectLogin soft cycle。
constexpr DWORD kStandReadyWaitMs = 15000;
// 等挂台时又离场（BIN 6c3ef8：inMap curFh=0 → Disconnected → 大厅）：2s 内清
// awaitingStand，允许 Done 再启。旧逻辑把「闪进图」锁成 stand_wait 直到 150s 谎报成功。
constexpr DWORD kStandLeftMapAbortMs = 2000;
// 泵心跳：与 MainPump::IsPumpTicking 默认 1500ms 对齐。断线 InterStage 常短暂 idle
// （B9B 08:32：~2.5s idle 后游戏自连）；先等活再动，勿立刻放 hold 交守护。
constexpr DWORD kPumpAliveMaxAgeMs = 1500;
// SoftPumpCall 新鲜度：对齐泵 quiesce idle 闸（kTransitIdleFailMs=400）略严。
// 仍用 1500 判「可 Call」会在 Bootstrap 半死时空打 High Sample/Dismiss（B9B 9865c3）。
constexpr DWORD kSoftPumpFreshMs = 300;
// 空闲自愈：轮询断线窗是否还在（不依赖 WM Login / Disconnected 边沿）。
constexpr DWORD kDlgProbeGapMs = 1500;
constexpr DWORD kDlgLingerHealMs = 2000;
constexpr DWORD kSoftSampleCallMs = 400;  // 对齐 kTransitInvokeCapMs；勿再等满 1500
// 成功进图后玩法错峰：Finish 放 hold，但仍压 Combat/Invuln 急钉，避免 q=8 齐开（B9B）。
// ce6797：quiet 一结束立刻 BAN ON + |v|~7k Impact → 再软断。
// land quiet = 整段停刀/停旋翼；post_air_gate ≥ quiet，防 quiet 被提前清掉后仍空中开打。
// RESULT 本身已等 curFh≠0；主动软重连落地后再套 300–600ms 是空等。
// 挂台：Arm 时直接 skip；未挂台才走 cap，PeekCurFh 一到即早释（wait_onFh 仍挡没台）。
constexpr DWORD kSoftLandQuietMs = 250;
constexpr DWORD kSoftPostAirGateMs = 400;
// Connected 后等分区列表刷出再 RequestRestart（BIN 21:44：壳指针先到、WorldItems=0）。
// 空列表靠加长等待 + soft cycle；勿 CloseSession 刷新（会登出登录会话）。
constexpr DWORD kHallReadyWaitMs = 12000;
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
// 中间态（limbo）判死。BIN 21:11 外机：换频确认在途 52s 无回音 → 泵进 InterStage 再没
// 出来；SceneLogin 已销毁、MapScene 没建起来 —— 客户端卡在登录与地图之间，
// ConnectLogin 的入口根本不存在：1130 轮采样全 sl_null，32 分钟零恢复。
//
// 判据 = SceneLogin 不存在 + 会话既非 Connected 也非 Connecting（游戏自己也没在自连）。
// **不得**拿 nmOk 当判据：ResolveNmFacadeOnPump 只取 statics 里第一个像堆指针的槽，
// 不校验 session/state，塌成中间态时照样返回非空（kick_sniff 的 gNmCached 带校验才为 0）。
// **不得**拿「泵在 quiesce」当判据：它在登录大厅期恒真（见 SoftShouldDeferPumpWork）。
// 逃逸口有两个 —— 状态转 Connecting/Connected，或 SceneLogin 重建；正常「进图后断线 SL
// 晚建」因此不会被误判。
//
// 必须早退：烧满 40s×10 周期使失败间隔达 554s > 熔断窗 kSoftFailBreakerWindowMs(300s)，
// gFailStreak 每次都被重置成 1（实测 3 次全是 fail_streak 1/3，从未 breaker ON），
// 守护于是永远等不到干净重拉窗口 —— 这才是「一直提示软重连失败」的成因。
constexpr int kLimboConnectRounds = 72;  // ~25s 实测轮距 347ms；健康自连 2s 内就转 Connecting
constexpr int kLimboSoftCycleMax = 2;    // 连续 limbo 周期满额放 hold → 守护干净重拉
// im<0（图内读不出）时回退 8 轮再重探，故轮数上限必须留得下这个回退量。
static_assert(kLimboConnectRounds > 8, "limbo backoff would underflow");
static_assert(kLimboConnectRounds < kConnectWaitRounds, "limbo must fire before connect-wait ends");
// BIN 04:20：Connecting 空耗 try=1→20 才 ConnectLogin；满 ~3s 改去 poll（自连优先，勿叠登）。
constexpr int kConnectingStallRounds = 12;

constexpr int kStateDisconnecting = 0;
constexpr int kStateDisconnected = 1;
constexpr int kStateConnecting = 2;
constexpr int kStateConnected = 3;

// SceneLogin 登录 UI 槽（与 auto_enter / il2cpp_shape 同口径；软登录只读判定用）
constexpr size_t kOffSlChannelUi = 0xC0;
constexpr size_t kOffSlWorldUi = 0xC8;

// UIUtilDialog（非 Ex）— 与 worldmap_marker_travel 同源
constexpr char kUtilDialogClass[] =
    "cabf3fe9cc1437a22ff14cae558ff4ccdc75c0b90a22311eaa71c8921615d15";
// UIDialog 基类（仅解析 Close；禁止 FindAll 基类——子树含 UIMiniMap 等 HUD）
constexpr char kUiDialogClass[] =
    "b898158ab45f364f4f30bd141a65763a8692ec18521c02e189e0242ebd4159b";
// UIMiniMap : UIDialog — 纵深防护（白名单路径本不应扫到）
constexpr char kMiniMapClass[] =
    "b962d74817f8c2df12e0a78f62cade89bb8267001f1f6d592c2e20e68bcfd52";
// scanBase 白名单：断线/踢线 Notice 族（显式 FindAll 各类，永不扫 UIDialog 基类）
constexpr char kNoticeDialogClass[] =
    "e385b1cd935707b8b784f9457030ac4ea934d49b9e0140aae81be6a2fe06cb4";
constexpr char kLoginUtilDialogClass[] =
    "fbc2ce7959249426f2fd70380894288901b68f5af2a7b2d484f71cce302e9bc";
constexpr char kSlideNoticeClass[] =
    "a5d356032766ccd207d8c143839a520c97cda604f765f7acaac7ec49219b76f";
constexpr char kMultiLineNoticeClass[] =
    "ab985ec23eda60fea535ae10c5a1328f1f845ce9e2e25c33de2e56d032f2b77";
constexpr char kAntiMacroNoticeClass[] =
    "eb0cd232582626989bee2a787691610ab0e6e16e88fcd30bdf39b36794d1577";
// UIUtilDialogEx — 与 shop_port 同源
constexpr char kUtilDialogExClass[] =
    "c0e2575bfaabf8fa25bee32fa3d0b6972a771b99104acbf9f0c98590c225be3";
// 官方关窗（CMS CloseDialog→UIDialog.Close）；不走 OnClickYes/Ok，避免踢线「確認」
constexpr uint32_t kRvaCloseDialog = 0x789400;   // UIUtilDialog.CloseDialog
constexpr uint32_t kRvaUiDialogClose = 0x14BABD0;  // UIDialog.Close（shop_port 同源）
constexpr char kHashCloseDialog[] =
    "a0030285f34cb3d07587574f1ec3f560c795231c8aa987cefed79dc03a9258f";
// UIUtilDialog.Notice — Abs trampoline 会 PATCH GA .text（NGS/GRAP 忌讳）。
// BIN 16:xx：装 Abs 后反复「安全模組…強制關閉」；封禁/踢线文案改走 DialogScrape。
// 开启：截 sMsg + 返回实例；dismiss 仍以 FindAll 为主。
constexpr bool kNoticeAbsEnabled = false;
constexpr uint32_t kRvaNotice = 0x75B6A0;
// 序言：push r15/r14/r12/rsi/rdi/rbp/rbx ; sub rsp,80h（17B，指令边界；IDA 运行时 dump）
constexpr size_t kNoticeSteal = 17;
constexpr uint8_t kNoticeSig[kNoticeSteal] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57,
                                              0x55, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00,
                                              0x00};
// YesNo — 同表邻接；封禁单钮窗也可能不经 Notice
constexpr uint32_t kRvaYesNo = 0x757610;
constexpr size_t kYesNoSteal = 19;
constexpr uint8_t kYesNoSig[kYesNoSteal] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
                                            0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x88,
                                            0x00, 0x00, 0x00};
// UIUtilDialog 表邻接备用 Open（少 xref；登录封禁路径候选，callee of login packet handler）
constexpr uint32_t kRvaAltDlgOpen = 0x790410;
constexpr size_t kAltDlgSteal = 14;
constexpr uint8_t kAltDlgSig[kAltDlgSteal] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
                                              0x81, 0xEC, 0x90, 0x00, 0x00, 0x00};
constexpr uint32_t kRvaCompGetGo = x::runtime::il2cpp::kRvaCompGetGo;
constexpr uint32_t kRvaGoSetActive = 0x4E97EE0;
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E98080;
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
using FnYesNo = void* (*)(void* sMsg, void* yes, void* no, void* sSnd, uint8_t a, uint8_t b,
                          uint8_t c, uint8_t d, const void* method);
using FnAltDlgOpen = void* (*)(void* a1, void* a2, void* a3, void* a4, uint8_t a5, uint8_t a6,
                               uint8_t a7, const void* method);

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
std::atomic<DWORD> gLandQuietArmedAtMs{0};   // Arm 墙钟；早释算 elapsed
std::atomic<unsigned> gResult{0};  // 0 none 1 ok 2 fail
std::atomic<bool> gUiEnabled{false};
char gWhy[64]{};
// 图内 CloseSession 粘性武装（BIN 17:08：踢线无 Disconnected / lost_session 被吞 → 永不 attempt）
std::atomic<bool> gDeferred{false};
std::atomic<DWORD> gDeferredSinceMs{0};
char gDeferredWhy[64]{};
constexpr DWORD kDeferredSoftMaxMs = 120000;

// 「本轮 why 需要真重连」的判定量。gWhy 是裸 char[64]，RequestAttempt 先 memset 再 strncpy_s，
// 读侧撞上中间态会拿到空串 → 判成「非真断线」→ 又放开假 already_in_map（BIN 17:29 误杀）。
// 判定只读这个原子量；gWhy 退化为纯日志用途。
std::atomic<bool> gWhyNeedsReconnect{false};
// RequestProactiveReconnect 置位；KickSniff 会把 gWhy 改成 disconnected，settle 用这个认主动关窗。
std::atomic<bool> gSettleProactiveFast{false};

// attempt 起点（GetTickCount，0=无）；Finish 用它兑现最小 hold 时长。
std::atomic<DWORD> gAttemptBeginMs{0};
// 守护 Status SHM 每 500ms 才发布一拍。attempt 若整段落在同一拍内，
// 「result 0→1 上升沿」与「hold=1」会一起被跳过，守护只看到「seq 涨了且 hold=0」
// → 判服务器踢线 → 干净重拉杀进程（BIN 17:29）。成功后把 hold 多按住几拍。
constexpr DWORD kMinHoldMs = 2000;

// 图内确认恢复：真断线 why 下 MapScene 残留不许算成功（否则 hold 只闪十余 ms），
// 但「playReady + 挂台 + NM Connected」连续保持这么久就是游戏自己恢复了（假断线边沿）。
// 没有这个出口时 connect-wait 会空耗 160 轮 × 10 cycle ≈ 十分钟 hold（全程停打怪），
// 最后 Finish(2) 反而让守护把好端端在图里的客户端杀掉。
// BIN 8cab31：8s 确认叠在 12s 空大厅等待上，角色早已挂台 curFh=47 才弹「软重连成功」。
// 守护误杀根因是 hold 只闪 13ms（kMinHoldMs=2s 已盖住），不是需要再盯 8s。
constexpr DWORD kInMapRecoverConfirmMs = 2000;
// 真断线 + MapScene 残留：必须先出图再 ConnectLogin / 进大厅。BIN 00:14:59
// curFh=307 + Disconnected 立刻 login_ui_ready → 叠登「已登出登入的帳號」+ avatars=0。
constexpr DWORD kLeaveMapBeforeConnectMs = 10000;
// 热重载 / 选角会闪一拍 !inMap；立刻清钟会让 cycle_begin 刚起的确认被下一轮重置，
// 再空耗 kHallReadyWaitMs（BIN 8cab31：04:43:01 起表 → 04:43:09 !inMap 清钟 → 12s hall）。
constexpr DWORD kInMapRecoverDropGraceMs = 500;
std::atomic<DWORD> gInMapRecoverSinceMs{0};
std::atomic<DWORD> gInMapRecoverDropSinceMs{0};

// 连续软失败熔断：hold 期间守护的一切干净重拉都被丢弃（hangup_schedule softHoldBlocksRelaunch），
// 而 result=2 只有上升沿有效 —— 失败后立刻再 hold 可以无限架空守护。
// 窗口内连续失败满额则暂停接管，让守护干净重拉一次。
constexpr int kSoftFailBreakerMax = 3;
constexpr DWORD kSoftFailBreakerWindowMs = 300000;
constexpr DWORD kSoftFailBreakerHoldOffMs = 300000;
std::atomic<int> gFailStreak{0};
std::atomic<DWORD> gFailStreakFirstMs{0};
std::atomic<DWORD> gBreakerUntilMs{0};

AbsHook gNoticeAbs{};
AbsHook gYesNoAbs{};
AbsHook gAltDlgAbs{};
std::atomic<void*> gLastNoticeDlg{nullptr};   // Abs hook 截到的最近 Notice 返回值
std::atomic<unsigned> gNoticeAbsHits{0};      // 进程内累计捕获次数
std::atomic<int> gNoticeAbsInstall{0};        // 0=未试 1=ok 2=fail
std::atomic<int> gYesNoAbsInstall{0};
std::atomic<int> gAltDlgAbsInstall{0};
std::atomic<unsigned> gDialogScrapeSeq{0};
char gLastScrapeMsg[480]{};
DWORD gLastScrapeLogMs = 0;
// 泵上 FindAll 到的活窗；泵卡死（封禁弹窗）后 worker 纯内存深扫。
constexpr int kDlgCacheCap = 12;
std::atomic<uintptr_t> gDlgCache[kDlgCacheCap]{};
std::atomic<int> gDlgCacheRr{0};

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
    int noticeKinds = 0;  // 本枪扫到的 Notice 白名单类（settle 的 1 是断线窗，skip 时仍 1 才脏）
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
    const std::wstring dev = x::runtime::OptionalRepoRuntimeDumpDir();
    if (!dev.empty()) return dev;
    std::wstring dir = ModuleDir();
    if (!dir.empty()) {
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

// IsArmed 会被 MethodInfo hook 在 Unity 主线程上（Nm.CloseSession 里）调到；
// 每次实测要跑 ResolveLogDir（含 CreateDirectoryW）+ 两次 GetFileAttributesW。
// 断线瞬间这就是主线程上的同步磁盘 I/O，缓存一小段窗口（marker 最多晚这么久生效）。
constexpr DWORD kMarkerTtlMs = 2000;
std::atomic<DWORD> gMarkerCheckMs{0};
std::atomic<int> gMarkerCached{-1};

bool MarkerArmedUncached() {
    const std::wstring logDir = ResolveLogDir();
    const std::wstring modDir = ModuleDir();
    return (!logDir.empty() && FileExists(logDir + L"\\" + kMarkerName)) ||
           (!modDir.empty() && FileExists(modDir + L"\\" + kMarkerName));
}

bool MarkerArmed() {
    const DWORD now = GetTickCount();
    const DWORD last = gMarkerCheckMs.load(std::memory_order_acquire);
    const int cached = gMarkerCached.load(std::memory_order_acquire);
    if (cached >= 0 && last != 0 && now - last < kMarkerTtlMs) return cached != 0;
    const bool armed = MarkerArmedUncached();
    gMarkerCached.store(armed ? 1 : 0, std::memory_order_release);
    gMarkerCheckMs.store(now ? now : 1, std::memory_order_release);
    return armed;
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

bool WaitNmNotConnected(DWORD waitMs, const char* tag);
bool WaitPumpAlive(DWORD waitMs, const char* tag);

// true=已发起硬拆（或 session 已空）；调用方清 invokeOk，等 Disconnected 再 ConnectLogin。
// 已进图 / 泵不新鲜：拒绝硬拆（返回 false），由 SoftProbeInMapOrFinish 解 hold。
// 空大厅 why：默认拒绝（kAllowEmptyHallCloseSession=false）——硬拆会「已登出登入的帳號」。
bool SoftForceNmTeardown(const char* why) {
    if (!kAllowEmptyHallCloseSession && why &&
        (std::strstr(why, "empty_hall") || std::strstr(why, "hall_wait"))) {
        LogLine("force_nm_close refuse empty_hall_policy why=%s", why);
        KickLogLine("force_nm_close refuse_empty_hall why=%s", why ? why : "?");
        return false;
    }
    if (x::features::ports::world::IsInMapScene()) {
        LogLine("force_nm_close refuse inMap why=%s", why ? why : "?");
        KickLogLine("force_nm_close refuse_inMap");
        return false;
    }
    if (SoftShouldDeferPumpWork()) {
        if (!WaitPumpAlive(kPumpWaitConnectMs, "force_nm_close")) {
            LogLine("force_nm_close defer pump_dead why=%s", why ? why : "?");
            KickLogLine("force_nm_close defer");
            return false;
        }
        if (x::features::ports::world::IsInMapScene()) {
            LogLine("force_nm_close refuse inMap after_wait why=%s", why ? why : "?");
            KickLogLine("force_nm_close refuse_inMap");
            return false;
        }
    }
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

// 图内主动拆：SoftForceNmTeardown 会 refuse inMap。一般路径只允许 Field+playReady。
// hangup_fires / hangup_timer：图内 play-ready 即拆（不要求 scene 仍是 Field）。
bool WhyIsHangupClose(const char* why) {
    return why && (std::strcmp(why, "hangup_fires") == 0 ||
                   std::strcmp(why, "hangup_timer") == 0);
}

bool SoftCloseSessionInField(const char* why) {
    using x::features::ports::world::GetSceneState;
    using x::features::ports::world::IsInMapScene;
    using x::features::ports::world::IsPlayReady;
    using x::features::ports::world::SceneState;
    const bool hangupClose = WhyIsHangupClose(why);
    const bool sceneOk = hangupClose
                             ? (IsInMapScene() && IsPlayReady())
                             : (GetSceneState() == SceneState::Field && IsPlayReady());
    if (!sceneOk) {
        LogLine("close_in_field refuse scene=%d inMap=%d play=%d why=%s",
                static_cast<int>(GetSceneState()), IsInMapScene() ? 1 : 0,
                IsPlayReady() ? 1 : 0, why ? why : "?");
        KickLogLine("close_in_field refuse");
        return false;
    }
    PumpCtx ctx{};
    if (!SoftPumpCall(&DoNmCloseSessionOnPump, &ctx, 3000)) {
        LogLine("close_in_field Call fail/timeout why=%s", why ? why : "?");
        KickLogLine("close_in_field pump_fail");
        return false;
    }
    LogLine("close_in_field ok=%d detail=%s why=%s", ctx.ok, ctx.detail, why ? why : "?");
    KickLogLine("close_in_field ok=%d why=%s", ctx.ok, why ? why : "?");
    return ctx.ok != 0 || std::strcmp(ctx.detail, "session_null") == 0 ||
           std::strcmp(ctx.detail, "nm_null") == 0;
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

// Il2CppString：length@0x10 chars@0x14（UTF-16）。纯内存读，可在 Notice 入口线程用。
bool ReadNoticeMsgUtf8(void* str, char* out, size_t outCap) {
    if (!str || !out || outCap < 2) return false;
    __try {
        const int32_t len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 512) return false;
        const auto* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        const int n =
            WideCharToMultiByte(CP_UTF8, 0, chars, len, out, static_cast<int>(outCap) - 1, nullptr,
                                nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool NoticeMsgLooksKickish(const char* msg) {
    if (!msg || !msg[0]) return false;
    // UTF-8 片段：登出 / 斷線|断线 / 踢 / 帳|賬
    static const char* kNeedles[] = {
        "\xE7\x99\xBB\xE5\x87\xBA",              // 登出
        "\xE6\x96\xB7\xE7\xB7\x9A",              // 斷線
        "\xE6\x96\xAD\xE7\xBA\xBF",              // 断线
        "\xE8\xB8\xA2",                          // 踢
        "\xE5\xB8\xB3\xE8\x99\x9F",              // 帳號
        "\xE8\xB3\x87\xE8\x99\x9F",              // 賬號
        "\xE7\x87\x9F\xE9\x81\x8B",              // 營運
        "\xE9\x99\x90\xE5\x88\xB6",              // 限制
        "\xE9\x81\x95\xE5\x8F\x8D",              // 違反
        "\xE6\x94\xBF\xE7\xAD\x96",              // 政策
        "logout",
        "kick",
        "disconnect",
        "ban",
    };
    char low[520]{};
    for (size_t i = 0; msg[i] && i + 1 < sizeof(low); ++i) {
        const unsigned char c = static_cast<unsigned char>(msg[i]);
        low[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : msg[i];
    }
    for (const char* n : kNeedles) {
        if (strstr(low, n)) return true;
    }
    return false;
}

// 自愈只认断线/登出窗，不把营运公告 / 测谎 Notice 当踢线。
bool NoticeMsgLooksDisconnectDlg(const char* msg) {
    if (!msg || !msg[0]) return false;
    static const char* kNeedles[] = {
        "\xE6\x96\xB7\xE7\xB7\x9A",              // 斷線
        "\xE6\x96\xAD\xE7\xBA\xBF",              // 断线
        "\xE5\xB7\xB2\xE4\xB8\xAD\xE6\x96\xB7",  // 已中斷
        "\xE5\xB7\xB2\xE4\xB8\xAD\xE6\x96\xAD",  // 已中断
        "\xE7\x99\xBB\xE5\x87\xBA",              // 登出
        "logout",
        "disconnect",
    };
    char low[520]{};
    for (size_t i = 0; msg[i] && i + 1 < sizeof(low); ++i) {
        const unsigned char c = static_cast<unsigned char>(msg[i]);
        low[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : msg[i];
    }
    for (const char* n : kNeedles) {
        if (strstr(low, n)) return true;
    }
    return false;
}

bool DlgHasDisconnectText(void* dlg) {
    if (!dlg || !LooksLikeHeapPtr(dlg)) return false;
    auto scan = [](void* obj, size_t lo, size_t hi) -> bool {
        if (!obj || !LooksLikeHeapPtr(obj)) return false;
        for (size_t off = lo; off < hi; off += sizeof(void*)) {
            void* p = ReadPtr(obj, off);
            if (!p || !LooksLikeHeapPtr(p)) continue;
            char buf[480]{};
            if (!ReadNoticeMsgUtf8(p, buf, sizeof(buf)) || !buf[0]) continue;
            if (NoticeMsgLooksDisconnectDlg(buf)) return true;
        }
        return false;
    };
    if (scan(dlg, 0x18, 0x280)) return true;
    int fan = 0;
    for (size_t off = 0x18; off < 0x280 && fan < 12; off += sizeof(void*)) {
        void* child = ReadPtr(dlg, off);
        if (!child || !LooksLikeHeapPtr(child) || child == dlg) continue;
        char probe[8]{};
        if (ReadNoticeMsgUtf8(child, probe, sizeof(probe))) continue;
        ++fan;
        if (scan(child, 0x10, 0x120)) return true;
    }
    return false;
}

void LogCapturedDialog(const char* api, unsigned hit, void* dlg, const char* msg, const char* sub,
                       int hold) {
    const int kickish = NoticeMsgLooksKickish(msg) || NoticeMsgLooksKickish(sub) ? 1 : 0;
    if (dlg && (hold || kickish)) {
        gLastNoticeDlg.store(dlg, std::memory_order_release);
    }
    LogLine("%s Abs hit=%u dlg=%p hold=%d kickish=%d msg=%s sub=%s", api, hit, dlg, hold, kickish,
            msg && msg[0] ? msg : "(unreadable)", sub && sub[0] ? sub : "(none)");
    if (kickish || hold) {
        KickLogLine("%s Abs hit=%u kickish=%d hold=%d msg=%s sub=%s", api, hit, kickish, hold,
                    msg && msg[0] ? msg : "?", sub && sub[0] ? sub : "?");
    }
}

void* __fastcall HookNoticeAbs(void* sMsg, void* sSub, uint8_t a, uint8_t b, uint8_t c, void* ok,
                               const void* method) {
    (void)a;
    (void)b;
    (void)c;
    auto* orig = reinterpret_cast<FnNotice>(gNoticeAbs.trampoline);
    void* dlg = nullptr;
    if (orig) {
        __try {
            dlg = orig(sMsg, sSub, a, b, c, ok, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            dlg = nullptr;
        }
    }
    const unsigned hit = gNoticeAbsHits.fetch_add(1, std::memory_order_relaxed) + 1;
    char msg[480]{};
    char sub[240]{};
    (void)ReadNoticeMsgUtf8(sMsg, msg, sizeof(msg));
    (void)ReadNoticeMsgUtf8(sSub, sub, sizeof(sub));
    LogCapturedDialog("Notice", hit, dlg, msg, sub,
                      gHold.load(std::memory_order_acquire) ? 1 : 0);
    return dlg;
}

void* __fastcall HookYesNoAbs(void* sMsg, void* yes, void* no, void* sSnd, uint8_t a, uint8_t b,
                              uint8_t c, uint8_t d, const void* method) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    auto* orig = reinterpret_cast<FnYesNo>(gYesNoAbs.trampoline);
    void* dlg = nullptr;
    if (orig) {
        __try {
            dlg = orig(sMsg, yes, no, sSnd, a, b, c, d, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            dlg = nullptr;
        }
    }
    static std::atomic<unsigned> sHits{0};
    const unsigned hit = sHits.fetch_add(1, std::memory_order_relaxed) + 1;
    char msg[480]{};
    char snd[120]{};
    (void)ReadNoticeMsgUtf8(sMsg, msg, sizeof(msg));
    (void)ReadNoticeMsgUtf8(sSnd, snd, sizeof(snd));
    LogCapturedDialog("YesNo", hit, dlg, msg, snd,
                      gHold.load(std::memory_order_acquire) ? 1 : 0);
    return dlg;
}

void* __fastcall HookAltDlgAbs(void* a1, void* a2, void* a3, void* a4, uint8_t a5, uint8_t a6,
                               uint8_t a7, const void* method) {
    (void)a5;
    (void)a6;
    (void)a7;
    auto* orig = reinterpret_cast<FnAltDlgOpen>(gAltDlgAbs.trampoline);
    void* dlg = nullptr;
    if (orig) {
        __try {
            dlg = orig(a1, a2, a3, a4, a5, a6, a7, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            dlg = nullptr;
        }
    }
    static std::atomic<unsigned> sHits{0};
    const unsigned hit = sHits.fetch_add(1, std::memory_order_relaxed) + 1;
    char msg[480]{};
    char sub[240]{};
    (void)ReadNoticeMsgUtf8(a1, msg, sizeof(msg));
    (void)ReadNoticeMsgUtf8(a2, sub, sizeof(sub));
    // 有的重载 a1=this；再试 a2/a3
    if (!msg[0]) (void)ReadNoticeMsgUtf8(a2, msg, sizeof(msg));
    if (!sub[0]) (void)ReadNoticeMsgUtf8(a3, sub, sizeof(sub));
    LogCapturedDialog("AltDlg", hit, dlg ? dlg : a1, msg, sub,
                      gHold.load(std::memory_order_acquire) ? 1 : 0);
    return dlg;
}

bool InstallAbsHook(AbsHook& hook, void* target, const uint8_t* sig, size_t steal, void* hookFn,
                    const char* tag) {
    if (hook.active) return true;
    if (!target || !sig || steal < 12 || steal > sizeof(hook.saved)) return false;
    auto* bytes = reinterpret_cast<uint8_t*>(target);
    for (size_t i = 0; i < steal; ++i) {
        if (bytes[i] != sig[i]) {
            LogLine("%s Abs refuse: sig mismatch @%p b0=%02X want=%02X", tag, target, bytes[0],
                    sig[0]);
            return false;
        }
    }
    void* tramp = VirtualAlloc(nullptr, steal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(hook.saved, target, steal);
    memcpy(tramp, target, steal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + steal,
                reinterpret_cast<uint8_t*>(target) + steal);
    DWORD old = 0;
    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hookFn);
    for (size_t i = 12; i < steal; ++i) reinterpret_cast<uint8_t*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    VirtualProtect(target, steal, old, &old);
    hook.target = target;
    hook.trampoline = tramp;
    hook.stolen = steal;
    hook.active = true;
    return true;
}

bool InstallNoticeAbs(void* target) {
    return InstallAbsHook(gNoticeAbs, target, kNoticeSig, kNoticeSteal,
                          reinterpret_cast<void*>(&HookNoticeAbs), "Notice");
}

// 进程级：Notice + YesNo + AltDlg Open；FindAll scrape 另走泵。
void EnsureNoticeAbsHook() {
    if (!kNoticeAbsEnabled) return;
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (!ga) ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) return;

    if (!gNoticeAbs.active) {
        const int st = gNoticeAbsInstall.load(std::memory_order_relaxed);
        if (st != 2) {
            void* target = reinterpret_cast<uint8_t*>(ga) + kRvaNotice;
            if (!InstallNoticeAbs(target)) {
                gNoticeAbsInstall.store(2, std::memory_order_relaxed);
                LogLine("Notice Abs install fail rva=0x%X", kRvaNotice);
            } else {
                gNoticeAbsInstall.store(1, std::memory_order_relaxed);
                LogLine("Notice Abs install ok rva=0x%X steal=%zu", kRvaNotice, kNoticeSteal);
            }
        }
    } else {
        gNoticeAbsInstall.store(1, std::memory_order_relaxed);
    }

    if (!gYesNoAbs.active) {
        const int st = gYesNoAbsInstall.load(std::memory_order_relaxed);
        if (st != 2) {
            void* target = reinterpret_cast<uint8_t*>(ga) + kRvaYesNo;
            if (!InstallAbsHook(gYesNoAbs, target, kYesNoSig, kYesNoSteal,
                                reinterpret_cast<void*>(&HookYesNoAbs), "YesNo")) {
                gYesNoAbsInstall.store(2, std::memory_order_relaxed);
                LogLine("YesNo Abs install fail rva=0x%X", kRvaYesNo);
            } else {
                gYesNoAbsInstall.store(1, std::memory_order_relaxed);
                LogLine("YesNo Abs install ok rva=0x%X steal=%zu", kRvaYesNo, kYesNoSteal);
            }
        }
    }

    if (!gAltDlgAbs.active) {
        const int st = gAltDlgAbsInstall.load(std::memory_order_relaxed);
        if (st != 2) {
            void* target = reinterpret_cast<uint8_t*>(ga) + kRvaAltDlgOpen;
            if (!InstallAbsHook(gAltDlgAbs, target, kAltDlgSig, kAltDlgSteal,
                                reinterpret_cast<void*>(&HookAltDlgAbs), "AltDlg")) {
                gAltDlgAbsInstall.store(2, std::memory_order_relaxed);
                LogLine("AltDlg Abs install fail rva=0x%X", kRvaAltDlgOpen);
            } else {
                gAltDlgAbsInstall.store(1, std::memory_order_relaxed);
                LogLine("AltDlg Abs install ok rva=0x%X steal=%zu", kRvaAltDlgOpen, kAltDlgSteal);
            }
        }
    }
}

void CacheDialogPtr(void* dlg) {
    if (!dlg || !LooksLikeHeapPtr(dlg)) return;
    const uintptr_t v = reinterpret_cast<uintptr_t>(dlg);
    for (int i = 0; i < kDlgCacheCap; ++i) {
        if (gDlgCache[i].load(std::memory_order_relaxed) == v) return;
    }
    for (int i = 0; i < kDlgCacheCap; ++i) {
        uintptr_t expect = 0;
        if (gDlgCache[i].compare_exchange_strong(expect, v, std::memory_order_relaxed)) return;
    }
    const int slot = gDlgCacheRr.fetch_add(1, std::memory_order_relaxed) % kDlgCacheCap;
    gDlgCache[slot].store(v, std::memory_order_relaxed);
}

bool LooksLikeNoiseUiString(const char* s) {
    if (!s || !s[0]) return true;
    if (strstr(s, "Sound/") || strstr(s, "sound/") || strstr(s, "Mob/") || strstr(s, "Bgm/") ||
        strstr(s, "Assets/") || strstr(s, ".prefab") || strstr(s, "UI/Atlas") ||
        strstr(s, "Effect/") || strstr(s, "Npc/") || strstr(s, "Map/") ||
        strstr(s, "Damage") || strstr(s, "/Die") || strstr(s, "Vuplex") ||
        strstr(s, "StreamingAssets") || strstr(s, "UnityEngine.") || strstr(s, "G:\\") ||
        strstr(s, "G:/") || strstr(s, "Maplestory_Classic_Data") ||
        strstr(s, "Assembly-CSharp") || strstr(s, "ResourceManagement") ||
        strstr(s, ".bundle") || strstr(s, ".pak") || strstr(s, ".dll")) {
        return true;
    }
    return false;
}

bool HasCjkUtf8(const char* s) {
    if (!s) return false;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        if (*p >= 0x80) return true;
    }
    return false;
}

// 只打踢线/封禁类；同文案 15s 内只落盘一次（防刷爆 soft/kick）。
constexpr DWORD kKickishLogGapMs = 15000;

void EmitDialogScrape(const char* tag, size_t off, const char* buf, const char* via) {
    if (!buf || !buf[0] || LooksLikeNoiseUiString(buf)) return;
    if (!NoticeMsgLooksKickish(buf)) return;
    const DWORD now = GetTickCount();
    if (gLastScrapeMsg[0] && strcmp(gLastScrapeMsg, buf) == 0 &&
        now - gLastScrapeLogMs < kKickishLogGapMs) {
        return;
    }
    strncpy_s(gLastScrapeMsg, buf, _TRUNCATE);
    gLastScrapeLogMs = now;
    const unsigned seq = gDialogScrapeSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    LogLine("DialogScrape #%u tag=%s via=%s off=0x%zX kickish=1 msg=%s", seq, tag,
            via ? via : "?", off, buf);
    KickLogLine("DialogScrape #%u tag=%s via=%s msg=%s", seq, tag, via ? via : "?", buf);
}

void ScrapeObjectStrings(void* obj, const char* tag, const char* via, size_t offLo, size_t offHi) {
    if (!obj || !LooksLikeHeapPtr(obj)) return;
    for (size_t off = offLo; off < offHi; off += sizeof(void*)) {
        void* p = ReadPtr(obj, off);
        if (!p || !LooksLikeHeapPtr(p)) continue;
        char buf[480]{};
        if (!ReadNoticeMsgUtf8(p, buf, sizeof(buf)) || !buf[0]) continue;
        EmitDialogScrape(tag, off, buf, via);
    }
}

// 一层深扫：dialog 字段 → 子对象再扫 string（TMP/Label 常挂在字段上）。
void ScrapeDlgFieldsDeep(void* dlg, const char* tag) {
    if (!dlg || !LooksLikeHeapPtr(dlg)) return;
    ScrapeObjectStrings(dlg, tag, "self", 0x18, 0x280);
    int fan = 0;
    for (size_t off = 0x18; off < 0x280 && fan < 12; off += sizeof(void*)) {
        void* child = ReadPtr(dlg, off);
        if (!child || !LooksLikeHeapPtr(child) || child == dlg) continue;
        // 跳过明显是 string 的槽（已在 self 扫过）
        char probe[8]{};
        if (ReadNoticeMsgUtf8(child, probe, sizeof(probe))) continue;
        ++fan;
        ScrapeObjectStrings(child, tag, "child", 0x10, 0x120);
    }
}

void ScrapeCachedDialogsOffPump() {
    // 进图后封禁/踢登录窗不会再弹；停扫并清缓存，避免脏指针+刷盘。
    if (x::features::ports::world::IsInMapScene()) {
        for (int i = 0; i < kDlgCacheCap; ++i) {
            if (gDlgCache[i].load(std::memory_order_relaxed))
                gDlgCache[i].store(0, std::memory_order_relaxed);
        }
        return;
    }
    for (int i = 0; i < kDlgCacheCap; ++i) {
        void* dlg = reinterpret_cast<void*>(gDlgCache[i].load(std::memory_order_relaxed));
        if (!dlg || !LooksLikeHeapPtr(dlg)) continue;
        ScrapeDlgFieldsDeep(dlg, "cached");
    }
}

void ScrapeTmpTextsOnPump(MethodInfoHead* miGetGo, MethodInfoHead* miGetActive,
                          MethodInfoHead* miGetHier) {
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.findAll) return;
    struct TmpTry {
        const char* ns;
        const char* name;
        const char* tag;
    };
    const TmpTry kTries[] = {
        {"TMPro", "TextMeshProUGUI", "TMPUGUI"},
        {"TMPro", "TextMeshPro", "TMP"},
        {"UnityEngine.UI", "Text", "UiText"},
    };
    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    for (const TmpTry& t : kTries) {
        void* klass = x::runtime::il2cpp::FindClass(t.ns, t.name);
        if (!klass) continue;
        void* typeObj = ClassTypeObjectOnPump(klass);
        if (!typeObj) continue;
        void* getTextMi = x::runtime::il2cpp_method::FindMethodByName(klass, "get_text", 0, false);
        MethodInfoHead* miText = AsMi(getTextMi);
        void* arr = nullptr;
        __try {
            arr = e.findAll(typeObj, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr = nullptr;
        }
        if (!arr || !LooksLikeHeapPtr(arr)) continue;
        int n = 0;
        __try {
            n = static_cast<int>(
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + offLen));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
        if (n <= 0) continue;
        // 活跃 Text 很多；封禁正文走 UiText.get_text，不必扫满图。
        if (n > 32) n = 32;
        int seen = 0;
        for (int i = 0; i < n && seen < 12; ++i) {
            void* o = nullptr;
            __try {
                o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                              static_cast<size_t>(i) * sizeof(void*));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                o = nullptr;
            }
            if (!o || !LooksLikeHeapPtr(o) || !UnityAlive(o)) continue;
            void* go = nullptr;
            if (miGetGo && miGetGo->methodPointer) {
                __try {
                    go = reinterpret_cast<FnGetGameObject>(miGetGo->methodPointer)(o, miGetGo);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    go = nullptr;
                }
            }
            if (go && LooksLikeHeapPtr(go)) {
                const bool selfOn = GoActiveSelf(go, miGetActive);
                const bool hierOn = miGetHier ? GoActiveInHierarchy(go, miGetHier) : selfOn;
                if (!selfOn && !hierOn) continue;
            }
            ++seen;
            char buf[480]{};
            if (miText && miText->methodPointer) {
                void* str = nullptr;
                __try {
                    using FnGetText = void* (*)(void* self, const void* method);
                    str = reinterpret_cast<FnGetText>(miText->methodPointer)(o, miText);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    str = nullptr;
                }
                if (ReadNoticeMsgUtf8(str, buf, sizeof(buf)) && buf[0]) {
                    EmitDialogScrape(t.tag, 0, buf, "get_text");
                    if (NoticeMsgLooksKickish(buf)) CacheDialogPtr(o);
                    continue;
                }
            }
            // get_text 失败：字段扫；仅 kickish 会 Emit / Cache
            ScrapeObjectStrings(o, t.tag, "tmp_fields", 0x40, 0x180);
        }
    }
}

void ScrapeLoginDialogsOnPump(void* /*user*/) {
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginDlgScrape")) return;
    if (!x::runtime::il2cpp::Ensure()) return;
    // 仅空闲大厅：进图 / 软重连 busy·hold 时不做 FindAll。
    if (x::features::ports::world::IsInMapScene()) return;
    if (gBusy.load(std::memory_order_acquire) || gHold.load(std::memory_order_acquire)) return;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.findAll) return;

    struct Entry {
        const char* hash;
        const char* plain;
        const char* tag;
    };
    const Entry kEntries[] = {
        {kLoginUtilDialogClass, "UILoginUtilDialog", "LoginUtil"},
        {kNoticeDialogClass, "UINoticeDialog", "NoticeDlg"},
        {kUtilDialogClass, nullptr, "UtilDlg"},
        {kUtilDialogExClass, nullptr, "UtilDlgEx"},
        {kMultiLineNoticeClass, "UIMultiLineNotice", "MultiLine"},
        {kSlideNoticeClass, "UISlideNotice", "Slide"},
    };

    MethodInfoHead* miGetGo = nullptr;
    MethodInfoHead* miGetActive = nullptr;
    MethodInfoHead* miGetHier = nullptr;
    void* compKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    if (compKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true};
        auto r = x::runtime::il2cpp_method::FindMethodResolved(compKlass, kRvaCompGetGo, kGo,
                                                               "get_gameObject", nullptr);
        miGetGo = AsMi(r.method);
    }
    if (goKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kAct{0, TypeKind::Bool, true, true, {}};
        auto ra = x::runtime::il2cpp_method::FindMethodResolved(goKlass, kRvaGoGetActiveSelf, kAct,
                                                                "get_activeSelf", nullptr);
        miGetActive = AsMi(ra.method);
        void* byHier =
            x::runtime::il2cpp_method::FindMethodByName(goKlass, "get_activeInHierarchy", 0, false);
        miGetHier = AsMi(byHier);
    }

    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    for (const Entry& ent : kEntries) {
        void* klass = ResolveUiKlass(ent.hash, ent.plain);
        if (!klass) continue;
        void* typeObj = ClassTypeObjectOnPump(klass);
        if (!typeObj) continue;
        void* arr = nullptr;
        __try {
            arr = e.findAll(typeObj, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr = nullptr;
        }
        if (!arr || !LooksLikeHeapPtr(arr)) continue;
        int n = 0;
        __try {
            n = static_cast<int>(
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + offLen));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
        if (n <= 0) continue;
        if (n > 12) n = 12;
        for (int i = 0; i < n; ++i) {
            void* o = nullptr;
            __try {
                o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                              static_cast<size_t>(i) * sizeof(void*));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                o = nullptr;
            }
            if (!o || !LooksLikeHeapPtr(o) || !UnityAlive(o)) continue;
            void* go = nullptr;
            if (miGetGo && miGetGo->methodPointer) {
                __try {
                    go = reinterpret_cast<FnGetGameObject>(miGetGo->methodPointer)(o, miGetGo);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    go = nullptr;
                }
            }
            if (go && LooksLikeHeapPtr(go)) {
                const bool selfOn = GoActiveSelf(go, miGetActive);
                const bool hierOn = miGetHier ? GoActiveInHierarchy(go, miGetHier) : selfOn;
                if (!selfOn && !hierOn) continue;
            }
            CacheDialogPtr(o);
            ScrapeDlgFieldsDeep(o, ent.tag);
        }
    }
    ScrapeTmpTextsOnPump(miGetGo, miGetActive, miGetHier);
}

struct ProbeDlgCtx {
    int live = 0;      // 白名单里 UnityAlive + 层级可见
    int kickish = 0;   // 正文含断线/登出
    int notice = 0;    // UINoticeDialog / UILoginUtilDialog 可见
    char detail[160]{};
};

// 只探活窗，不 Close。FindAll 白名单，永不扫 UIDialog 基类。进图残留也扫。
void ProbeLiveKickDialogOnPump(void* user) {
    auto* ctx = static_cast<ProbeDlgCtx*>(user);
    if (!ctx) return;
    *ctx = {};
    if (!x::runtime::main_thread::AssertOnPumpThread("SoftLoginDlgProbe")) {
        snprintf(ctx->detail, sizeof(ctx->detail), "not_on_pump");
        return;
    }
    if (!x::runtime::il2cpp::Ensure()) {
        snprintf(ctx->detail, sizeof(ctx->detail), "il2cpp");
        return;
    }
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.findAll) {
        snprintf(ctx->detail, sizeof(ctx->detail), "no_findall");
        return;
    }

    struct Entry {
        const char* hash;
        const char* plain;
        const char* tag;
        int isNotice;
    };
    const Entry kEntries[] = {
        {kLoginUtilDialogClass, "UILoginUtilDialog", "LoginUtil", 1},
        {kNoticeDialogClass, "UINoticeDialog", "NoticeDlg", 1},
        {kUtilDialogClass, nullptr, "UtilDlg", 0},
        {kUtilDialogExClass, nullptr, "UtilDlgEx", 0},
        {kMultiLineNoticeClass, "UIMultiLineNotice", "MultiLine", 0},
    };

    MethodInfoHead* miGetGo = nullptr;
    MethodInfoHead* miGetActive = nullptr;
    MethodInfoHead* miGetHier = nullptr;
    void* compKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    if (compKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true};
        auto r = x::runtime::il2cpp_method::FindMethodResolved(compKlass, kRvaCompGetGo, kGo,
                                                               "get_gameObject", nullptr);
        miGetGo = AsMi(r.method);
    }
    if (goKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kAct{0, TypeKind::Bool, true, true, {}};
        auto ra = x::runtime::il2cpp_method::FindMethodResolved(goKlass, kRvaGoGetActiveSelf, kAct,
                                                                "get_activeSelf", nullptr);
        miGetActive = AsMi(ra.method);
        void* byHier =
            x::runtime::il2cpp_method::FindMethodByName(goKlass, "get_activeInHierarchy", 0, false);
        miGetHier = AsMi(byHier);
    }

    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    for (const Entry& ent : kEntries) {
        void* klass = ResolveUiKlass(ent.hash, ent.plain);
        if (!klass) continue;
        void* typeObj = ClassTypeObjectOnPump(klass);
        if (!typeObj) continue;
        void* arr = nullptr;
        __try {
            arr = e.findAll(typeObj, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr = nullptr;
        }
        if (!arr || !LooksLikeHeapPtr(arr)) continue;
        int n = 0;
        __try {
            n = static_cast<int>(
                *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + offLen));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
        if (n <= 0) continue;
        if (n > 12) n = 12;
        for (int i = 0; i < n; ++i) {
            void* o = nullptr;
            __try {
                o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                              static_cast<size_t>(i) * sizeof(void*));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                o = nullptr;
            }
            if (!o || !LooksLikeHeapPtr(o) || !UnityAlive(o)) continue;
            bool visible = true;
            void* go = nullptr;
            if (miGetGo && miGetGo->methodPointer) {
                __try {
                    go = reinterpret_cast<FnGetGameObject>(miGetGo->methodPointer)(o, miGetGo);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    go = nullptr;
                }
            }
            if (go && LooksLikeHeapPtr(go)) {
                const bool selfOn = GoActiveSelf(go, miGetActive);
                const bool hierOn = miGetHier ? GoActiveInHierarchy(go, miGetHier) : selfOn;
                visible = selfOn || hierOn;
            }
            if (!visible) continue;
            ctx->live++;
            if (ent.isNotice) ctx->notice++;
            if (DlgHasDisconnectText(o)) ctx->kickish++;
            CacheDialogPtr(o);
        }
    }
    snprintf(ctx->detail, sizeof(ctx->detail), "live=%d kickish=%d notice=%d", ctx->live,
             ctx->kickish, ctx->notice);
}

// BIN 22:47:45：skip 时 Notice 仍在 → GoWorld → avatars=0。
// 00:03/00:16 成功轮 skip 时 noticeKinds=0；settle 那枪的 1 不在这里判。
bool DirtyHallNoticeBlocksEnter(const char* tag) {
    ProbeDlgCtx probe{};
    if (!SoftPumpCall(&ProbeLiveKickDialogOnPump, &probe, kDismissCallMs)) return false;
    if (probe.kickish <= 0 && probe.notice <= 0) return false;
    LogLine("hall_notice dirty tag=%s %s — recheck %ums", tag ? tag : "?", probe.detail,
            static_cast<unsigned>(kDirtyHallNoticeRecheckMs));
    KickLogLine("hall_notice dirty tag=%s %s", tag ? tag : "?", probe.detail);
    Sleep(kDirtyHallNoticeRecheckMs);
    if (gStop.load()) return true;
    ProbeDlgCtx again{};
    if (!SoftPumpCall(&ProbeLiveKickDialogOnPump, &again, kDismissCallMs)) return false;
    if (again.kickish <= 0 && again.notice <= 0) return false;
    LogLine("hall_notice persist tag=%s %s — no GoWorld (avoid 叠登)", tag ? tag : "?",
            again.detail);
    KickLogLine("hall_notice persist tag=%s", tag ? tag : "?");
    return true;
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
    ctx->noticeKinds = noticeHits;

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

bool ImpactAirSkipStand() {
    return x::features::simple_combat::IsEnabled() &&
           x::features::simple_combat::IsImpactApproachEnabled();
}

void ArmLandQuiet(DWORD ms) {
    const DWORD now = GetTickCount();
    DWORD armed = now ? now : 1;
    gLandQuietArmedAtMs.store(armed, std::memory_order_release);

    // RESULT 已等挂台：再套 quiet 是落地后空等。Impact 起飞仍有 wait_onFh。
    // 空中贴怪：curFh 一直 0，quiet 会再 SafeLand 把正在打的人拽到台下。
    if (x::features::ports::world::IsPlayReady() &&
        (x::features::ports::foothold::PeekCurFhId() != 0 || ImpactAirSkipStand())) {
        gLandQuietUntilMs.store(0, std::memory_order_release);
        gPostSoftAirUntilMs.store(0, std::memory_order_release);
        if (x::features::ports::foothold::PeekCurFhId() != 0) {
            LogLine("land_quiet skip stood (already onFh)");
            KickLogLine("land_quiet skip stood");
        } else {
            LogLine("land_quiet skip impact air (curFh=0 hover is land)");
            KickLogLine("land_quiet skip impact_air");
        }
        return;
    }

    const DWORD dur = ms ? ms : kSoftLandQuietMs;
    DWORD until = now + dur;
    if (until == 0) until = 1;
    gLandQuietUntilMs.store(until, std::memory_order_release);
    DWORD airUntil = now + kSoftPostAirGateMs;
    if (airUntil == 0) airUntil = 1;
    // 若调用方传入更长 quiet，空中闸至少盖住 quiet。
    if (static_cast<int>(airUntil - until) < 0) airUntil = until;
    gPostSoftAirUntilMs.store(airUntil, std::memory_order_release);
    LogLine("land_quiet arm=%ums post_air_gate=%ums (not onFh)", static_cast<unsigned>(dur),
            static_cast<unsigned>(airUntil - now));
    KickLogLine("land_quiet arm=%ums post_air=%ums", static_cast<unsigned>(dur),
                static_cast<unsigned>(airUntil - now));
}

// Finish 兑现最小 hold 时长期间静默窗会照常流逝；顺延同样时长，
// 避免 hold 一落地就已经没有停刀/禁空中窗口。
void ExtendQuietWindows(DWORD extraMs) {
    if (!extraMs) return;
    DWORD until = gLandQuietUntilMs.load(std::memory_order_acquire);
    if (until) {
        DWORD next = until + extraMs;
        if (next == 0) next = 1;
        gLandQuietUntilMs.store(next, std::memory_order_release);
    }
    DWORD air = gPostSoftAirUntilMs.load(std::memory_order_acquire);
    if (air) {
        DWORD next = air + extraMs;
        if (next == 0) next = 1;
        gPostSoftAirUntilMs.store(next, std::memory_order_release);
    }
}

bool BreakerActive() {
    DWORD until = gBreakerUntilMs.load(std::memory_order_acquire);
    if (!until) return false;
    const DWORD now = GetTickCount();
    if (static_cast<int>(until - now) > 0) return true;
    if (gBreakerUntilMs.compare_exchange_strong(until, 0, std::memory_order_acq_rel)) {
        LogLine("breaker off — resume soft takeover");
        KickLogLine("breaker off");
    }
    return false;
}

void ClearBreakerLocked(const char* why) {
    const DWORD until = gBreakerUntilMs.exchange(0, std::memory_order_acq_rel);
    gFailStreak.store(0, std::memory_order_release);
    gFailStreakFirstMs.store(0, std::memory_order_release);
    if (!until) return;
    LogLine("breaker cleared why=%s (allow hop-fail CloseSession)", why ? why : "?");
    KickLogLine("breaker cleared why=%s", why ? why : "?");
}

void NoteSoftSuccess() {
    gFailStreak.store(0, std::memory_order_release);
    gFailStreakFirstMs.store(0, std::memory_order_release);
}

void NoteSoftFailure() {
    const DWORD now = GetTickCount();
    DWORD first = gFailStreakFirstMs.load(std::memory_order_acquire);
    if (!first || now - first > kSoftFailBreakerWindowMs) {
        first = now ? now : 1;
        gFailStreakFirstMs.store(first, std::memory_order_release);
        gFailStreak.store(0, std::memory_order_release);
    }
    const int streak = gFailStreak.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (streak < kSoftFailBreakerMax) {
        LogLine("fail_streak %d/%d window=%ums", streak, kSoftFailBreakerMax,
                static_cast<unsigned>(now - first));
        KickLogLine("fail_streak %d/%d", streak, kSoftFailBreakerMax);
        return;
    }
    DWORD until = now + kSoftFailBreakerHoldOffMs;
    if (until == 0) until = 1;
    gBreakerUntilMs.store(until, std::memory_order_release);
    gFailStreak.store(0, std::memory_order_release);
    gFailStreakFirstMs.store(0, std::memory_order_release);
    LogLine("breaker ON — %d fails in %ums; stop takeover for %us (guardian clean relaunch)",
            streak, static_cast<unsigned>(now - first),
            static_cast<unsigned>(kSoftFailBreakerHoldOffMs / 1000));
    KickLogLine("breaker ON fails=%d holdoff=%us", streak,
                static_cast<unsigned>(kSoftFailBreakerHoldOffMs / 1000));
    x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
        x::features::notify::NotificationKind::Warning, "soft-login-breaker", "软重连暂停接管",
        "连续失败已达上限 · 交守护干净重拉", 8000});
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

// suppressNotify：调用方已自行弹了更贴切的气泡（见 SoftFinishIfInMarket）。
// 只压气泡，日志 / result / hold 一律照常，守护那侧的判据不受影响。
void Finish(unsigned result, const char* line, bool suppressNotify = false) {
    gResult.store(result, std::memory_order_release);
    if (line) LogLine("%s", line);
    if (!suppressNotify) PublishSoftLoginNotify(result, line);
    // 成功/失败都解冻：失败若留 freeze=1，InterStage 不 quiesce、play 功能一直 defer，
    // 再叠 SoftPumpCall 会把已死泵打成 job timeout 螺旋（BIN 02:23）。
    x::runtime::managed_main::SetLoginFreeze(false);
    // 成功：丢掉本轮 CloseSession / 进图 Session 抖动排进的 pending，避免
    // RESULT 同毫秒立刻 attempt begin（BIN 02:11–02:12 无限软重连）。
    if (result == 1) gPending.store(false, std::memory_order_release);
    if (result == 1) {
        NoteSoftSuccess();
    } else if (result == 2) {
        NoteSoftFailure();
    }
    gInMapRecoverSinceMs.store(0, std::memory_order_release);
    gInMapRecoverDropSinceMs.store(0, std::memory_order_release);
    gSettleProactiveFast.store(false, std::memory_order_release);
    // 守护 500ms 才采一拍 Status SHM：太短的 attempt 会让「result=0/hold=1」整拍消失，
    // 上升沿吞 seq 与 hold 推迟两条判据同时落空 → 干净重拉杀进程（BIN 17:29）。
    // 这里把 hold 按住到至少 kMinHoldMs，保证守护先采到观察窗、再采到 RESULT。
    const DWORD began = gAttemptBeginMs.exchange(0, std::memory_order_acq_rel);
    if (began && !gStop.load()) {
        const DWORD elapsed = GetTickCount() - began;
        if (elapsed < kMinHoldMs) {
            const DWORD remain = kMinHoldMs - elapsed;
            LogLine("min_hold keep hold=1 +%ums (attempt=%ums < %ums; guardian SHM 500ms/tick)",
                    static_cast<unsigned>(remain), static_cast<unsigned>(elapsed),
                    static_cast<unsigned>(kMinHoldMs));
            KickLogLine("min_hold +%ums attempt=%ums", static_cast<unsigned>(remain),
                        static_cast<unsigned>(elapsed));
            Sleep(remain);
            if (result == 1) ExtendQuietWindows(remain);
        }
    }
    SetHold(false);
    gBusy.store(false);
}

// BIN 02:11–02:12：进图后 SoftHall.WorldItems=0（不在登录 UI）被当成「空大厅」
// → CloseSession → KickSniff RequestAttempt → 再软重连 → stickyCh+1 无限环。
// 图内绝不硬拆；直接 RESULT success 解 hold。
//
// 返回：1=已 success 解 hold；0=确认不在图（可硬拆）；-1=未知/泵忙（禁止 CloseSession）。
// BIN 15:38：play-boot 泵堵时 SoftPumpCall(SamplePlayReady) 失败被当成 !inMap，
// 同时 WorldPort 已 inMap=1 — 误 SoftForceNmTeardown → job timeout 螺旋。
//
// BIN 17:29：Disconnected / close_session_inmap 时 MapScene 常短暂残留；
// 若仍走 already_in_map → hold 仅十余 ms → 守护看不到 hold、吞不了 disconnectSeq → 干净重拉杀进程。
// 真断线 why 禁止只凭场景判「已在图成功」；场景残留时返回 -1（勿硬拆），已离开图返回 0（走重连）。
// 但残留与「游戏自己恢复了」必须能分开：后者由 InMapRecoverConfirmed 给出口
//（playReady+挂台+Connected 稳住 2s），否则 connect-wait 会空耗 160 轮 × 10 cycle
// ≈ 十分钟 hold 停打怪，最后 Finish(2) 把好端端在图里的客户端交给守护杀掉。
bool WhyStringNeedsRealReconnect(const char* why) {
    if (!why || !why[0]) return false;
    return std::strcmp(why, "disconnected") == 0 || std::strcmp(why, "disconnecting") == 0 ||
           std::strcmp(why, "close_session_inmap") == 0 || std::strcmp(why, "stuck_lobby") == 0 ||
           std::strcmp(why, "dialog_linger") == 0 ||
           std::strcmp(why, "nm_gone_inmap") == 0 || std::strcmp(why, "inbound_dead_inmap") == 0 ||
           std::strcmp(why, "mob_gather_timer") == 0 ||
           std::strcmp(why, "hangup_timer") == 0 ||
           std::strcmp(why, "hangup_fires") == 0 ||
           std::strcmp(why, "mob_gather_clear") == 0 ||
           std::strncmp(why, "channel_hop", 11) == 0;
}

std::atomic<DWORD> gLastRefuseLogMs{0};
constexpr DWORD kRefuseLogGapMs = 3000;

// 真断线 why 下的图内出口：playReady + 挂台（curFh≠0）+ NM Connected 连续
// kInMapRecoverConfirmMs → 游戏已自行恢复（假断线边沿 / 客户端自连），可以认成功解 hold。
// 条件不齐或未满窗则清零计时返回 false，调用方仍按「MapScene 残留」处理（禁止硬拆）。
//
// ★ fh-ban 武装时引擎故意 CurFh=0（F6 / F5 Impact / Travel）：不得当成「未恢复悬空」
// 清掉计时，否则主城 soft hold 期间一开 F6 就永久卡 recover（BIN 2026-08-12 22:02）。
bool InMapRecoverConfirmed(const PlayReadyCtx& play, const char* tag) {
    const bool intentionalDetach = x::features::ports::fly_fh_ban::IsBanActive();
    SampleCtx s{};
    const bool nmOk = SoftPumpCall(&SampleNmOnPump, &s, kSoftSampleCallMs) && s.nmOk &&
                      s.state == kStateConnected;
    const bool standing = play.ready && (play.curFh != 0 || intentionalDetach);
    if (!standing || !nmOk) {
        const DWORD now = GetTickCount();
        DWORD drop = gInMapRecoverDropSinceMs.load(std::memory_order_acquire);
        if (gInMapRecoverSinceMs.load(std::memory_order_acquire)) {
            if (!drop) {
                gInMapRecoverDropSinceMs.store(now ? now : 1, std::memory_order_release);
                return false;
            }
            if (now - drop < kInMapRecoverDropGraceMs) return false;
        }
        if (gInMapRecoverSinceMs.exchange(0, std::memory_order_acq_rel)) {
            gInMapRecoverDropSinceMs.store(0, std::memory_order_release);
            LogLine("in_map_recover reset tag=%s ready=%d curFh=%u fhBan=%d nmOk=%d state=%s(%d)",
                    tag ? tag : "?", play.ready, static_cast<unsigned>(play.curFh),
                    intentionalDetach ? 1 : 0, s.nmOk, StateName(s.state), s.state);
        }
        return false;
    }
    gInMapRecoverDropSinceMs.store(0, std::memory_order_release);
    const DWORD now = GetTickCount();
    DWORD since = gInMapRecoverSinceMs.load(std::memory_order_acquire);
    if (!since) {
        since = now ? now : 1;
        gInMapRecoverSinceMs.store(since, std::memory_order_release);
        LogLine("in_map_recover watch tag=%s curFh=%u fhBan=%d Connected — confirm after %ums",
                tag ? tag : "?", static_cast<unsigned>(play.curFh), intentionalDetach ? 1 : 0,
                static_cast<unsigned>(kInMapRecoverConfirmMs));
        KickLogLine("in_map_recover watch curFh=%u fhBan=%d", static_cast<unsigned>(play.curFh),
                    intentionalDetach ? 1 : 0);
        return false;
    }
    const DWORD held = now - since;
    if (held < kInMapRecoverConfirmMs) return false;
    LogLine("in_map_recover confirmed tag=%s held=%ums curFh=%u", tag ? tag : "?",
            static_cast<unsigned>(held), static_cast<unsigned>(play.curFh));
    KickLogLine("in_map_recover confirmed held=%ums", static_cast<unsigned>(held));
    return true;
}

// 野外开拍卖 / 进商城是客户端**主动迁服**（WorldManager.SendMigrateToGlobalMarketRequest）：
// 游戏自己关掉当前会话再连市场服，Disconnected 边沿照样抬，但这不是被踢。
// 判据取游戏自维护的场景态：GlobalMarket=5（BIN D217 实测拍卖读作 5）、CashShop=4；
// channel_hop / encounter / player_hide / invuln 早已按同一口径判，此处不另立标准。
//
// 服端拒绝野外迁移（GlobalMarketTerminated，op 398）时场景**不会**变成 4/5 —— 那是真断线，
// 仍走原路重连。两种情形正好被这一条判据分开，不必再看 why / 会话状态。
bool SoftSceneIsMarket(int* sceneOut) {
    using x::features::ports::world::SceneState;
    const SceneState ss = x::features::ports::world::GetSceneState();
    if (sceneOut) *sceneOut = static_cast<int>(ss);
    return ss == SceneState::GlobalMarket || ss == SceneState::CashShop;
}

// 命中则整轮 attempt 以「成功」收尾，与 already_in_map 同口径。
// **不得**改成在 RequestAttempt 里直接不接：那样 hold 保持 0 撞上 disconnectSeq 上涨，
// 守护会按服务器踢线干净重拉，把正在逛拍卖的客户端杀掉（hangup_schedule 只在
// softLoginResult 0→1 上升沿吞 seq）。所以必须先接下 hold，再报 result=1。
// 气泡节流：land quiet 只有 1.5s，压不住逛拍卖期间的后续断线边沿（lost_session 在
// !inMap 下照样武装）。日志不节流（取证要全），只防气泡刷屏。
std::atomic<DWORD> gLastMarketNotifyMs{0};
constexpr DWORD kMarketNotifyGapMs = 30000;

bool SoftFinishIfInMarket(const char* tag) {
    int scene = -1;
    if (!SoftSceneIsMarket(&scene)) return false;
    char ok[240]{};
    snprintf(ok, sizeof(ok),
             "RESULT success in_market skip_teardown tag=%s scene=%d why=%s (拍卖/商城迁服，非踢线)",
             tag ? tag : "?", scene, gWhy);
    KickLogLine("in_market skip_teardown tag=%s scene=%d", tag ? tag : "?", scene);
    // 不弹 Finish 那条「软重连成功」：本轮压根没重连，报成功会让用户以为刚被踢过。
    // 换一条说清「是你自己开了拍卖/商城」，顺带交代守护不会重拉。
    const DWORD nowNotify = GetTickCount();
    const DWORD lastNotify = gLastMarketNotifyMs.load(std::memory_order_acquire);
    if (!lastNotify || nowNotify - lastNotify >= kMarketNotifyGapMs) {
        gLastMarketNotifyMs.store(nowNotify ? nowNotify : 1, std::memory_order_release);
        x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
            x::features::notify::NotificationKind::Info, "soft-login-market", "拍卖/商城迁服",
            scene == static_cast<int>(x::features::ports::world::SceneState::CashShop)
                ? "已识别为进商城，非踢线：软重连站住不动，守护也不会重拉"
                : "已识别为开拍卖，非踢线：软重连站住不动，守护也不会重拉",
            6000});
    }
    ArmLandQuiet(kSoftLandQuietMs);
    Finish(1, ok, /*suppressNotify=*/true);
    return true;
}

int SoftProbeInMapOrFinish(const char* tag) {
    // 拍卖/商城优先于一切图内判据：此时 inMap 必为 0，不先认出来就会被当成「回到大厅」。
    if (SoftFinishIfInMarket(tag)) return 1;
    // 出刀闸到期：禁止 stuck_lobby / already_in_map 假成功把 hangup 当落地。
    if (x::features::ports::mob_gather::HangupFiresDue() &&
        x::features::ports::world::IsInMapScene()) {
        const DWORD now = GetTickCount();
        const DWORD lastLog = gLastRefuseLogMs.load(std::memory_order_acquire);
        if (!lastLog || now - lastLog >= kRefuseLogGapMs) {
            gLastRefuseLogMs.store(now ? now : 1, std::memory_order_release);
            LogLine("in_map_probe refuse hangup_fires_due tag=%s why=%s — CloseSession first",
                    tag ? tag : "?", gWhy);
            KickLogLine("in_map_probe refuse hangup_fires_due tag=%s", tag ? tag : "?");
        }
        return -1;
    }
    // gWhy 是裸缓冲；判定读原子量，避免 memset 中间态被读成空串又放开假成功。
    const bool needReconnect = gWhyNeedsReconnect.load(std::memory_order_acquire);
    // Off-pump：WorldPort 场景缓存（worker 其它路径已在用）；泵堵时仍能认出已进图。
    // 真断线 why 不得只凭场景缓存判成功（MapScene 会残留）——落到下面泵采样做确认恢复。
    if (!needReconnect && x::features::ports::world::IsInMapScene()) {
        char ok[240]{};
        snprintf(ok, sizeof(ok),
                 "RESULT success already_in_map skip_teardown tag=%s via=off_pump why=%s",
                 tag ? tag : "?", gWhy);
        KickLogLine("already_in_map skip_teardown tag=%s via=off_pump", tag ? tag : "?");
        ArmLandQuiet(kSoftLandQuietMs);
        Finish(1, ok);
        return 1;
    }
    if (SoftShouldDeferPumpWork()) {
        LogLine("in_map_probe defer tag=%s pump_stale age=%ums", tag ? tag : "?",
                static_cast<unsigned>(x::runtime::main_thread::LastRealTickAgeMs()));
        KickLogLine("in_map_probe defer tag=%s pump_stale", tag ? tag : "?");
        return -1;
    }
    PlayReadyCtx play{};
    if (!SoftPumpCall(&SamplePlayReadyOnPump, &play, kPlayReadySampleMs)) {
        LogLine("in_map_probe defer tag=%s sample_pump_fail", tag ? tag : "?");
        KickLogLine("in_map_probe defer tag=%s sample_pump_fail", tag ? tag : "?");
        return -1;
    }
    if (!play.sampled) {
        KickLogLine("in_map_probe defer tag=%s not_sampled", tag ? tag : "?");
        return -1;
    }
    if (!play.inMap) {
        // 已在确认恢复：闪一拍 !inMap 不清钟（返回 -1 让 WaitInMapRecoverIfPending 继续等）。
        if (gInMapRecoverSinceMs.load(std::memory_order_acquire)) {
            const DWORD now = GetTickCount();
            DWORD drop = gInMapRecoverDropSinceMs.load(std::memory_order_acquire);
            if (!drop) {
                gInMapRecoverDropSinceMs.store(now ? now : 1, std::memory_order_release);
                return -1;
            }
            if (now - drop < kInMapRecoverDropGraceMs) return -1;
            gInMapRecoverSinceMs.store(0, std::memory_order_release);
            gInMapRecoverDropSinceMs.store(0, std::memory_order_release);
            LogLine("in_map_recover reset tag=%s left_map", tag ? tag : "?");
        }
        return 0;
    }
    if (needReconnect) {
        if (!InMapRecoverConfirmed(play, tag)) {
            // 每 250ms 一轮的 connect-wait 会刷爆日志；节流后仍保留可诊断节奏。
            const DWORD now = GetTickCount();
            const DWORD lastLog = gLastRefuseLogMs.load(std::memory_order_acquire);
            if (!lastLog || now - lastLog >= kRefuseLogGapMs) {
                gLastRefuseLogMs.store(now ? now : 1, std::memory_order_release);
                LogLine("in_map_probe refuse already_in_map why=%s tag=%s curFh=%u ready=%d "
                        "(MapScene linger / not recovered yet)",
                        gWhy, tag ? tag : "?", static_cast<unsigned>(play.curFh), play.ready);
                KickLogLine("in_map_probe refuse already_in_map why=%s tag=%s", gWhy,
                            tag ? tag : "?");
            }
            return -1;
        }
        char ok[240]{};
        snprintf(ok, sizeof(ok),
                 "RESULT success in_map_recovered tag=%s curFh=%u confirm=%ums why=%s",
                 tag ? tag : "?", static_cast<unsigned>(play.curFh),
                 static_cast<unsigned>(kInMapRecoverConfirmMs), gWhy);
        KickLogLine("RESULT success in_map_recovered tag=%s curFh=%u", tag ? tag : "?",
                    static_cast<unsigned>(play.curFh));
        ArmLandQuiet(kSoftLandQuietMs);
        Finish(1, ok);
        return 1;
    }
    char ok[240]{};
    snprintf(ok, sizeof(ok),
             "RESULT success already_in_map skip_teardown tag=%s curFh=%u why=%s",
             tag ? tag : "?", static_cast<unsigned>(play.curFh), gWhy);
    KickLogLine("already_in_map skip_teardown tag=%s curFh=%u", tag ? tag : "?",
                static_cast<unsigned>(play.curFh));
    ArmLandQuiet(kSoftLandQuietMs);
    Finish(1, ok);
    return 1;
}

bool SoftFinishIfAlreadyInMap(const char* tag) {
    return SoftProbeInMapOrFinish(tag) == 1;
}

// cycle_begin / 空大厅超时：已经 playReady+挂台，把确认窗等完，禁止再 ConnectLogin / 空等 12s hall。
// BIN 8cab31：04:43:01 curFh=47 ready=1 仍走重连 → hall_ready timeout 12s + recover 8s 才弹成功。
bool WaitInMapRecoverIfPending(const char* tag) {
    if (!gInMapRecoverSinceMs.load(std::memory_order_acquire)) return false;
    LogLine("in_map_recover wait_out tag=%s confirm=%ums", tag ? tag : "?",
            static_cast<unsigned>(kInMapRecoverConfirmMs));
    KickLogLine("in_map_recover wait_out tag=%s", tag ? tag : "?");
    while (!gStop.load()) {
        if (SoftProbeInMapOrFinish(tag) == 1) return true;
        if (!gInMapRecoverSinceMs.load(std::memory_order_acquire)) return false;
        Sleep(200);
    }
    return false;
}

// 1=已 Finish 成功（图内自愈）；0=已出图，允许 ConnectLogin；-1=仍图内超时，禁止叠登。
int WaitLeaveMapOrRecover(const char* tag) {
    if (!gWhyNeedsReconnect.load(std::memory_order_acquire)) return 0;
    const DWORD deadline = GetTickCount() + kLeaveMapBeforeConnectMs;
    bool logged = false;
    while (!gStop.load()) {
        const int im = SoftProbeInMapOrFinish(tag);
        if (im == 1) return 1;
        if (im == 0) {
            if (logged) {
                LogLine("leave_map ok tag=%s — ConnectLogin allowed", tag ? tag : "?");
                KickLogLine("leave_map ok tag=%s", tag ? tag : "?");
            }
            return 0;
        }
        if (WaitInMapRecoverIfPending(tag)) return 1;
        if (!logged) {
            logged = true;
            LogLine("leave_map wait tag=%s max=%ums — no ConnectLogin while MapScene linger",
                    tag ? tag : "?", static_cast<unsigned>(kLeaveMapBeforeConnectMs));
            KickLogLine("leave_map wait tag=%s", tag ? tag : "?");
        }
        if (static_cast<int>(deadline - GetTickCount()) <= 0) {
            LogLine("leave_map timeout tag=%s — skip ConnectLogin (avoid 叠登)",
                    tag ? tag : "?");
            KickLogLine("leave_map timeout tag=%s", tag ? tag : "?");
            return -1;
        }
        Sleep(200);
    }
    return -1;
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
// 1=可进；0=超时/停止（调用方 soft cycle，勿空等 WorldItems）；-1=已图内收尾（Finish 过）。
//
// BIN 9cbbfb：auto_enter Failed 后大厅 items=0（人已回图 curFh=422 / NM Connected），
// 旧逻辑空等满 12s 才 hall_wait_timeout 起 recover，气泡比落地晚十几秒。
int WaitHallPickReady(DWORD waitMs) {
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
                // SoftHall ready 时 MapScene 仍可能残留（BIN 00:14:59）；先探图再放行进大厅。
                const int im = SoftProbeInMapOrFinish("hall_ready");
                if (im == 1) return -1;
                if (im < 0) {
                    if (WaitInMapRecoverIfPending("hall_ready")) return -1;
                    LogLine("hall_ready blocked inMap linger items=%d — keep waiting",
                            hall.worldItems);
                    KickLogLine("hall_ready blocked inMap items=%d", hall.worldItems);
                } else {
                    KickLogLine("hall_ready ok items=%d selWorld=%d", hall.worldItems,
                                hall.selectedWorld);
                    return 1;
                }
            }
        }
        // 空大厅等待中人已经挂台：立刻走确认恢复，禁止再空耗剩余 12s。
        {
            const int im = SoftProbeInMapOrFinish("hall_wait");
            if (im == 1) return -1;
            if (im < 0 && WaitInMapRecoverIfPending("hall_wait")) return -1;
        }
        if (static_cast<int>(deadline - GetTickCount()) <= 0) break;
        Sleep(kHallReadyPollMs);
    }
    LogLine("hall_ready timeout %ums lastItems=%d", static_cast<unsigned>(waitMs), lastItems);
    KickLogLine("hall_ready timeout items=%d", lastItems);
    return 0;
}

// 弱网 / 泵暂死可恢复：未用尽 soft cycle 则保持 hold/busy，回跳再跑 ConnectLogin→重进。
// true = 调用方 ++softCycle 后 goto soft_cycle_begin；false = 已 Finish(2) 或应停。
// 泵短暂 idle 允许 soft cycle（B9B 08:32：connect_wait_pump_dead 后 ~1s 自连，
// 旧 soft_fatal_no_retry 立刻放 hold → 守护干净重拉，体感「乱杀」）。
// emptyHallStreak：连续空大厅满 kEmptyHallSoftCycleMax 放 hold（BIN 01:53 书页空转）。
// limboStreak：连续中间态满 kLimboSoftCycleMax 放 hold（BIN 21:11 两锚点全失）。
//
// 同类失败连续满额 → 放 hold 交守护。true = 已 Finish(2)，调用方不得再重试。
// 不同类失败会打断计数：一次成功采样即证明还有着力点，不该按累计判死。
bool StreakGiveUp(int* streak, int max, bool hit, const char* tag, const char* failLine) {
    if (!streak) return false;
    if (!hit) {
        *streak = 0;
        return false;
    }
    if (++(*streak) < max) return false;
    LogLine("%s streak=%d/%d — release hold for guardian", tag, *streak, max);
    KickLogLine("%s give_up streak=%d", tag, *streak);
    Finish(2, failLine);
    return true;
}

bool SoftFailOrRetry(int softCycle, const char* failLine, int* emptyHallStreak, int* limboStreak) {
    const bool emptyHall =
        failLine && std::strstr(failLine, "hall_world_items_empty") != nullptr;
    const bool limbo = failLine && std::strstr(failLine, "connect_wait_limbo") != nullptr;
    if (StreakGiveUp(emptyHallStreak, kEmptyHallSoftCycleMax, emptyHall, "empty_hall", failLine))
        return false;
    if (StreakGiveUp(limboStreak, kLimboSoftCycleMax, limbo, "limbo", failLine)) return false;
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

bool SoftFailOrRetry(int softCycle, const char* failLine, int* emptyHallStreak) {
    return SoftFailOrRetry(softCycle, failLine, emptyHallStreak, nullptr);
}

bool SoftFailOrRetry(int softCycle, const char* failLine) {
    return SoftFailOrRetry(softCycle, failLine, nullptr, nullptr);
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

// 中间态一轮命中：SceneLogin 不存在（同轮 Sample 与 Connect 两次窥探一致）+ 会话既非
// Connected 也非 Connecting（游戏自己也没在自连）。判据取值理由见 kLimboConnectRounds。
//
// **不得**再加「大厅没东西可点」：BIN 21:11 实测中间态下 SoftHall 仍报 ready=1 items=2
// （登录 UI 对象滞留在堆里，SceneLogin.Instance 已为 null），加这条会让出口永不触发。
// stuck_lobby 现以 WM SceneState==Login 为准，不再采 SoftHall.ready。
bool SampleLooksLimbo(const SampleCtx& s, const char* detail) {
    if (!DetailIsSlNull(detail)) return false;
    if (s.slOk) return false;
    return s.state != kStateConnected && s.state != kStateConnecting;
}

DWORD WINAPI Worker(LPVOID) {
    LogLine("worker start");
    DWORD lastDlgScrapeMs = 0;
    DWORD lastOffScrapeMs = 0;
    DWORD lastStuckLobbyMs = 0;
    DWORD loginHallSinceMs = 0;
    DWORD lastDlgProbeMs = 0;
    DWORD dlgHealSinceMs = 0;
    DWORD lastDlgProbeLogMs = 0;
    bool clearedCacheInMap = false;
    while (!gStop.load()) {
        // GA 可能晚于 Init；武装后空闲也装 Abs，赶在登录封禁窗之前。
        if (IsArmed()) {
            EnsureNoticeAbsHook();
            // 封禁/踢登录文案只出现在大厅；进图停 **文案 scrape**（防脏指针）。
            // 断线窗 **探活** 进图残留也跑（MapScene linger 时窗盖在图上）。
            const bool inMap = x::features::ports::world::IsInMapScene();
            const auto scene = x::features::ports::world::GetSceneState();
            const bool softBusy = gBusy.load(std::memory_order_acquire) ||
                                  gHold.load(std::memory_order_acquire);
            const DWORD now = GetTickCount();
            // 图内 CloseSession 粘性：必须真出图再武装。大厅壳残留 + inMap=1 开火 = 叠登。
            if (gDeferred.load(std::memory_order_acquire) && !softBusy && !IsLandQuiet() &&
                !gPending.load(std::memory_order_acquire)) {
                const DWORD since = gDeferredSinceMs.load(std::memory_order_acquire);
                if (since && now - since > kDeferredSoftMaxMs) {
                    gDeferred.store(false, std::memory_order_release);
                    LogLine("deferred soft expire why=%s", gDeferredWhy[0] ? gDeferredWhy : "?");
                    KickLogLine("deferred soft expire");
                } else if (!inMap) {
                    char whyBuf[64]{};
                    strncpy_s(whyBuf, gDeferredWhy[0] ? gDeferredWhy : "deferred", _TRUNCATE);
                    gDeferred.store(false, std::memory_order_release);
                    LogLine("deferred soft fire why=%s inMap=0", whyBuf);
                    KickLogLine("deferred soft fire why=%s inMap=0", whyBuf);
                    RequestAttempt(whyBuf);
                }
            }
            if (!inMap && !softBusy) {
                clearedCacheInMap = false;
                if (now - lastDlgScrapeMs >= 2500 && !SoftShouldDeferPumpWork()) {
                    lastDlgScrapeMs = now;
                    SoftPumpCall(&ScrapeLoginDialogsOnPump, nullptr, 600);
                }
                if (now - lastOffScrapeMs >= 1500) {
                    lastOffScrapeMs = now;
                    ScrapeCachedDialogsOffPump();
                }
            } else if (inMap && !clearedCacheInMap) {
                ScrapeCachedDialogsOffPump();
                clearedCacheInMap = true;
            }
            using x::features::ports::world::SceneState;
            // 空闲自愈：High 泵上轮询断线窗是否还在。不看 SoftShouldDeferPumpWork
            //（大厅/InterStage 泵 300ms 不新鲜会饿死，本机 BIN 01:23:52 就是这样干坐）。
            // 文案命中断线/登出，或 Notice/LoginUtil 可见且不在 Field → 连续 ≥2s 拉软重连。
            if (!softBusy && !gPending.load(std::memory_order_acquire) &&
                !SoftSceneIsMarket(nullptr) && now - lastDlgProbeMs >= kDlgProbeGapMs) {
                lastDlgProbeMs = now;
                ProbeDlgCtx probe{};
                if (SoftPumpCall(&ProbeLiveKickDialogOnPump, &probe, 600)) {
                    const bool healHint = probe.kickish > 0 ||
                                          (probe.notice > 0 && scene != SceneState::Field);
                    if (healHint && (!lastDlgProbeLogMs || now - lastDlgProbeLogMs >= 5000)) {
                        lastDlgProbeLogMs = now;
                        LogLine("dialog_poll %s scene=%d inMap=%d", probe.detail,
                                static_cast<int>(scene), inMap ? 1 : 0);
                        KickLogLine("dialog_poll %s scene=%d", probe.detail,
                                    static_cast<int>(scene));
                    }
                    if (healHint && !inMap && !IsLandQuiet() &&
                        !gDeferred.load(std::memory_order_acquire) &&
                        x::features::auto_enter::IsDesired() &&
                        (x::features::auto_enter::IsDone() ||
                         x::features::auto_enter::IsFailed())) {
                        if (!dlgHealSinceMs) dlgHealSinceMs = now ? now : 1;
                        if (dlgHealSinceMs && now - dlgHealSinceMs >= kDlgLingerHealMs &&
                            now - lastStuckLobbyMs >= 2000) {
                            lastStuckLobbyMs = now;
                            dlgHealSinceMs = 0;
                            LogLine("dialog_linger RequestAttempt %s scene=%d inMap=%d",
                                    probe.detail, static_cast<int>(scene), inMap ? 1 : 0);
                            KickLogLine("dialog_linger RequestAttempt %s", probe.detail);
                            RequestAttempt("dialog_linger");
                        }
                    } else if (!healHint) {
                        dlgHealSinceMs = 0;
                    }
                }
            }
            // WM 大厅兜底：已经进过图（Done/Failed）又回到登录 / 卡在 InterStage。
            // 本机 BIN 01:23:52 真回大厅后 scene 停在 InterStage=1，从未打到 Login=2；
            // 只认 Login 会漏。拍卖/商城是 4/5。冷启 WaitWorldList 的 IsDone=0 不抢。
            const bool hallLike = scene == SceneState::Login ||
                                  (scene == SceneState::InterStage && !inMap);
            // 超级赶路贴门后必经 InterStage≈2s（客户 20:13 west00 后 scene=1）。
            // 旧逻辑 2s 当 stuck_lobby → Yield 掐 Travel → 过路图当挂机图 → INVULN_OFF。
            if (hallLike && x::features::travel::IsActive()) {
                loginHallSinceMs = 0;
            } else if (!softBusy && !IsLandQuiet() && !gPending.load(std::memory_order_acquire) &&
                !gDeferred.load(std::memory_order_acquire) &&
                x::features::auto_enter::IsDesired() &&
                (x::features::auto_enter::IsDone() || x::features::auto_enter::IsFailed()) &&
                hallLike) {
                if (!loginHallSinceMs) loginHallSinceMs = now ? now : 1;
                if (loginHallSinceMs && now - loginHallSinceMs >= 2000 &&
                    now - lastStuckLobbyMs >= 2000) {
                    lastStuckLobbyMs = now;
                    LogLine("stuck_lobby detect scene=%d inMap=%d done=%d fail=%d — "
                            "RequestAttempt",
                            static_cast<int>(scene), inMap ? 1 : 0,
                            x::features::auto_enter::IsDone() ? 1 : 0,
                            x::features::auto_enter::IsFailed() ? 1 : 0);
                    KickLogLine("stuck_lobby RequestAttempt scene=%d inMap=%d",
                                static_cast<int>(scene), inMap ? 1 : 0);
                    RequestAttempt("stuck_lobby");
                }
            } else {
                loginHallSinceMs = 0;
            }
        }
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
        // Finish 用它兑现最小 hold 时长（守护 SHM 500ms/拍）。
        {
            const DWORD beginMs = GetTickCount();
            gAttemptBeginMs.store(beginMs ? beginMs : 1, std::memory_order_release);
        }
    gInMapRecoverSinceMs.store(0, std::memory_order_release);
        gInMapRecoverDropSinceMs.store(0, std::memory_order_release);
        gLastRefuseLogMs.store(0, std::memory_order_release);
        EnsureNoticeAbsHook();
        const bool settleFast = gSettleProactiveFast.load(std::memory_order_acquire);
        const DWORD settleMs = settleFast ? kSettleProactiveMs : kSettleMs;
        LogLine("attempt begin why=%s settle=%ums proactive=%d hold=1 softCycles=%d doneRestarts=%d",
                gWhy, static_cast<unsigned>(settleMs), settleFast ? 1 : 0, kSoftCycleMax,
                kDoneNoPlayMaxRestarts);
        KickLogLine("attempt begin why=%s hold=1 softCycles=%d", gWhy, kSoftCycleMax);
        // 拍卖/商城迁服照样要接下 hold（否则守护按踢线干净重拉），但别弹「试连中」——
        // 用户只是点了拍卖，没被踢。收尾那条 in_market 气泡会把原委讲清楚。
        if (!SoftSceneIsMarket(nullptr)) {
            char body[160]{};
            snprintf(body, sizeof(body), "why=%s · 推迟守护重拉", gWhy[0] ? gWhy : "disconnect");
            x::features::notify::PublishNotification(x::features::notify::NotificationEvent{
                x::features::notify::NotificationKind::Info, "soft-login-try", "软重连试连中",
                body, 5000});
        }
        x::features::galaxy_token_probe::RequestSample("pre_soft_login");

        int softCycle = 1;
        int emptyHallStreak = 0;
        int limboStreak = 0;
        int charSelectCloseTried = 0;
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
        int connectingStreak = 0;
        int emptyHallConnect = 0;
        int limboRounds = 0;
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
        // lost_session 图内 blip / 误武装：已在图则直接 success，勿拆大厅。
        // disconnected / close_session_inmap：MapScene 残留也不得假成功（BIN 17:29 守护误杀）。
        // 残留期间禁止 ConnectLogin / 进大厅（BIN 00:14:59 叠登 → 已登出 + avatars=0）。
        if (SoftFinishIfAlreadyInMap("cycle_begin")) goto next;
        if (WaitInMapRecoverIfPending("cycle_begin")) goto next;
        {
            const int left = WaitLeaveMapOrRecover("cycle_begin");
            if (left == 1) goto next;
            if (left < 0) {
                Finish(2,
                       "RESULT fail inmap_linger_no_connect — guardian relaunch (avoid 叠登)");
                goto next;
            }
        }

        // 断线 Notice 往往立刻弹出；勿等 Connected 才关。
        // 墙钟截止 + Connecting/Connected 早退（302081）。
        // 大厅减负（d29b56 后）：settle 内 **最多 1 次** FindAll dismiss，其余轮只 Sample NM；
        // 勿每 120ms 叠 Util/Ex/Notice（Bootstrap 半死时易挤 ConnectLogin）。
        // scanBase=1：Notice 白名单；永不 FindAll(UIDialog 基类)。
        {
            const bool proactiveFast = gSettleProactiveFast.load(std::memory_order_acquire);
            const DWORD settleCap = proactiveFast ? kSettleProactiveMs : kSettleMs;
            const DWORD settleDeadline = GetTickCount() + settleCap;
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
                        if (early.closed > 0 || early.inactivated > 0) {
                            settleDismissDone = true;
                            if (proactiveFast) {
                                LogLine("settle early-exit proactive dismiss_done remain≈%dms",
                                        static_cast<int>(settleDeadline - GetTickCount()));
                                KickLogLine("settle early_proactive dismiss_done");
                                break;
                            }
                        }
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
                const int im = SoftProbeInMapOrFinish("login_ui_ready_post_settle");
                if (im == 1) goto next;
                if (im < 0) {
                    if (WaitInMapRecoverIfPending("login_ui_ready_post_settle")) goto next;
                    LogLine("login_ui_ready post-settle blocked inMap linger — no auto_enter");
                    KickLogLine("login_ui_ready blocked inMap where=post_settle");
                } else {
                    LogLine("login_ui_ready post-settle world=%d ch=%d items=%d — skip ConnectLogin "
                            "arm reenter",
                            peek.worldUi, peek.channelUi, peek.worldItems);
                    KickLogLine("resume_login_ui skip_connect world=%d ch=%d items=%d where=post_settle",
                                peek.worldUi, peek.channelUi, peek.worldItems);
                    invokeOk = true;
                    goto connected_path;
                }
            }
        }

        connectPumpFail = 0;
        connectingStreak = 0;
        emptyHallConnect = 0;
        limboRounds = 0;
        for (int t = 0; t < kConnectWaitRounds && !gStop.load(); ++t) {
            // 每轮复查：断线边沿常早于场景切换落定，cycle_begin 那一次未必看得到 scene=5；
            // 且用户可能在 attempt 途中才点开拍卖。必须赶在 ConnectLogin 之前站住，
            // 否则会把人从拍卖行拽回登录流程。放在泵 idle 分支之前 —— 场景态是 off-pump
            // 直读（与本 worker 其它路径的 IsInMapScene 同口径），泵不新鲜时照样判得出。
            if (SoftFinishIfInMarket("connect_wait")) goto next;
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
                // 复位必须与 SampleLooksLimbo 的逃逸口一致：只认「状态在动」。
                // 勿改回按 nmOk 复位 —— ResolveNmFacadeOnPump 宽松，中间态下它也返回非空，
                // 那样每轮先清零再自增，limboRounds 永远到不了阈值，判死出口成死代码。
                if (sample.state == kStateConnected || sample.state == kStateConnecting) {
                    limboRounds = 0;
                }
                if (sample.state == kStateConnected) {
                    if (LoginUiReady(sample)) {
                        const int im = SoftProbeInMapOrFinish("login_ui_ready");
                        if (im == 1) goto next;
                        if (im < 0) {
                            if (WaitInMapRecoverIfPending("login_ui_ready")) goto next;
                            LogLine("login_ui_ready blocked inMap linger try=%d — no auto_enter",
                                    t);
                            KickLogLine("login_ui_ready blocked inMap try=%d", t);
                            Sleep(kConnectWaitMs);
                            continue;
                        }
                        LogLine("login_ui_ready world=%d ch=%d items=%d try=%d — skip ConnectLogin",
                                sample.worldUi, sample.channelUi, sample.worldItems, t);
                        KickLogLine("resume_login_ui skip_connect world=%d ch=%d items=%d try=%d",
                                    sample.worldUi, sample.channelUi, sample.worldItems, t);
                        invokeOk = true;
                        goto connected_path;
                    }
                    // BIN 01:53：Connected+items=0 时 ConnectLogin 空转；Disconnect 只自连回
                    // Connected。禁止在 Connected 上 ConnectLogin。
                    // BIN 02:12：图内 SoftHall 也是 items=0 — 先认 inMap，绝不硬拆。
                    // BIN 15:40：CloseSession 硬拆 →「已登出登入的帳號」；改等多等 / soft cycle。
                    {
                        const int im = SoftProbeInMapOrFinish("connected_empty_hall");
                        if (im == 1) goto next;
                        if (im < 0) {
                            if (WaitInMapRecoverIfPending("connected_empty_hall")) goto next;
                            LogLine("NM Connected items=0 try=%d — defer (in_map unknown/pump)", t);
                            KickLogLine("connect_wait empty_hall_defer try=%d", t);
                            Sleep(kConnectWaitMs);
                            continue;
                        }
                    }
                    ++emptyHallConnect;
                    if (emptyHallConnect >= kEmptyHallConnectWaitRounds) {
                        LogLine("NM Connected hall empty streak=%d/%d try=%d items=%d — soft "
                                "cycle (no CloseSession)",
                                emptyHallConnect, kEmptyHallConnectWaitRounds, t,
                                sample.worldItems);
                        KickLogLine("connect_wait empty_hall_soft_cycle try=%d items=%d", t,
                                    sample.worldItems);
                        char failHall[200]{};
                        snprintf(failHall, sizeof(failHall),
                                 "RESULT fail hall_world_items_empty why=%s where=connect_wait — "
                                 "soft cycle or release hold",
                                 gWhy);
                        if (SoftFailOrRetry(softCycle, failHall, &emptyHallStreak)) {
                            ++softCycle;
                            goto soft_cycle_begin;
                        }
                        goto next;
                    }
                    if ((emptyHallConnect % 8) == 1) {
                        LogLine("NM Connected hall not ready world=%d ch=%d items=%d try=%d "
                                "empty=%d/%d — wait (no CloseSession)",
                                sample.worldUi, sample.channelUi, sample.worldItems, t,
                                emptyHallConnect, kEmptyHallConnectWaitRounds);
                        KickLogLine("connect_wait empty_hall_wait try=%d empty=%d", t,
                                    emptyHallConnect);
                    }
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
                    ++connectingStreak;
                    if (connectingStreak >= kConnectingStallRounds) {
                        // BIN 04:20：Connecting 空转数秒后仍 ConnectLogin 叠登 → 易变 Disconnected。
                        LogLine("connect-wait Connecting stall try=%d streak=%d — poll early", t,
                                connectingStreak);
                        KickLogLine("connect_wait Connecting_stall try=%d", t);
                        break;
                    }
                    // Connecting：自连优先，不抢泵做 dismiss。
                    Sleep(kConnectWaitMs);
                    continue;
                }
                connectingStreak = 0;
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
                // 中间态：SceneLogin 不存在且游戏没在自连 —— 重试没有着力点，攒满即判死。
                if (SampleLooksLimbo(sample, ctx.detail)) {
                    if (++limboRounds >= kLimboConnectRounds) {
                        // 先排除「其实已在图」：图内放 hold 会让守护杀掉好端端的客户端
                        // （BIN 17:29 误杀）。读不出来时退几轮再探，不拿未知判死。
                        const int im = SoftProbeInMapOrFinish("limbo");
                        if (im == 1) goto next;
                        if (im < 0) {
                            LogLine("limbo hold try=%d rounds=%d — defer (in_map unknown/pump)", t,
                                    limboRounds);
                            KickLogLine("connect_wait limbo_defer try=%d", t);
                            // 退 8 轮（~2s）再重探：in_map 探测要占泵，别每 250ms 打一次。
                            limboRounds = kLimboConnectRounds - 8;
                            Sleep(kConnectWaitMs);
                            continue;
                        }
                        char failLimbo[240]{};
                        snprintf(failLimbo, sizeof(failLimbo),
                                 "RESULT fail connect_wait_limbo why=%s rounds=%d state=%d nmOk=%d "
                                 "last=%s try=%d — 客户端卡在登录/地图之间，软重连无入口",
                                 gWhy, limboRounds, sample.state, sample.nmOk, ctx.detail, t);
                        KickLogLine("RESULT fail connect_wait_limbo rounds=%d state=%d", limboRounds,
                                    sample.state);
                        if (SoftFailOrRetry(softCycle, failLimbo, &emptyHallStreak, &limboStreak)) {
                            ++softCycle;
                            goto soft_cycle_begin;
                        }
                        goto next;
                    }
                    if ((limboRounds % 24) == 1) {
                        LogLine("limbo watch try=%d rounds=%d/%d state=%d nmOk=%d (%s)", t,
                                limboRounds, kLimboConnectRounds, sample.state, sample.nmOk,
                                ctx.detail);
                        KickLogLine("connect_wait limbo_watch rounds=%d/%d state=%d", limboRounds,
                                    kLimboConnectRounds, sample.state);
                    }
                } else {
                    limboRounds = 0;
                }
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
        int discPoll = 0;
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
            if (sample.state == kStateDisconnected || sample.state == kStateDisconnecting) {
                ++discPoll;
                emptyHallPoll = 0;
                if (discPoll >= kDiscPollFailRounds) {
                    LogLine("poll[%d] Disconnected streak=%d/%d — soft cycle early", i, discPoll,
                            kDiscPollFailRounds);
                    KickLogLine("poll_disc_early streak=%d", discPoll);
                    break;
                }
                continue;
            }
            discPoll = 0;
            if (sample.state == kStateConnected) {
                KickLogLine("RESULT connected rounds=%d world=%d ch=%d items=%d ready=%d", i + 1,
                            sample.worldUi, sample.channelUi, sample.worldItems, sample.hallReady);
                if (LoginUiReady(sample)) {
                    const int im = SoftProbeInMapOrFinish("poll_login_ui");
                    if (im == 1) goto next;
                    if (im < 0) {
                        if (WaitInMapRecoverIfPending("poll_login_ui")) goto next;
                        LogLine("poll login_ui blocked inMap linger rounds=%d — no auto_enter",
                                i + 1);
                        KickLogLine("poll login_ui blocked inMap rounds=%d", i + 1);
                        continue;
                    }
                    LogLine("NM Connected+login_ui after soft path why=%s rounds=%d world=%d ch=%d "
                            "items=%d hallReady=1",
                            gWhy, i + 1, sample.worldUi, sample.channelUi, sample.worldItems);
                    goto connected_path;
                }
                // BIN 01:53：空等满 poll；满 kEmptyHallPollRounds 后 soft cycle（不 CloseSession）。
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
                const int im = SoftProbeInMapOrFinish("poll_empty_hall");
                if (im == 1) goto next;
                if (im < 0) {
                    if (WaitInMapRecoverIfPending("poll_empty_hall")) goto next;
                    LogLine("poll end Connected items=%d — defer soft cycle (in_map unknown)",
                            last.worldItems);
                    KickLogLine("poll_end empty_hall_defer");
                    Sleep(kConnectWaitMs);
                    if (SoftProbeInMapOrFinish("poll_empty_hall_retry") == 1) goto next;
                    char failDefer[200]{};
                    snprintf(failDefer, sizeof(failDefer),
                             "RESULT fail hall_world_items_empty why=%s where=poll_defer — soft "
                             "cycle or release hold",
                             gWhy);
                    if (SoftFailOrRetry(softCycle, failDefer, &emptyHallStreak)) {
                        ++softCycle;
                        goto soft_cycle_begin;
                    }
                    goto next;
                }
                LogLine("poll end Connected items=%d — soft cycle (no CloseSession)",
                        last.worldItems);
                KickLogLine("poll_end Connected_empty_hall soft_cycle");
                if (kAllowEmptyHallCloseSession) SoftForceNmTeardown("poll_empty_hall");
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
                {
                    const int im = SoftProbeInMapOrFinish("connected_path");
                    if (im == 1) goto next;
                    if (im < 0) {
                        if (WaitInMapRecoverIfPending("connected_path")) goto next;
                        LogLine("connected_path blocked inMap linger — no auto_enter");
                        KickLogLine("connected_path blocked inMap");
                        ++softCycle;
                        if (softCycle > kSoftCycleMax) {
                            Finish(2,
                                   "RESULT fail inmap_linger_no_connect — guardian relaunch "
                                   "(avoid 叠登)");
                            goto next;
                        }
                        goto soft_cycle_begin;
                    }
                }
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
                    LogLine("dismiss_miss — no active UIUtilDialog; retry LoginUtil/Notice once");
                    KickLogLine("dismiss_miss");
                    // BIN 17:43：Util 已关/池实例 alreadyOff，真窗在 UILoginUtilDialog；补一枪白名单。
                    DismissCtx again{};
                    again.aggressive = 1;
                    again.scanBase = 1;
                    if (SoftPumpCall(&DismissKickDialogOnPump, &again, kDismissCallMs)) {
                        LogLine("dismiss_retry %s", again.detail);
                        KickLogLine("dismiss_retry %s", again.detail);
                        dismissHits += again.scanned + again.inactivated + again.closed +
                                       again.destroyed;
                    }
                    if (dismissHits <= 0)
                        LogLine("dismiss_miss — continue reenter anyway");
                }

                // BIN 22:47:45 skip 时 Notice 仍在就 GoWorld → avatars=0；成功轮此时 notice=0。
                if (DirtyHallNoticeBlocksEnter("connected_path")) {
                    Finish(2,
                           "RESULT fail logged_out_hall_notice — guardian relaunch (no GoWorld)");
                    goto next;
                }

                // 分区列表未刷出就 RequestRestart → auto_enter 卡 waiting WorldItems?（BIN 21:44）。
                const int hallPick = WaitHallPickReady(kHallReadyWaitMs);
                if (hallPick < 0) goto next;
                if (hallPick == 0) {
                    const int im = SoftProbeInMapOrFinish("hall_wait_timeout");
                    if (im == 1) goto next;
                    if (im < 0) {
                        if (WaitInMapRecoverIfPending("hall_wait_timeout")) goto next;
                        LogLine("hall_wait timeout — defer soft cycle (in_map unknown/pump)");
                        KickLogLine("hall_wait empty_hall_defer");
                        Sleep(kConnectWaitMs);
                        if (SoftProbeInMapOrFinish("hall_wait_timeout_retry") == 1) goto next;
                        char failDefer[200]{};
                        snprintf(failDefer, sizeof(failDefer),
                                 "RESULT fail hall_world_items_empty why=%s where=hall_defer — "
                                 "soft cycle or release hold",
                                 gWhy);
                        if (SoftFailOrRetry(softCycle, failDefer, &emptyHallStreak)) {
                            ++softCycle;
                            goto soft_cycle_begin;
                        }
                        goto next;
                    }
                    LogLine("hall_wait timeout — soft cycle (no CloseSession)");
                    KickLogLine("hall_wait empty_hall_soft_cycle");
                    if (kAllowEmptyHallCloseSession) SoftForceNmTeardown("hall_wait_timeout");
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
                    DWORD standLeftMapSinceMs = 0;
                    bool lastSampleInMap = false;
                    int pumpFailStreak = 0;
                    DWORD emptyAvatarSinceMs = 0;
                    DWORD budgetStartMs = GetTickCount();
                    DWORD budgetMs = kReenterBudgetMs;
                    for (int r = 0; !gStop.load(); ++r) {
                        if (GetTickCount() - budgetStartMs >= budgetMs) break;

                        // 重进途中又断线：busy 会吞掉 RequestAttempt（6c3ef8 06:13:48
                        // skip busy），必须在 wait 里自己看见 Disconnected 另开一轮。
                        const int nmSt = x::features::kick_sniff::LastSessionState();
                        if (nmSt == kStateDisconnected || nmSt == kStateDisconnecting) {
                            LogLine("reenter: NM %s during wait round=%d "
                                    "soft_cycle=%d — abort to new cycle",
                                    StateName(nmSt), r, softCycle);
                            KickLogLine("reenter nm_disc abort round=%d cycle=%d", r, softCycle);
                            ++softCycle;
                            if (softCycle > kSoftCycleMax) {
                                Finish(2, "RESULT fail reenter_nm_disc cycles_exhausted");
                                earlyFail = true;
                                break;
                            }
                            goto soft_cycle_begin;
                        }

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

                        // 大厅关窗：重进期间周期性扫 LoginUtil/Notice。
                        // BIN 17:43：「已登出登入的帳號」挡选区；旧逻辑仅 r==0 且 scanBase=0，
                        // 漏掉 UILoginUtilDialog，且窗晚于首枪弹出后永不补关。
                        const bool stillInHall = !x::features::auto_enter::IsDone() &&
                                                 !x::features::auto_enter::IsFailed();
                        if (stillInHall && (r == 0 || (r % 10) == 0) &&
                            !SoftShouldDeferPumpWork()) {
                            DismissCtx again{};
                            again.aggressive = 1;
                            again.scanBase = 1;  // Util/Ex + UILoginUtilDialog / Notice 白名单
                            if (SoftPumpCall(&DismissKickDialogOnPump, &again,
                                                               kDismissCallMs)) {
                                LogLine("dismiss hall r=%d %s", r, again.detail);
                                if (again.closed > 0 || again.inactivated > 0 ||
                                    again.destroyed > 0)
                                    KickLogLine("dismiss hall r=%d %s", r, again.detail);
                            }
                            if (DirtyHallNoticeBlocksEnter("reenter_hall")) {
                                Finish(2,
                                       "RESULT fail logged_out_hall_notice — guardian "
                                       "relaunch (no GoWorld)");
                                earlyFail = true;
                                break;
                            }
                        }

                        if (!x::features::auto_enter::IsDone() &&
                            x::features::auto_enter::CharUiVisible() &&
                            x::features::auto_enter::LastCharAvatarCount() <= 0) {
                            const DWORD nowAv = GetTickCount();
                            if (!emptyAvatarSinceMs) emptyAvatarSinceMs = nowAv ? nowAv : 1;
                            if (emptyAvatarSinceMs &&
                                nowAv - emptyAvatarSinceMs >= kEmptyAvatarFailMs) {
                                LogLine("logged_out empty avatars held=%ums — no CloseSession",
                                        static_cast<unsigned>(nowAv - emptyAvatarSinceMs));
                                KickLogLine("RESULT fail logged_out_empty_avatars held=%ums",
                                            static_cast<unsigned>(nowAv - emptyAvatarSinceMs));
                                Finish(2,
                                       "RESULT fail logged_out_empty_avatars — guardian "
                                       "relaunch (no CloseSession)");
                                earlyFail = true;
                                break;
                            }
                        } else {
                            emptyAvatarSinceMs = 0;
                        }

                        if (x::features::auto_enter::IsFailed()) {
                            const int charStreak =
                                x::features::auto_enter::CharSelectTimeoutStreak();
                            const int avatars =
                                x::features::auto_enter::LastCharAvatarCount();
                            if (charStreak >= 1 && avatars <= 0) {
                                LogLine("logged_out empty avatars streak=%d — no CloseSession",
                                        charStreak);
                                KickLogLine("RESULT fail logged_out_empty_avatars streak=%d",
                                            charStreak);
                                Finish(2,
                                       "RESULT fail logged_out_empty_avatars — guardian "
                                       "relaunch (no CloseSession)");
                                earlyFail = true;
                                break;
                            }
                            if (charStreak >= kCharSelectStuckMax) {
                                const int im =
                                    SoftProbeInMapOrFinish("char_select_stuck");
                                if (im == 1) {
                                    earlyFail = true;
                                    break;
                                }
                                if (im == 0 && charSelectCloseTried == 0) {
                                    LogLine("char_select_stuck streak=%d — CloseSession then "
                                            "reconnect (not guardian yet)",
                                            charStreak);
                                    KickLogLine("char_select_stuck CloseSession streak=%d",
                                                charStreak);
                                    if (SoftForceNmTeardown("char_select_stuck")) {
                                        charSelectCloseTried = 1;
                                        ++softCycle;
                                        if (softCycle > kSoftCycleMax) {
                                            Finish(2,
                                                   "RESULT fail char_select_busy_stuck "
                                                   "cycles_exhausted after CloseSession");
                                            earlyFail = true;
                                            break;
                                        }
                                        goto soft_cycle_begin;
                                    }
                                    LogLine("char_select_stuck CloseSession failed — "
                                            "release hold");
                                }
                                LogLine("char_select_stuck streak=%d closed=%d — "
                                        "release hold for guardian",
                                        charStreak, charSelectCloseTried);
                                KickLogLine("RESULT fail char_select_busy_stuck streak=%d "
                                            "closed=%d",
                                            charStreak, charSelectCloseTried);
                                Finish(2,
                                       "RESULT fail char_select_busy_stuck — release hold "
                                       "for guardian relaunch");
                                earlyFail = true;
                                break;
                            }
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
                        if (play.sampled) lastSampleInMap = play.inMap != 0;
                        if (play.ready) {
                            // 图内须挂台（curFh≠0）再 RESULT；悬空=掉落/热重载循环（ec1fe7）。
                            // 空中贴怪悬停 curFh 一直 0：等挂台只会晚弹成功、再 SafeLand 打断正在打。
                            const bool impactAir = play.inMap && play.curFh == 0 &&
                                                   ImpactAirSkipStand();
                            if (play.inMap && play.curFh == 0 && !impactAir) {
                                awaitingStand = true;
                                sawInMapWhileDone = true;  // 热重载 !inMap 时勿 Done 再启
                                standLeftMapSinceMs = 0;
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
                            if (impactAir) {
                                LogLine("play-ready skip stand wait — impact air curFh=0");
                                KickLogLine("play-ready skip_stand impact_air");
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
                                standLeftMapSinceMs = 0;
                                if (!sawInMapWhileDone) {
                                    sawInMapWhileDone = true;
                                    LogLine("reenter: Done+inMap playReady=0 — wait alive, "
                                            "no RequestRestart");
                                    KickLogLine("reenter done_in_map wait_alive");
                                }
                                doneSinceMs = 0;  // 不走 Done 再启时钟
                            } else if (play.sampled && !play.inMap) {
                                if (awaitingStand) {
                                    if (!standLeftMapSinceMs) standLeftMapSinceMs = now ? now : 1;
                                    if (standLeftMapSinceMs &&
                                        now - standLeftMapSinceMs >= kStandLeftMapAbortMs) {
                                        LogLine("reenter: left map during stand_wait held=%ums "
                                                "— clear await, RequestRestart",
                                                static_cast<unsigned>(now - standLeftMapSinceMs));
                                        KickLogLine("reenter stand_left_map abort");
                                        awaitingStand = false;
                                        sawInMapWhileDone = false;
                                        standSinceMs = 0;
                                        standLeftMapSinceMs = 0;
                                        if (doneRestartCount < kDoneNoPlayMaxRestarts) {
                                            ++doneRestartCount;
                                            x::features::auto_enter::RequestRestart("soft_login");
                                            doneSinceMs = now;
                                            budgetStartMs = now;
                                            budgetMs = kReenterBudgetMs;
                                        }
                                    } else if ((r % 10) == 0) {
                                        LogLine("reenter wait[%d] stand_wait map_transit "
                                                "!inMap held=%ums — wait leave confirm",
                                                r,
                                                static_cast<unsigned>(now - standLeftMapSinceMs));
                                    }
                                    doneSinceMs = 0;
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
                        // 只在**此刻仍在图**才降级成功。闪进图又掉回大厅后 sawInMapWhileDone
                        // 仍真（6c3ef8）：旧逻辑谎报 success，守护 absorb，人坐在断线窗上。
                        if (sawInMapWhileDone && lastSampleInMap) {
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

void ClearBreaker(const char* why) { ClearBreakerLocked(why); }

void Init() {
    if (IsArmed()) {
        LogLine("armed — will try SceneLogin ConnectLogin after disconnect (hold defers guardian)");
        KickLogLine("armed");
        EnsureNoticeAbsHook();  // 登录即可能出封禁/踢线窗，勿等 RequestAttempt
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
        EnsureNoticeAbsHook();
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

// 已 PlayReady 且 CurFh≠0：立刻早释 quiet/air。未挂台仍走 cap。
static void MaybeEarlyReleaseQuietOnFh() {
    const DWORD until = gLandQuietUntilMs.load(std::memory_order_acquire);
    const DWORD air = gPostSoftAirUntilMs.load(std::memory_order_acquire);
    if (!until && !air) return;
    if (!x::features::ports::world::IsPlayReady()) return;
    if (x::features::ports::foothold::PeekCurFhId() == 0) return;
    const DWORD armed = gLandQuietArmedAtMs.load(std::memory_order_acquire);
    const DWORD now = GetTickCount();
    const DWORD elapsed = armed ? (now - armed) : 0;
    if (until) gLandQuietUntilMs.store(0, std::memory_order_release);
    if (air) gPostSoftAirUntilMs.store(0, std::memory_order_release);
    LogLine("land_quiet early_release onFh elapsed=%ums", static_cast<unsigned>(elapsed));
    KickLogLine("land_quiet early_release onFh elapsed=%ums", static_cast<unsigned>(elapsed));
}

bool IsLandQuiet() {
    MaybeEarlyReleaseQuietOnFh();
    DWORD until = gLandQuietUntilMs.load(std::memory_order_acquire);
    if (!until) return false;
    const DWORD now = GetTickCount();
    if (static_cast<int>(until - now) > 0) return true;
    (void)gLandQuietUntilMs.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
    return false;
}

bool IsPostSoftAirCombatBlocked() {
    MaybeEarlyReleaseQuietOnFh();
    DWORD until = gPostSoftAirUntilMs.load(std::memory_order_acquire);
    if (!until) return false;
    const DWORD now = GetTickCount();
    if (static_cast<int>(until - now) > 0) return true;
    (void)gPostSoftAirUntilMs.compare_exchange_strong(until, 0, std::memory_order_acq_rel);
    return false;
}

bool IsGameplayQuiet() { return IsHoldActive() || IsLandQuiet(); }

unsigned ResultCode() { return gResult.load(std::memory_order_acquire); }

void RequestDeferredAttempt(const char* why) {
    if (!IsArmed()) return;
    if (BreakerActive()) {
        LogLine("deferred skip breaker why=%s", why ? why : "?");
        return;
    }
    if (gBusy.load(std::memory_order_acquire) || gPending.load(std::memory_order_acquire)) {
        LogLine("deferred skip busy/pending why=%s", why ? why : "?");
        return;
    }
    if (x::features::channel_hop::IsMigrateInFlight()) {
        LogLine("deferred skip hop migrate why=%s", why ? why : "?");
        KickLogLine("deferred skip hop migrate why=%s", why ? why : "?");
        return;
    }
    // land_quiet 期间也允许武装：worker 仍等 quiet 结束才 fire。
    // 本机 BIN 01:23:52：RESULT success 后 680ms 真 Disconnected，MapScene 还残留，
    // 旧逻辑整票丢掉；400ms 后已是 InterStage 大厅，没人再拉。
    memset(gDeferredWhy, 0, sizeof(gDeferredWhy));
    strncpy_s(gDeferredWhy, why ? why : "deferred", _TRUNCATE);
    gDeferredSinceMs.store(GetTickCount(), std::memory_order_release);
    gDeferred.store(true, std::memory_order_release);
    x::features::ports::mob_gather::NoteNmSessionEnded(gDeferredWhy);
    LogLine("deferred soft arm why=%s (wait !inMap or hall)", gDeferredWhy);
    KickLogLine("deferred soft arm why=%s", gDeferredWhy);
}

bool IsDeferredPending() { return gDeferred.load(std::memory_order_acquire); }

bool IsAttemptBusy() { return gBusy.load(std::memory_order_acquire); }

bool IsReconnectInFlight() {
    return IsHoldActive() || IsLandQuiet() || IsAttemptBusy() || IsDeferredPending() ||
           gPending.load(std::memory_order_acquire);
}

bool RequestProactiveReconnect(const char* why) {
    using x::features::ports::world::GetSceneState;
    using x::features::ports::world::IsInMapScene;
    using x::features::ports::world::IsPlayReady;
    using x::features::ports::world::SceneState;
    if (!IsArmed()) {
        LogLine("proactive skip not_armed why=%s", why ? why : "?");
        KickLogLine("proactive skip not_armed");
        return false;
    }
    if (BreakerActive()) {
        LogLine("proactive skip breaker why=%s", why ? why : "?");
        return false;
    }
    const bool hangupFires = why && std::strcmp(why, "hangup_fires") == 0;
    if (IsReconnectInFlight()) {
        // 图内假 recover（stuck_lobby）占着 in_flight 时，出刀闸仍必须拆会话。
        if (!hangupFires || !IsInMapScene() || !IsPlayReady()) {
            LogLine("proactive skip in_flight why=%s", why ? why : "?");
            return false;
        }
        LogLine("proactive hangup_fires while in_flight — CloseSession anyway");
        KickLogLine("proactive hangup_fires in_flight CloseSession");
        if (!SoftCloseSessionInField(why)) return false;
        gSettleProactiveFast.store(true, std::memory_order_release);
        LogLine("proactive close issued why=%s (in_flight, wait !inMap)", why);
        KickLogLine("proactive close issued why=%s", why);
        return true;
    }
    const bool sceneOk = WhyIsHangupClose(why)
                             ? (IsInMapScene() && IsPlayReady())
                             : (GetSceneState() == SceneState::Field && IsPlayReady());
    if (!sceneOk) {
        LogLine("proactive skip not_field scene=%d inMap=%d play=%d why=%s",
                static_cast<int>(GetSceneState()), IsInMapScene() ? 1 : 0,
                IsPlayReady() ? 1 : 0, why ? why : "?");
        return false;
    }
    // 先粘性：拆会话后 kick_sniff 也可能再写 close_session_inmap。图内不 SetHold。
    RequestDeferredAttempt(why ? why : "proactive");
    if (!SoftCloseSessionInField(why ? why : "proactive")) {
        if (x::features::ports::world::IsInMapScene()) {
            gDeferred.store(false, std::memory_order_release);
            LogLine("proactive close fail — drop deferred (still inMap)");
        }
        return false;
    }
    gSettleProactiveFast.store(true, std::memory_order_release);
    LogLine("proactive close issued why=%s (deferred, wait !inMap)", why ? why : "?");
    KickLogLine("proactive close issued why=%s", why ? why : "?");
    return true;
}

void RequestAttempt(const char* why) {
    if (!IsArmed()) return;
    // 熔断期不接管：早退在 SetHold 之前，hold 保持 0，守护得以干净重拉。
    if (BreakerActive()) {
        LogLine("request skip breaker why=%s (guardian may clean relaunch)", why ? why : "?");
        KickLogLine("request skip breaker why=%s", why ? why : "?");
        return;
    }
    // hop 已发包：KickSniff / CloseSession / stuck_lobby 边沿交给 hop Fail 后再拉。
    // channel_hop_timeout / FailThenSoft（已 Idle）不会命中。
    if (why && x::features::channel_hop::IsMigrateInFlight() &&
        (std::strcmp(why, "disconnected") == 0 || std::strcmp(why, "disconnecting") == 0 ||
         std::strcmp(why, "lost_session") == 0 || std::strcmp(why, "close_session_inmap") == 0 ||
         std::strcmp(why, "stuck_lobby") == 0 || std::strcmp(why, "dialog_linger") == 0 ||
         std::strcmp(why, "nm_gone_inmap") == 0)) {
        LogLine("request skip hop migrate in flight why=%s", why);
        KickLogLine("request skip hop migrate why=%s", why);
        return;
    }
    // BIN 16:05：图内药店 Session 空窗 why=lost_session → hold 闪一下像强制软重连。
    // 真回大厅由 stuck_lobby / Disconnected 边沿 / RequestDeferredAttempt 处理；
    // lost_session 在图内一律吞掉。nm_gone_inmap 是「丢失满 3s」的真死会话，不走这条吞。
    if (why && std::strcmp(why, "lost_session") == 0 &&
        x::features::ports::world::IsInMapScene()) {
        LogLine("request skip lost_session inMap");
        KickLogLine("request skip lost_session inMap");
        return;
    }
    // 正式 attempt 接管粘性票。
    gDeferred.store(false, std::memory_order_release);
    // 进行中的 soft 自拆 CloseSession / 进图 Session 抖动会再抬 Disconnected；
    // 再排队 → RESULT 后立刻 attempt begin（BIN 02:12）。本轮 soft 自己会处理断线。
    if (gBusy.load(std::memory_order_acquire)) {
        LogLine("request skip busy why=%s", why ? why : "?");
        KickLogLine("request skip busy why=%s", why ? why : "?");
        return;
    }
    // Finish 已放 hold。落地静默只吞 Session 闪断（lost_session）；真 Disconnected
    // 常还带着 MapScene 残留，400ms 后才变 InterStage——整票 skip 会把人扔在大厅
    //（本机 BIN 01:23:51 success → 01:23:52 skip land_quiet → 再无 attempt）。
    if (IsLandQuiet()) {
        // 与 WaitLeaveMap 同一张 why 表：漏一项就会在落地静默里 skip，叠登窗口重开。
        if (WhyStringNeedsRealReconnect(why)) {
            RequestDeferredAttempt(why);
            LogLine("request defer land_quiet why=%s (wait leave-map / quiet end)", why);
            KickLogLine("request defer land_quiet why=%s", why);
        } else {
            LogLine("request skip land_quiet why=%s", why ? why : "?");
            KickLogLine("request skip land_quiet why=%s", why ? why : "?");
        }
        return;
    }
    // 判定量先于裸缓冲落定：worker 只读 gWhyNeedsReconnect，不会撞上 memset 中间态。
    gWhyNeedsReconnect.store(WhyStringNeedsRealReconnect(why ? why : "disconnect"),
                             std::memory_order_release);
    memset(gWhy, 0, sizeof(gWhy));
    strncpy_s(gWhy, why ? why : "disconnect", _TRUNCATE);
    x::features::ports::mob_gather::NoteNmSessionEnded(gWhy);
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
