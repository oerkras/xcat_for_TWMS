// Classic TWMS travel — 本板块 seed BFS goto + teleport/直调进门
// 对照枫星：假火软确认 / 唯一桥 / 按门名重解 / PlayReady·换图稳图闸（无码头、无 crawl）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "travel.h"

#include "travel_graph.h"
#include "../ports/travel_port.h"
#include "../ports/fly_fh_ban.h"
#include "../ports/foothold_path.h"
#include "../ports/teleport_port.h"
#include "../ports/world_port.h"
#include "../invuln/invuln.h"
#include "../simple_combat/heli_rotor.h"
#include "../soft_login_probe/soft_login_probe.h"
#include "../kick_sniff/kick_sniff.h"
#include "../notify/notify.h"
#include "../simple_combat/simple_combat.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include "../../../common/xcat_map_names.h"
#include "../../../common/xcat_map_towns.h"

#include <Windows.h>
#include <timeapi.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace x::features::travel {
namespace {

constexpr bool kEnabled = true;
constexpr DWORD kTickMs = 50;
// BIN 11:44–11:47：真换图多在 FIRED 后 <300ms；假火则空等满窗才重试。
// 旧 8s/13s 把「CheckMove 未命中」误当成「服端慢换图」，多跳路径每假火空等 ~13s。
constexpr DWORD kHopWaitMs = 1200;
// 唯一桥假火空等：1500 时首跳常 FIRED→soft≈1.5s 体感卡 2s（BIN 17:47）。
// 仍靠 leave-from 认换图；过短易把慢进图误判 soft（可再调）。
constexpr DWORD kHopWaitUniqueBridgeMs = 700;
// BIN 15:57 回挂机：换图后立刻开火会撞 InterStage 闪黑屏；稳图再下一跳。
// BIN 02:34：hop1 Field 闪回后 1.5s 就 hop2 stick，第二跳 Up 半截进门卡死 → 曾拉到 2500。
// 体感每跳「准备很久」：2500→1200（仍 ≥ PlayReadyStable 500；过短再加回）。
constexpr DWORD kMidHopSettleMs = 1200;         // 换图后稳图再下一跳
// 首跳（Goto 起点、未换图）同样要稳图：BIN 首枪常假火；禁止只靠 PlayReadyStable(500)。
// BIN 18:47：世界地图 YesNo 后 ~1.2s 就 ↑，前两枪 timeout until=0；加长到 1200 给关图后进门窗。
constexpr DWORD kFirstHopSettleMs = 1200;
constexpr DWORD kPlayReadyStableMs = 500;       // 连续 PlayReady / Field 才开火 / 认到站
constexpr DWORD kArriveStableMs = 1500;         // 到目标图后再稳一会才 Idle（防到站二次卸图）
// BIN 8fa033：稳图计时到了仍 curFh=0 就 Idle → 卸旋翼 freefall；再托一会等 onFh。
constexpr DWORD kArriveLandExtraMs = 2000;
constexpr DWORD kHopLandExtraMs = 800;
// BIN 01:48：FIRED_STICK_UP 后 uniqueBridge 700ms 就 soft 补 ↑，第二枪撞半截换图 → 黑屏。
// 任意开火后禁补枪静默窗（MapId 未变也不许 soft 重试）。
constexpr DWORD kPostFireQuietMs = 2500;
// WM 状态机：进 InterStage / MapId 已闪变后等 Field；超时则停（客户端已卡死）。
constexpr DWORD kInterStageStuckMs = 12000;
constexpr DWORD kUniqueBridgeLateGraceMs = 10000;
constexpr int kFakeFireSoftConfirm = 2;          // 未满：只重试，不标死
constexpr int kFakeFireFuseConfirm = 3;          // 满：停赶路
constexpr int kFakeFireUniqueBridgeConfirm = 3;  // 唯一桥永不标死，满则停
constexpr DWORD kTransientFireFuseMs = 8000;     // TELEPORT_FAIL 等瞬态空转超时 → FireStuck+Notify
static_assert(kFakeFireSoftConfirm < kFakeFireFuseConfirm, "soft < fuse");

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::mutex gMu;

Graph gGraph;
bool gSeedLoaded = false;

enum class Mode { Idle, Goto };
Mode gMode = Mode::Idle;
uint32_t gFireEpoch = 0;  // SetIdle/新 goto 递增；开火放锁后对不上则丢弃提交
std::string gTarget;
std::vector<Hop> gHops;
size_t gHopIdx = 0;
DWORD gHopStartedMs = 0;
DWORD gNextHopReadyAt = 0;  // mid-hop 稳图后再发下一跳
DWORD gPlayReadySince = 0;  // 连续 PlayReady 起点；掉就绪清零
std::string gStableMapKey;  // PlayReady 连续计时所属图；换图必须重计（BIN 01:27）
DWORD gArriveReadyAt = 0;   // 非 0：已到目标图，等到该时刻再 SetIdle(arrived)
DWORD gGotoAtMs = 0;        // StartGoto 墙钟；首枪假火探针用 sinceGoto
std::string gExpectMap;
std::string gPendingFrom;
std::string gPendingSeedId;
std::string gPendingName;  // 逻辑门名（假火 streak key）
std::string gFiredPortal;  // 实际开火用的 live 名
// WM 状态机旁证：MapId 已离出发图 / 见过 InterStage，但尚未 Field 闭合。
bool gTransferSeen = false;
DWORD gTransferSinceMs = 0;
std::string gTransferToMap;
struct FakeFireStreak {
    std::string key;
    int count = 0;
};
FakeFireStreak gFakeFire;

// 开火未成功（瞬态）：同图同门同错误持续过久则停 + 历史事件 Notify
struct TransientFireStreak {
    std::string key;
    DWORD firstMs = 0;
    int count = 0;
};
TransientFireStreak gTransientFire;

struct LateWait {
    bool active = false;
    DWORD untilMs = 0;
    std::string fromMap;
    std::string hintName;
};
LateWait gUniqueLate;

std::string gPendingGoto;  // no_map 时暂存，稳图后再启动
// 断线 / 软重连 / 落地静默：Goto 只暂停，禁止假火/InterStage 墙钟把剩余跳熔断。
bool gGapPause = false;
bool gKeepGotoOverFarm = false;  // 重连后 F5 仍勾选时，在途赶路优先于 combat_on
std::string gLastMsg;
DWORD gLastCombatOnNotifyMs = 0;
std::string gLastCombatOnDst;
DWORD gLastCombatOnLogMs = 0;
FailKind gFailKind = FailKind::None;
DWORD gLastSnapMs = 0;
std::mutex gCmdMu;
std::string gPendingCmd;

// 赶路期间顶住无敌，防进出门被怪打飞导致 CheckMove 脱门。
bool gTravelHeldInvuln = false;
bool gInvulnPrevDesired = false;

void HoldInvulnForTravel() {
    if (gTravelHeldInvuln) return;
    gInvulnPrevDesired = invuln::IsDesired();
    if (!gInvulnPrevDesired) invuln::SetDesired(true);
    gTravelHeldInvuln = true;
    x::runtime::LogI("Travel", "invuln hold for goto (prev=%d)", gInvulnPrevDesired ? 1 : 0);
}

void ReleaseInvulnForTravel() {
    if (!gTravelHeldInvuln) return;
    if (!gInvulnPrevDesired) invuln::SetDesired(false);
    gTravelHeldInvuln = false;
    x::runtime::LogI("Travel", "invuln release (restored=%d)", gInvulnPrevDesired ? 1 : 0);
}

// Idle/停路兜底：卸 Travel 禁挂台 + 停旋翼（贴门成功路径会 LeaveArmed，换图/Idle 在此收尾）。
// 只清 BanSource::Travel，不碰 F5 CombatImpact / F6 Fly。
void ReleaseTravelFhBan() {
    namespace heli = x::features::simple_combat::heli;
    // 带 owner：只停自己那份。F6 抢占中时这里静默 no-op，不会把人从天上掐下来。
    heli::Disarm(heli::Owner::Travel);
    heli::Release(heli::Owner::Travel);
    if ((ports::fly_fh_ban::ActiveMask() &
         static_cast<unsigned>(ports::fly_fh_ban::BanSource::Travel)) == 0) {
        return;
    }
    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
    x::runtime::LogI("Travel", "fhBan Travel release mask=0x%x", ports::fly_fh_ban::ActiveMask());
}

// BIN 9d504e / adfed6：贴门后 Disarm → settle 窗 freefall。
// 对齐 Combat 软着陆：approach 带 Travel ban → 靠近后卸 ban（否则引擎永不 onFh）→
// 短 Station 消速 → drop 卸旋翼挂台。禁止「一直 BAN ON 却指望 onFh」。
constexpr float kSettleArrivePx = 36.f;
constexpr float kSettleOnFhOkPx = 64.f;
constexpr float kSettleResnapPx = 520.f;
// BIN d76f13：回城卷落到 (132,60)，SnapStandAt(preferFlat) 退化到同高远台 (-232,47)
// → settle hover 横飞 ~360px「怪位置」。托台只许近距落点，远点就地卸 ban。
constexpr float kSettleMaxSnapPx = 100.f;
// BIN f283a3：换图后 d≈49 仍走 approach BAN ON → 起飞再 catch 落地。近台且非暴坠时直接 catch。
// BIN 64b013：回城卷出生点 d=81 仍 BAN ON 起飞；软接半径与 MaxSnap 对齐。
constexpr float kSettleSoftCatchPx = 100.f;
constexpr float kSettleSoftMaxSpeed = 560.f;
constexpr DWORD kSettleCatchHoldMs = 350;

float gSettleSx = 0.f;
float gSettleSy = 0.f;
uint32_t gSettleFh = 0;
bool gSettleHave = false;
bool gSettleCatching = false;
bool gSettleDrop = false;
DWORD gSettleCatchAt = 0;

void ClearSettleHoverState() {
    gSettleHave = false;
    gSettleFh = 0;
    gSettleCatching = false;
    gSettleDrop = false;
    gSettleCatchAt = 0;
}

void TickSettleHover(DWORD now, const char* phase) {
    namespace heli = x::features::simple_combat::heli;
    ports::teleport::FlightState st{};
    if (!ports::teleport::QueryFlightState(st) || !st.ok) return;

    if (st.onFh) {
        if (heli::CurrentOwner() == heli::Owner::Travel ||
            (ports::fly_fh_ban::ActiveMask() &
             static_cast<unsigned>(ports::fly_fh_ban::BanSource::Travel)) != 0) {
            heli::Disarm(heli::Owner::Travel);
            heli::Release(heli::Owner::Travel);
            if ((ports::fly_fh_ban::ActiveMask() &
                 static_cast<unsigned>(ports::fly_fh_ban::BanSource::Travel)) != 0) {
                ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
            }
            static DWORD sLandLog = 0;
            if (!sLandLog || now - sLandLog >= 800) {
                sLandLog = now;
                x::runtime::LogI("Travel", "settle hover onFh phase=%s ap=(%.0f,%.0f)",
                                 phase ? phase : "?", st.x, st.y);
            }
        }
        ClearSettleHoverState();
        return;
    }

    // soft_login 静默 hold 期：heli::Tick 整拍 return false（guard=soft_hold），
    // 此时若下面 approach 分支 BAN ON，人被摘台又没冲量托着 → 自由落体到底。
    // BIN 2026-08-13 00:17:58 勇士之村 102000000：hold=1 时走 approach，
    // ap 从 -1765 掉到 -2718（图底 T=-2250）→ ghost/oob → 客户端重载地图。
    // 静默期一律撒手：卸 ban、放旋翼，交给引擎自然挂台，闸开后再照常 approach。
    if (x::features::soft_login_probe::IsHoldActive()) {
        if (heli::CurrentOwner() == heli::Owner::Travel) {
            heli::Disarm(heli::Owner::Travel);
            heli::Release(heli::Owner::Travel);
        }
        if ((ports::fly_fh_ban::ActiveMask() &
             static_cast<unsigned>(ports::fly_fh_ban::BanSource::Travel)) != 0) {
            ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
        }
        static DWORD sHoldLog = 0;
        if (!sHoldLog || now - sHoldLog >= 800) {
            sHoldLog = now;
            x::runtime::LogI("Travel",
                             "settle hover soft_hold release phase=%s ap=(%.0f,%.0f) v=(%.0f,%.0f)",
                             phase ? phase : "?", st.x, st.y, st.vx, st.vy);
        }
        return;
    }

    // settle 托台期间禁止 stale bail 拆旋翼（BIN adfed6：Station 到位 v=0 被判 stale →
    // 卸 ban 直坠；用户体感「FH ARM 卸不掉」——其实是 ban 卡死无法 onFh）。
    if (heli::Bailed()) heli::ClearBailed();

    // 钉死第一次可用落点；坠落中每拍 Snap 会跳到远处/图底台（adfed6 fh 跳变）。
    // preferFlat=false：settle 要「脚下/近处」台，不要同高几百 px 外的平台（d76f13）。
    auto resnap = [&]() {
        float sx = st.x;
        float sy = st.y;
        uint32_t fh = 0;
        bool got = ports::foothold_path::SnapStandAt(st.x, st.y, &sx, &sy, &fh,
                                                     /*preferFlat=*/false,
                                                     /*avoidWalkJunction=*/false,
                                                     /*cliffInset=*/false) &&
                   fh != 0;
        if (got) {
            const float sdx = sx - st.x;
            const float sdy = sy - st.y;
            const float sd = std::sqrt(sdx * sdx + sdy * sdy);
            if (sd > kSettleMaxSnapPx) {
                static DWORD sFar = 0;
                if (!sFar || now - sFar >= 800) {
                    sFar = now;
                    x::runtime::LogI("Travel",
                                     "settle hover snap reject far d=%.0f fh=%u "
                                     "sp=(%.0f,%.0f) ap=(%.0f,%.0f) → in-place",
                                     sd, (unsigned)fh, sx, sy, st.x, st.y);
                }
                sx = st.x;
                sy = st.y;
                fh = 0;
                got = false;
            }
        }
        if (got) {
            gSettleSx = sx;
            gSettleSy = sy;
            gSettleFh = fh;
            gSettleHave = true;
        } else if (!gSettleHave) {
            // 无近台：钉在出生点，随后 catch 卸 ban 原地挂台（勿横飞找台）。
            gSettleSx = st.x;
            gSettleSy = st.y;
            gSettleFh = 0;
            gSettleHave = true;
        }
    };
    if (!gSettleHave || gSettleFh == 0) {
        resnap();
    } else {
        const float rdx = st.x - gSettleSx;
        const float rdy = st.y - gSettleSy;
        if (std::sqrt(rdx * rdx + rdy * rdy) > kSettleResnapPx) {
            gSettleCatching = false;
            gSettleDrop = false;
            gSettleCatchAt = 0;
            resnap();
        }
    }

    const float dx = st.x - gSettleSx;
    const float dy = st.y - gSettleSy;
    const float d = std::sqrt(dx * dx + dy * dy);
    const float spd = std::sqrt(st.vx * st.vx + st.vy * st.vy);
    // 崩② 01:36:57：F6 3X 甩开 v≈-1093 后旧 snap d=334，BAN ON Cruise 追 → 进图 AV。
    // 高速时丢 snap、卸 Travel 旋翼，等引擎消速再托台（自然掉落 vy≈90 远低于此阈）。
    constexpr float kSettleBleedSpeed = 800.f;
    if (spd > kSettleBleedSpeed) {
        ClearSettleHoverState();
        if (heli::CurrentOwner() == heli::Owner::Travel) {
            heli::Disarm(heli::Owner::Travel);
            heli::Release(heli::Owner::Travel);
        }
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
        static DWORD sBleed = 0;
        if (!sBleed || now - sBleed >= 400) {
            sBleed = now;
            x::runtime::LogI("Travel",
                             "settle hover bleed spd=%.0f phase=%s ap=(%.0f,%.0f) — no approach",
                             spd, phase ? phase : "?", st.x, st.y);
        }
        return;
    }
    // 近台：禁止 approach 的 BAN ON（换图「起飞又落地」体感）。直接卸 ban 等挂台。
    const bool softNear =
        d <= kSettleSoftCatchPx && spd <= kSettleSoftMaxSpeed;

    // BIN 0b0e8c：换图后 soft catch 仍 Station 托 350ms → 体感「多余飞一下再落地」。
    // 近台/软接：不抢旋翼、不 Station，只卸 Travel ban 让引擎挂台，再去贴门。
    if (d <= kSettleArrivePx || softNear) {
        if (!gSettleCatching) {
            gSettleCatching = true;
            gSettleDrop = true;
            gSettleCatchAt = now;
            x::runtime::LogI("Travel",
                             "settle hover soft-drop phase=%s d=%.0f spd=%.0f soft=%d fh=%u "
                             "ap=(%.0f,%.0f) sp=(%.0f,%.0f) skipHeli=1",
                             phase ? phase : "?", d, spd, softNear ? 1 : 0,
                             (unsigned)gSettleFh, st.x, st.y, gSettleSx, gSettleSy);
        }
        if (heli::CurrentOwner() == heli::Owner::Travel) {
            heli::Disarm(heli::Owner::Travel);
            heli::Release(heli::Owner::Travel);
        }
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
        // 仅超出软接半径才改走 approach；OnFhOk 内继续等引擎挂台（勿 BAN ON 起飞）。
        if (gSettleDrop && d > kSettleSoftCatchPx) {
            gSettleCatching = false;
            gSettleDrop = false;
            gSettleCatchAt = 0;
            x::runtime::LogI("Travel", "settle hover soft drop_far retry d=%.0f", d);
        } else {
            return;
        }
    }

    // F6 占着旋翼：不抢；只记日志。
    if (!heli::TryAcquire(heli::Owner::Travel)) {
        static DWORD sSkip = 0;
        if (!sSkip || now - sSkip >= 1000) {
            sSkip = now;
            x::runtime::LogI("Travel", "settle hover skip owner=%s phase=%s",
                             heli::OwnerName(heli::CurrentOwner()), phase ? phase : "?");
        }
        return;
    }

    if (!gSettleCatching && d <= kSettleArrivePx) {
        // 远距 approach 拉近后的收束（soft 路径上面已 return）。
        gSettleCatching = true;
        gSettleDrop = false;
        gSettleCatchAt = now;
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
        x::runtime::LogI("Travel",
                         "settle hover catch begin phase=%s d=%.0f spd=%.0f soft=0 fh=%u "
                         "ap=(%.0f,%.0f) sp=(%.0f,%.0f) banOff=1",
                         phase ? phase : "?", d, spd, (unsigned)gSettleFh, st.x, st.y,
                         gSettleSx, gSettleSy);
    }

    if (gSettleCatching) {
        const DWORD catchAge = gSettleCatchAt ? (now - gSettleCatchAt) : 0;
        if (!gSettleDrop && catchAge >= kSettleCatchHoldMs) {
            gSettleDrop = true;
            heli::Disarm(heli::Owner::Travel);
            heli::Release(heli::Owner::Travel);
            x::runtime::LogI("Travel", "settle hover catch drop phase=%s d=%.0f age=%ums",
                             phase ? phase : "?", d, (unsigned)catchAge);
        }
        if (gSettleDrop) {
            if (d > kSettleOnFhOkPx) {
                gSettleCatching = false;
                gSettleDrop = false;
                gSettleCatchAt = 0;
                x::runtime::LogI("Travel", "settle hover catch drop_far retry d=%.0f", d);
            }
            return;
        }
        // catch hold：ban 已卸，Station 消速等引擎挂台。
        ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, false);
        heli::Setpoint sp{};
        sp.mode = heli::Mode::Station;
        sp.x = gSettleSx;
        sp.y = gSettleSy;
        heli::SetSetpoint(heli::Owner::Travel, sp);
        (void)heli::Tick(heli::Owner::Travel, now, nullptr);
        return;
    }

    ports::fly_fh_ban::SetSourceArmed(ports::fly_fh_ban::BanSource::Travel, true);
    heli::Setpoint sp{};
    sp.mode = (d > 120.f) ? heli::Mode::Cruise : heli::Mode::Station;
    sp.x = gSettleSx;
    sp.y = gSettleSy;
    heli::SetSetpoint(heli::Owner::Travel, sp);
    (void)heli::Tick(heli::Owner::Travel, now, nullptr);

    static DWORD sDriveLog = 0;
    if (!sDriveLog || now - sDriveLog >= 400) {
        sDriveLog = now;
        x::runtime::LogI("Travel",
                         "settle hover drive phase=%s fh=%u sp=(%.0f,%.0f) ap=(%.0f,%.0f) d=%.0f",
                         phase ? phase : "?", (unsigned)gSettleFh, gSettleSx, gSettleSy, st.x,
                         st.y, d);
    }
}

// 用户可见反馈：BIN 有 log 不够，launcher 气泡要跟上。
void NotifyTravelOutcome(FailKind kind, const std::string& src, const std::string& dst,
                         const char* raw = nullptr) {
    using notify::NotificationKind;
    char body[220]{};
    const char* title = "超级赶路";
    const char* key = "travel.fail";
    NotificationKind nk = NotificationKind::Warning;
    switch (kind) {
    case FailKind::Unreachable:
        key = "travel.unreachable";
        nk = NotificationKind::Warning;
        snprintf(body, sizeof(body),
                 "不可达：%s → %s（同盘无路径，可能需手动过港口/换板块）",
                 src.empty() ? "?" : src.c_str(), dst.empty() ? "?" : dst.c_str());
        break;
    case FailKind::BadTarget:
        key = "travel.bad_target";
        nk = NotificationKind::Warning;
        snprintf(body, sizeof(body), "目标无法解析：%s",
                 (raw && raw[0]) ? raw : (dst.empty() ? "?" : dst.c_str()));
        break;
    case FailKind::AlreadyThere:
        key = "travel.already";
        nk = NotificationKind::Info;
        snprintf(body, sizeof(body), "已在目标地图 %s", src.empty() ? "?" : src.c_str());
        break;
    case FailKind::FakeFireStop:
        key = "travel.fake_fire";
        nk = NotificationKind::Danger;
        snprintf(body, sizeof(body), "进门多次未换图，已停止（%s）",
                 src.empty() ? "?" : src.c_str());
        break;
    case FailKind::FireStuck:
        key = "travel.fire_stuck";
        nk = NotificationKind::Danger;
        snprintf(body, sizeof(body), "贴门失败已停止：%s @ %s → %s",
                 (raw && raw[0]) ? raw : "?", src.empty() ? "?" : src.c_str(),
                 dst.empty() ? "?" : dst.c_str());
        break;
    case FailKind::CombatOn: {
        key = "travel.combat_on";
        nk = NotificationKind::Warning;
        snprintf(body, sizeof(body), "请先关闭 F5 自动打怪再赶路");
        // AutoSupply 续跑每 200ms 重试 RequestGoto；同目标 5s 内只弹一次。
        const DWORD now = GetTickCount();
        if (gLastCombatOnNotifyMs && now - gLastCombatOnNotifyMs < 5000 &&
            gLastCombatOnDst == dst)
            return;
        gLastCombatOnNotifyMs = now;
        gLastCombatOnDst = dst;
        break;
    }
    default:
        return;
    }
    notify::PublishNotification(
        notify::NotificationEvent{nk, key, title, body, 5500});
    x::runtime::LogI("Travel", "notify %s: %s", key, body);
}

std::string Join(const char* bin, const char* rel) {
    std::string out = bin ? bin : "";
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out += '\\';
    out += rel ? rel : "";
    return out;
}

std::string SeedPath() { return Join(x::runtime::GetBinDir(), "state\\travel_graph.seed.tsv"); }
std::string GraphPath() { return Join(x::runtime::GetBinDir(), "state\\travel_graph.tsv"); }
std::string CmdPath() { return Join(x::runtime::GetBinDir(), "state\\travel_cmd.txt"); }

void SaveGraph() { (void)gGraph.Save(GraphPath()); }

void ClearFakeFire() { gFakeFire = FakeFireStreak{}; }
void ClearTransientFire() { gTransientFire = TransientFireStreak{}; }
void ClearLateWait() { gUniqueLate = LateWait{}; }

void ClearTransferWatch() {
    gTransferSeen = false;
    gTransferSinceMs = 0;
    gTransferToMap.clear();
}

void SetIdle(const char* msg, FailKind fail = FailKind::None) {
    gMode = Mode::Idle;
    ++gFireEpoch;
    gHops.clear();
    gHopIdx = 0;
    gExpectMap.clear();
    gPendingFrom.clear();
    gPendingSeedId.clear();
    gPendingName.clear();
    gFiredPortal.clear();
    gHopStartedMs = 0;
    gNextHopReadyAt = 0;
    gPlayReadySince = 0;
    gStableMapKey.clear();
    gArriveReadyAt = 0;
    gGotoAtMs = 0;
    gPendingGoto.clear();
    gGapPause = false;
    gKeepGotoOverFarm = false;
    ClearTransferWatch();
    ClearFakeFire();
    ClearTransientFire();
    ClearLateWait();
    ReleaseInvulnForTravel();
    ReleaseTravelFhBan();
    ClearSettleHoverState();
    gFailKind = fail;
    if (msg) gLastMsg = msg;
    gLastSnapMs = GetTickCount();
}

void NotePlayReadyGate(DWORD now, bool ready) {
    if (!ready) {
        gPlayReadySince = 0;
        gStableMapKey.clear();
        if (gArriveReadyAt) gArriveReadyAt = now + kArriveStableMs;
        return;
    }
    if (!gPlayReadySince) gPlayReadySince = now;
}

// BIN 2026-08-09 01:27：MapId 先闪变时 PlayReady 仍短时为真，旧图的
// PlayReadyStable 时钟未清 → 过早 map ok → 下一跳贴门撞半截 InterStage。
void NoteMapKeyForStable(const std::string& cur, DWORD now) {
    if (cur.empty()) return;
    if (gStableMapKey == cur) return;
    if (!gStableMapKey.empty()) {
        x::runtime::LogI("Travel", "play_ready stable reset map %s -> %s",
                         gStableMapKey.c_str(), cur.c_str());
    }
    gStableMapKey = cur;
    gPlayReadySince = now;  // 换图后必须重新攒满 kPlayReadyStableMs
}

bool PlayReadyStable(DWORD now) {
    return gPlayReadySince != 0 && (now - gPlayReadySince) >= kPlayReadyStableMs;
}

// 到站真源 = WM 状态机闭合：Field + mapScene + IsPlayReady。
// MapId 只作「传送已启动」旁证，绝不当 map ok。
bool WmFieldClosed() {
    if (ports::world::GetSceneState() != ports::world::SceneState::Field) return false;
    if (!ports::world::GetMapScene()) return false;
    return ports::world::IsPlayReady();
}

void NoteTransferStarted(const std::string& toMap, DWORD now, const char* why) {
    if (!gTransferSeen) {
        gTransferSeen = true;
        gTransferSinceMs = now ? now : GetTickCount();
        gTransferToMap = toMap;
        x::runtime::LogI("Travel",
                         "wm transfer started from=%s to=%s why=%s scene=%d (wait Field)",
                         gPendingFrom.empty() ? "?" : gPendingFrom.c_str(),
                         toMap.empty() ? "?" : toMap.c_str(), why ? why : "-",
                         static_cast<int>(ports::world::GetSceneState()));
    } else if (!toMap.empty() && gTransferToMap != toMap) {
        gTransferToMap = toMap;
    }
}

bool MapLeftPendingFrom(const std::string& cur) {
    return !gPendingFrom.empty() && !cur.empty() && cur != gPendingFrom;
}

// InterStage / MapId 已闪变后等 Field；超时停路（客户端黑屏卡死）。
bool NoteInterStageStuck(DWORD now, const std::string& curHint) {
    if (!gTransferSeen || gTransferSinceMs == 0) return false;
    if (now - gTransferSinceMs < kInterStageStuckMs) return false;
    char detail[128]{};
    snprintf(detail, sizeof(detail), "InterStage stuck %ums ->%s",
             (unsigned)(now - gTransferSinceMs),
             gTransferToMap.empty() ? (curHint.empty() ? "?" : curHint.c_str())
                                    : gTransferToMap.c_str());
    x::runtime::LogW("Travel", "interstage_stuck stop %s scene=%d", detail,
                     static_cast<int>(ports::world::GetSceneState()));
    SetIdle("interstage_stuck", FailKind::FireStuck);
    NotifyTravelOutcome(FailKind::FireStuck,
                        gPendingFrom.empty() ? curHint : gPendingFrom, gTarget, detail);
    return true;
}

bool StartGotoResolved(const std::string& src, const std::string& dst, const std::string& rawArg) {
    if (simple_combat::IsFarmingActive()) {
        SetIdle("combat_on", FailKind::CombatOn);
        const DWORD now = GetTickCount();
        if (!gLastCombatOnLogMs || now - gLastCombatOnLogMs >= 2000) {
            gLastCombatOnLogMs = now;
            x::runtime::LogW("Travel", "goto blocked: F5 farming still active dst=%s",
                             dst.c_str());
        }
        NotifyTravelOutcome(FailKind::CombatOn, src, dst);
        return false;
    }
    if (src.empty()) {
        gPendingGoto = rawArg.empty() ? dst : rawArg;
        gLastMsg = "no_map_wait";
        x::runtime::LogW("Travel", "goto defer no_map dst=%s raw=%s (wait CurrentMapKey)",
                         dst.c_str(), rawArg.c_str());
        return false;
    }
    if (src == dst) {
        SetIdle("already_there", FailKind::AlreadyThere);
        x::runtime::LogI("Travel", "already_there %s", src.c_str());
        NotifyTravelOutcome(FailKind::AlreadyThere, src, dst);
        return false;
    }
    gTarget = dst;
    gHops = gGraph.PathTo(src, dst);
    gHopIdx = 0;
    if (gHops.empty()) {
        const int revived = gGraph.ReviveDeadFromDestKey(src);
        if (revived > 0) {
            SaveGraph();
            gHops = gGraph.PathTo(src, dst);
        }
    }
    if (gHops.empty()) {
        SetIdle("unreachable", FailKind::Unreachable);
        x::runtime::LogW("Travel", "unreachable %s -> %s", src.c_str(), dst.c_str());
        NotifyTravelOutcome(FailKind::Unreachable, src, dst);
        return false;
    }
    gPendingGoto.clear();
    gGapPause = false;
    gKeepGotoOverFarm = false;
    gMode = Mode::Goto;
    ++gFireEpoch;
    ClearFakeFire();
    ClearTransientFire();
    ClearLateWait();
    ClearTransferWatch();
    gFailKind = FailKind::None;
    gExpectMap.clear();
    gPendingFrom.clear();
    gPendingSeedId.clear();
    gPendingName.clear();
    gFiredPortal.clear();
    gHopStartedMs = 0;
    // 首跳也走 settle：世界地图确认/刚进 Goto 时 PlayReady 已亮，但进门脚本常未就绪；
    // 旧逻辑只等 500ms → 首枪假火 → uniqueBridge 再空等一轮（体感首跳卡 ~2s）。
    gNextHopReadyAt = GetTickCount() + kFirstHopSettleMs;
    if (gNextHopReadyAt == 0) gNextHopReadyAt = 1;
    gGotoAtMs = GetTickCount();
    if (gGotoAtMs == 0) gGotoAtMs = 1;
    gPlayReadySince = 0;
    gStableMapKey.clear();
    gArriveReadyAt = 0;
    gLastMsg = "goto " + dst + " hops=" + std::to_string(gHops.size());
    gLastSnapMs = GetTickCount();
    x::runtime::LogI("Travel", "goto %s -> %s hops=%d fire=%s firstSettle=%ums", src.c_str(),
                     dst.c_str(), (int)gHops.size(),
                     ports::travel::FireModeName(ports::travel::GetFireMode()),
                     (unsigned)kFirstHopSettleMs);
    HoldInvulnForTravel();
    return true;
}

void EnsureGraphLoaded() {
    if (gSeedLoaded) return;
    const auto& names = xcat::GetSharedMapNames(x::runtime::GetBinDir());
    x::runtime::LogI("Travel", "map_names shared loaded=%d names=%d", names.loaded ? 1 : 0,
                     (int)names.keyByName.size());
    gGraph = Graph{};
    const bool seedOk = gGraph.Load(SeedPath());
    (void)gGraph.Load(GraphPath());
    gSeedLoaded = seedOk || gGraph.MapCount() > 0;
    x::runtime::LogI("Travel", "graph ready seed=%d maps=%d edges=%d", seedOk ? 1 : 0,
                     gGraph.MapCount(), gGraph.EdgeCount());
}

std::string ResolveTarget(const std::string& raw) {
    const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
    return xcat::MapNamesResolveQuery(pack, raw);
}

std::string PortalNameHint(const Hop& hop) {
    std::string pname = gGraph.PortalName(hop.map, hop.portalId);
    if (!pname.empty()) return pname;
    const size_t slash = hop.portalId.rfind('/');
    if (slash != std::string::npos) return hop.portalId.substr(slash + 1);
    return {};
}

// 当前图实时解门：返回可开火的 portalName（经典版 FirePortalByName 按名）。
// seedId 命中时用该门 name；否则按 hintName；再按 expectDest；无则 ""。
std::string ResolveLivePortalName(const std::string& seedId, const std::string& hintName,
                                  const std::string& expectDest,
                                  std::vector<ports::travel::PortalInfo>* outLive = nullptr) {
    std::string mapName;
    std::vector<ports::travel::PortalInfo> portals;
    if (!ports::travel::EnumPortals(mapName, portals) || portals.empty()) return {};
    if (outLive) *outLive = portals;

    auto pick = [&](const ports::travel::PortalInfo& p) -> std::string {
        // 产品路径不因 Enable 脏读拦死；FirePortalByName 侧已放宽 DirectEnter/Stick
        return p.name.empty() ? p.id : p.name;
    };

    if (!seedId.empty()) {
        for (const auto& p : portals) {
            if (p.id == seedId) {
                const std::string n = pick(p);
                if (!n.empty()) return n;
            }
        }
    }
    if (!hintName.empty()) {
        for (const auto& p : portals) {
            if (p.name == hintName) {
                const std::string n = pick(p);
                if (!n.empty()) return n;
            }
        }
    }
    // 名对不上时按目标图号兜底（同盘唯一桥常见）
    if (!expectDest.empty()) {
        const ports::travel::PortalInfo* hit = nullptr;
        int hits = 0;
        for (const auto& p : portals) {
            if (p.destMap == expectDest) {
                hit = &p;
                ++hits;
            }
        }
        if (hits == 1 && hit) {
            const std::string n = pick(*hit);
            if (!n.empty()) {
                x::runtime::LogI("Travel", "resolve by dest %s -> name=%s", expectDest.c_str(),
                                 n.c_str());
                return n;
            }
        }
    }
    return {};
}

void LogResolveEmptyDiag(const std::string& hint, const std::string& seedId,
                         const std::vector<ports::travel::PortalInfo>& live) {
    if (live.empty()) {
        x::runtime::LogW("Travel", "RESOLVE_EMPTY hint=%s seed=%s enum=0", hint.c_str(),
                         seedId.c_str());
        return;
    }
    char sample[256]{};
    size_t used = 0;
    for (size_t i = 0; i < live.size() && used + 24 < sizeof(sample); ++i) {
        const auto& p = live[i];
        const int n = snprintf(sample + used, sizeof(sample) - used, "%s%s(%d->%d)",
                               i ? "," : "", p.name.c_str(), p.activate ? 1 : 0, p.toMapId);
        if (n > 0) used += static_cast<size_t>(n);
    }
    x::runtime::LogW("Travel", "RESOLVE_EMPTY hint=%s seed=%s n=%d [%s]", hint.c_str(),
                     seedId.c_str(), (int)live.size(), sample);
}

void UpsertLiveEdges(const std::string& mk,
                     const std::vector<ports::travel::PortalInfo>& live) {
    if (mk.empty() || live.empty()) return;
    gGraph.UpsertMap(mk, live);
    for (const auto& p : live) {
        if (!p.destMap.empty()) gGraph.SetDest(mk, p.id, p.destMap, p.destMap, false);
    }
}

void MarkLogicalPortalDead(const std::string& fromMap, const std::string& seedId,
                           const std::string& hintName) {
    bool any = false;
    if (!seedId.empty() && gGraph.SetDest(fromMap, seedId, "__DEAD__")) any = true;
    if (!hintName.empty()) {
        const std::string seedByName = "seed:" + fromMap + "/" + hintName;
        if (seedByName != seedId && gGraph.SetDest(fromMap, seedByName, "__DEAD__")) any = true;
        if (gGraph.SetDestForPortalName(fromMap, hintName, "__DEAD__") > 0) any = true;
    }
    if (any) SaveGraph();
    x::runtime::LogW("Travel", "DEAD logic [%s] seed=%s name=%s", fromMap.c_str(),
                     seedId.empty() ? "-" : seedId.c_str(),
                     hintName.empty() ? "-" : hintName.c_str());
}

bool IsTransientFireFail(const std::string& res) {
    // FH0 → 假火软确认；MAP_TRANSITION 现由 FirePortal 返回 true，不再当失败重试
    return res == "TELEPORT_FAIL" || res == "TELEPORT_UNBOUND" || res == "MAIN_TIMEOUT" ||
           res == "NO_CHECKMOVE" || res == "NO_LOCALUSER" || res == "KEY_FAIL" ||
           res == "EXCEPTION" || res == "NOT_PLAY_READY" || res == "TELEPORT_COOLDOWN" ||
           res == "NOT_STOOD" || res == "OUT_OF_RECT" || res == "IMPACT_STICK_FAIL";
}

// 可累计 FireStuck 熔断的硬贴门失败（冷却/未就绪只重试，不熔断，防误停）。
bool IsFusableTransientFireFail(const std::string& res) {
    return res == "TELEPORT_FAIL" || res == "TELEPORT_UNBOUND" || res == "MAIN_TIMEOUT" ||
           res == "NO_CHECKMOVE" || res == "NO_LOCALUSER" || res == "KEY_FAIL" ||
           res == "EXCEPTION" || res == "NOT_STOOD" || res == "OUT_OF_RECT" ||
           res == "IMPACT_STICK_FAIL";
}

// 返回 true=已停赶路并 Notify；false=继续重试。
bool NoteTransientFireFail(DWORD now, const std::string& curMap, const std::string& portalName,
                           const std::string& fireResult) {
    // 冷却 / 未就绪：清 streak，只打节流 log，不进 8s 熔断
    if (!IsFusableTransientFireFail(fireResult)) {
        ClearTransientFire();
        if (now - gHopStartedMs > 1500) {
            gHopStartedMs = now;
            x::runtime::LogW("Travel", "fire transient %s: %s (no fuse)", portalName.c_str(),
                             fireResult.c_str());
        }
        return false;
    }
    const std::string key = curMap + "|" + portalName + "|" + fireResult;
    if (gTransientFire.key != key) {
        gTransientFire.key = key;
        gTransientFire.firstMs = now;
        gTransientFire.count = 1;
    } else {
        ++gTransientFire.count;
    }
    const DWORD elapsed = now - gTransientFire.firstMs;
    if (elapsed < kTransientFireFuseMs) {
        if (now - gHopStartedMs > 1500) {
            gHopStartedMs = now;
            x::runtime::LogW("Travel", "fire transient %s: %s (%ums/%ums n=%d)",
                             portalName.c_str(), fireResult.c_str(), elapsed,
                             kTransientFireFuseMs, gTransientFire.count);
        }
        return false;
    }
    char detail[128]{};
    snprintf(detail, sizeof(detail), "%s %s", portalName.c_str(), fireResult.c_str());
    x::runtime::LogW("Travel", "fire_stuck stop map=%s portal=%s res=%s elapsed=%ums n=%d",
                     curMap.c_str(), portalName.c_str(), fireResult.c_str(), elapsed,
                     gTransientFire.count);
    SetIdle("fire_stuck", FailKind::FireStuck);
    NotifyTravelOutcome(FailKind::FireStuck, curMap, gTarget, detail);
    return true;
}

// 返回 true=已停赶路；false=清 pending 后由 Tick 重试/改路。
bool NoteNoMapChangeTimeout(DWORD now) {
    // MapId 已离出发图：传送在跑，禁假火补 ↑（BIN 01:48 第二枪黑屏）。
    const std::string curNow = ports::travel::CurrentMapKey();
    if (MapLeftPendingFrom(curNow) || gTransferSeen) {
        NoteTransferStarted(curNow, now, "timeout_guard");
        x::runtime::LogI("Travel",
                         "skip fake-fire (wm transfer in flight) from=%s cur=%s",
                         gPendingFrom.c_str(), curNow.empty() ? "?" : curNow.c_str());
        return false;
    }
    // 开火后静默窗：uniqueBridge 700ms 太短，stick 首枪未换图就 soft 补枪。
    if (gHopStartedMs != 0 && now - gHopStartedMs < kPostFireQuietMs) {
        return false;
    }
    const std::string logic =
        !gPendingName.empty() ? gPendingName
                              : (!gFiredPortal.empty() ? gFiredPortal : gPendingSeedId);
    const std::string key = gPendingFrom + "|name=" + logic;
    if (gFakeFire.key != key) {
        gFakeFire.key = key;
        gFakeFire.count = 1;
    } else {
        ++gFakeFire.count;
    }

    const bool uniqueBridge =
        !gTarget.empty() && !gPendingName.empty() &&
        gGraph.IsUniqueBridgeName(gPendingFrom, gTarget, gPendingName);

    if (gFakeFire.count < kFakeFireSoftConfirm) {
        x::runtime::LogW("Travel",
                         "fake soft %d/%d [%s] name=%s%s（不标死）", gFakeFire.count,
                         kFakeFireSoftConfirm, gPendingFrom.c_str(),
                         logic.empty() ? "-" : logic.c_str(),
                         uniqueBridge ? " uniqueBridge" : "");
        gExpectMap.clear();
        gFiredPortal.clear();
        gHopStartedMs = 0;
        return false;
    }

    if (uniqueBridge) {
        if (gFakeFire.count < kFakeFireUniqueBridgeConfirm) {
            x::runtime::LogW("Travel",
                             "uniqueBridge soft %d/%d [%s] name=%s（永不标死）",
                             gFakeFire.count, kFakeFireUniqueBridgeConfirm,
                             gPendingFrom.c_str(), logic.c_str());
            gExpectMap.clear();
            gFiredPortal.clear();
            gHopStartedMs = 0;
            return false;
        }
        // 迟到换图观察窗：保持 Goto，不再发门
        gUniqueLate.active = true;
        gUniqueLate.untilMs = now + kUniqueBridgeLateGraceMs;
        if (gUniqueLate.untilMs == 0) gUniqueLate.untilMs = 1;
        gUniqueLate.fromMap = gPendingFrom;
        gUniqueLate.hintName = gPendingName;
        gExpectMap.clear();
        gFiredPortal.clear();
        gHopStartedMs = 0;
        gLastMsg = "unique_bridge_late_wait";
        x::runtime::LogW("Travel",
                         "uniqueBridge late-wait %ums [%s] name=%s",
                         (unsigned)kUniqueBridgeLateGraceMs, gPendingFrom.c_str(),
                         logic.c_str());
        return false;
    }

    MarkLogicalPortalDead(gPendingFrom, gPendingSeedId, gPendingName);
    gExpectMap.clear();
    gFiredPortal.clear();
    gHopStartedMs = 0;

    if (gFakeFire.count < kFakeFireFuseConfirm) {
        ClearFakeFire();  // 改路后按新门重新累计
        gHops = gGraph.PathTo(ports::travel::CurrentMapKey(), gTarget);
        gHopIdx = 0;
        if (gHops.empty()) {
            const int revived = gGraph.ReviveDeadFromDestKey(ports::travel::CurrentMapKey());
            if (revived > 0) {
                SaveGraph();
                gHops = gGraph.PathTo(ports::travel::CurrentMapKey(), gTarget);
            }
        }
        if (gHops.empty()) {
            SetIdle("unreachable", FailKind::Unreachable);
            NotifyTravelOutcome(FailKind::Unreachable, ports::travel::CurrentMapKey(), gTarget);
            return true;
        }
        x::runtime::LogW("Travel", "fake marked → replan hops=%d", (int)gHops.size());
        return false;
    }

    SetIdle("fake_fire_stop", FailKind::FakeFireStop);
    x::runtime::LogW("Travel", "fake_fire_stop fuse=%d at %s", kFakeFireFuseConfirm,
                     ports::travel::CurrentMapKey().c_str());
    NotifyTravelOutcome(FailKind::FakeFireStop, ports::travel::CurrentMapKey(), gTarget);
    return true;
}

bool ReplanOrStop(const std::string& cur);

bool InTravelGapPause() {
    if (x::features::soft_login_probe::IsReconnectInFlight()) return true;
    const int nm = x::features::kick_sniff::LastSessionState();
    // Disconnecting=0 Disconnected=1 Connecting=2 Connected=3；-1=尚未采到，不得当空档。
    if (nm == 0 || nm == 1 || nm == 2) return true;
    const auto scene = ports::world::GetSceneState();
    if (scene == ports::world::SceneState::Login || scene == ports::world::SceneState::None ||
        scene == ports::world::SceneState::CashShop ||
        scene == ports::world::SceneState::GlobalMarket ||
        scene == ports::world::SceneState::Unknown)
        return true;
    // InterStage / Field：真换图等待，不是断线空档。
    return false;
}

void NoteGotoGapPause(DWORD now) {
    if (gGapPause) return;
    gGapPause = true;
    gKeepGotoOverFarm = true;
    x::runtime::LogW("Travel",
                     "goto pause (disconnect/soft_login) target=%s hop=%d/%d expect=%s",
                     gTarget.c_str(), (int)gHopIdx, (int)gHops.size(),
                     gExpectMap.empty() ? "-" : gExpectMap.c_str());
    (void)now;
}

// 重连落地：丢弃在途发门等待（不是真进门），按当前图重规划剩余跳。禁止把重连当实测边。
bool ResumeGotoAfterGap(DWORD now) {
    ClearTransferWatch();
    ClearLateWait();
    ClearFakeFire();
    ClearTransientFire();
    gExpectMap.clear();
    gFiredPortal.clear();
    gPendingFrom.clear();
    gPendingSeedId.clear();
    gPendingName.clear();
    gHopStartedMs = 0;
    gArriveReadyAt = 0;
    ReleaseTravelFhBan();
    ClearSettleHoverState();

    const std::string cur = ports::travel::CurrentMapKey();
    if (cur.empty()) {
        gLastMsg = "wait_resume";
        x::runtime::LogW("Travel", "resume deferred (no CurrentMapKey) target=%s",
                         gTarget.c_str());
        return false;
    }
    gGapPause = false;
    NoteMapKeyForStable(cur, now);
    if (cur == gTarget) {
        gArriveReadyAt = now + kArriveStableMs;
        gLastMsg = "arrive_settle";
        x::runtime::LogI("Travel", "resume after gap already at target %s", cur.c_str());
        return true;
    }
    if (!ReplanOrStop(cur)) return false;
    gNextHopReadyAt = now + kMidHopSettleMs;
    gLastMsg = "resume settle -> next";
    x::runtime::LogI("Travel", "resume after gap %s -> %s hops=%d (replan, no measured edge)",
                     cur.c_str(), gTarget.c_str(), (int)gHops.size());
    return true;
}

bool ReplanOrStop(const std::string& cur) {
    gHops = gGraph.PathTo(cur, gTarget);
    gHopIdx = 0;
    if (!gHops.empty()) return true;
    const int revived = gGraph.ReviveDeadFromDestKey(cur);
    if (revived > 0) {
        x::runtime::LogI("Travel", "revive DEAD from destKey n=%d", revived);
        SaveGraph();
        ClearFakeFire();
        gHops = gGraph.PathTo(cur, gTarget);
        gHopIdx = 0;
        if (!gHops.empty()) return true;
    }
    SetIdle("unreachable", FailKind::Unreachable);
    NotifyTravelOutcome(FailKind::Unreachable, cur, gTarget);
    return false;
}

void EnqueueCmd(const std::string& cmd) {
    std::lock_guard<std::mutex> lock(gCmdMu);
    gPendingCmd = cmd;
}

bool ConsumeDiskCmd(std::string& out) {
    out.clear();
    const std::string path = CmdPath();
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char buf[512]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    DeleteFileA(path.c_str());
    if (n == 0) return false;
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return !out.empty();
}

void HandleCmd(const std::string& cmd) {
    if (cmd.rfind("stop", 0) == 0) {
        SetIdle("stopped");
        x::runtime::LogI("Travel", "stop");
        return;
    }
    if (cmd.rfind("save", 0) == 0) {
        EnsureGraphLoaded();
        const bool ok = gGraph.Save(GraphPath());
        gLastMsg = ok ? "saved" : "save_fail";
        return;
    }
    if (cmd.rfind("capture", 0) == 0) {
        std::string arg = cmd.size() > 7 ? cmd.substr(7) : "";
        while (!arg.empty() && (arg[0] == ' ' || arg[0] == '\t')) arg.erase(arg.begin());
        const bool on = (arg == "1" || arg == "on" || arg == "true");
        ports::travel::SetCaptureEnabled(on);
        gLastMsg = on ? "capture_on" : "capture_off";
        return;
    }
    if (cmd.rfind("firemode", 0) == 0) {
        std::string arg = cmd.size() > 8 ? cmd.substr(8) : "";
        while (!arg.empty() && (arg[0] == ' ' || arg[0] == '\t')) arg.erase(arg.begin());
        if (arg == "up")
            ports::travel::SetFireMode(ports::travel::FireMode::Up);
        else if (arg == "check")
            ports::travel::SetFireMode(ports::travel::FireMode::CheckMove);
        else if (arg == "rpc")
            ports::travel::SetFireMode(ports::travel::FireMode::Rpc);
        else if (arg == "stick" || arg == "stickup" || arg == "tp" || arg == "teleport")
            ports::travel::SetFireMode(ports::travel::FireMode::StickUp);
        else if (arg == "direct" || arg == "enter")
            ports::travel::SetFireMode(ports::travel::FireMode::DirectEnter);
        gLastMsg = std::string("firemode=") + ports::travel::FireModeName(ports::travel::GetFireMode());
        return;
    }
    auto fireOne = [&](ports::travel::FireMode mode, bool warp, const std::string& pn) {
        ports::travel::SetFireMode(mode);
        ports::travel::SetCaptureEnabled(true);
        std::string res;
        const bool ok = ports::travel::FirePortalByName(pn, warp, res);
        gLastMsg = (ok ? "ok " : "fail ") + res + " pn=" + pn;
        x::runtime::LogI("Travel", "probe mode=%s warp=%d pn=%s -> %s",
                         ports::travel::FireModeName(mode), warp ? 1 : 0, pn.c_str(), res.c_str());
    };
    if (cmd.rfind("rpcfar ", 0) == 0) {
        fireOne(ports::travel::FireMode::Rpc, false, cmd.substr(7));
        return;
    }
    if (cmd.rfind("rpc ", 0) == 0) {
        fireOne(ports::travel::FireMode::Rpc, true, cmd.substr(4));
        return;
    }
    if (cmd.rfind("check ", 0) == 0) {
        fireOne(ports::travel::FireMode::CheckMove, true, cmd.substr(6));
        return;
    }
    if (cmd.rfind("stick ", 0) == 0) {
        fireOne(ports::travel::FireMode::StickUp, true, cmd.substr(6));
        return;
    }
    if (cmd.rfind("direct ", 0) == 0) {
        fireOne(ports::travel::FireMode::DirectEnter, true, cmd.substr(7));
        return;
    }
    if (cmd.rfind("directfar ", 0) == 0) {
        fireOne(ports::travel::FireMode::DirectEnter, false, cmd.substr(10));
        return;
    }

    std::string arg;
    if (cmd.rfind("goto", 0) == 0) {
        arg = cmd.substr(4);
        while (!arg.empty() && (arg[0] == ' ' || arg[0] == '\t')) arg.erase(arg.begin());
    } else {
        return;
    }
    EnsureGraphLoaded();
    const std::string dst = ResolveTarget(arg);
    if (dst.empty()) {
        SetIdle("bad_target", FailKind::BadTarget);
        x::runtime::LogW("Travel", "goto unresolved '%s'", arg.c_str());
        NotifyTravelOutcome(FailKind::BadTarget, "", "", arg.c_str());
        return;
    }
    const std::string src = ports::travel::CurrentMapKey();
    (void)StartGotoResolved(src, dst, arg);
}

void TryFlushPendingGoto() {
    if (gPendingGoto.empty() || gMode != Mode::Idle) return;
    if (InTravelGapPause()) return;
    if (!ports::world::IsPlayReady()) return;
    const std::string src = ports::travel::CurrentMapKey();
    if (src.empty()) return;
    const std::string raw = gPendingGoto;
    const std::string dst = ResolveTarget(raw);
    if (dst.empty()) {
        gPendingGoto.clear();
        x::runtime::LogW("Travel", "pending goto unresolved '%s'", raw.c_str());
        SetIdle("bad_target", FailKind::BadTarget);
        NotifyTravelOutcome(FailKind::BadTarget, src, "", raw.c_str());
        return;
    }
    x::runtime::LogI("Travel", "flush pending goto raw=%s src=%s", raw.c_str(), src.c_str());
    (void)StartGotoResolved(src, dst, raw);
}

void OnMapEnterConfirmed(const std::string& cur, DWORD now) {
    // 贴门成功进门后 ban 留到换图；新图稳图前卸 Travel 位，允许落地 midhop。
    ReleaseTravelFhBan();
    ClearSettleHoverState();  // 新图须重钉；近台走 soft catch，避免再 BAN ON 起飞
    ClearTransferWatch();
    if (!gFiredPortal.empty() || !gPendingSeedId.empty()) {
        gGraph.SetDest(gPendingFrom, gPendingSeedId, cur, cur, /*measured=*/true);
        if (!gPendingName.empty()) {
            // 同名 seed 键也实测确认（不经 measured API 的同名批量仅用于 DEAD）
            const std::string seedByName = "seed:" + gPendingFrom + "/" + gPendingName;
            if (seedByName != gPendingSeedId)
                gGraph.SetDest(gPendingFrom, seedByName, cur, cur, true);
        }
        SaveGraph();
    }
    ClearFakeFire();
    ClearTransientFire();
    ClearLateWait();
    gExpectMap.clear();
    gFiredPortal.clear();
    gPendingFrom.clear();
    gPendingSeedId.clear();
    gPendingName.clear();
    gHopIdx++;
    gHopStartedMs = 0;
    gNextHopReadyAt = now + kMidHopSettleMs;
    // arrived 只认 cur==目标；跳数用尽但未到目标 → 重规划 / 不可达（禁止假 arrived）
    if (cur == gTarget) {
        // 不立刻 Idle：BIN 到站后仍可能 InterStage 二次卸图；稳完再放无敌/交棒 AutoSupply
        gArriveReadyAt = now + kArriveStableMs;
        gLastMsg = "arrive_settle";
        x::runtime::LogI("Travel", "arrive pending settle=%ums map=%s",
                         (unsigned)kArriveStableMs, cur.c_str());
        return;
    }
    if (gHopIdx >= gHops.size()) {
        x::runtime::LogW("Travel", "hops exhausted at %s target=%s — replan", cur.c_str(),
                         gTarget.c_str());
        if (!ReplanOrStop(cur)) return;
        gNextHopReadyAt = now + kMidHopSettleMs;
        gLastMsg = "midhop settle -> replan";
        return;
    }
    gLastMsg = "midhop settle -> next";
    x::runtime::LogI("Travel", "map ok %s hop=%d/%d settle=%ums (wm Field stable)", cur.c_str(),
                     (int)gHopIdx, (int)gHops.size(), (unsigned)kMidHopSettleMs);
}

void TickGoto(DWORD now, std::unique_lock<std::mutex>& lock) {
    // 断线 / 软重连 / 落地静默：先于 F5 互斥。墙钟停走，落地后 Resume 重规划。
    if (InTravelGapPause()) {
        NoteGotoGapPause(now);
        gLastMsg = "wait_resume";
        if (gNextHopReadyAt) gNextHopReadyAt = now + kMidHopSettleMs;
        if (ports::world::IsInMapScene() && ports::world::IsPlayReady())
            TickSettleHover(now, "wait_resume");
        return;
    }
    if (gGapPause) {
        if (!ResumeGotoAfterGap(now)) {
            if (gMode != Mode::Goto) return;
            gLastMsg = "wait_resume";
            return;
        }
    }

    // F5 真正在打怪时不赶路（HardPause 如 AutoSupply 开趟不算；中途再开 F5 立刻停）
    // 重连后 F5 仍勾选：在途 Goto 优先，Combat 已对 IsActive 让路。
    if (simple_combat::IsFarmingActive() && !gKeepGotoOverFarm) {
        const std::string cur = ports::travel::CurrentMapKey();
        SetIdle("combat_on", FailKind::CombatOn);
        if (!gLastCombatOnLogMs || now - gLastCombatOnLogMs >= 2000) {
            gLastCombatOnLogMs = now;
            x::runtime::LogW("Travel", "stop: F5 farming active during goto @%s -> %s",
                             cur.c_str(), gTarget.c_str());
        }
        NotifyTravelOutcome(FailKind::CombatOn, cur, gTarget);
        return;
    }

    const auto scene = ports::world::GetSceneState();
    const bool play = ports::world::IsInMapScene() && ports::world::IsPlayReady();
    // InterStage 时 MapId（_currentMapData）仍可读；用作 transfer 旁证。
    const std::string curPeek = ports::travel::CurrentMapKey();

    if (!play) {
        NotePlayReadyGate(now, false);
        if (gNextHopReadyAt) gNextHopReadyAt = now + kMidHopSettleMs;

        if (!gExpectMap.empty() || gUniqueLate.active) {
            if (MapLeftPendingFrom(curPeek)) {
                NoteTransferStarted(curPeek, now, "map_id");
            } else if (scene == ports::world::SceneState::InterStage && !gExpectMap.empty()) {
                NoteTransferStarted(curPeek, now, "interstage");
            }
            if (gTransferSeen && NoteInterStageStuck(now, curPeek)) return;
            // BIN 8fa033：MAP_CHANGED 后 InterStage/~1s 等 Field 时已 Disarm 旋翼，
            // 此窗无 hover → 每跳 curFh=0 无限掉落。托台优先于稳图。
            gLastMsg = gTransferSeen ? "wait_wm_field" : "wait_play_ready";
            TickSettleHover(now, gLastMsg.c_str());
            return;
        }
        gLastMsg = "wait_play_ready";
        return;
    }
    NotePlayReadyGate(now, true);

    const std::string cur = curPeek.empty() ? ports::travel::CurrentMapKey() : curPeek;
    if (cur.empty()) return;
    NoteMapKeyForStable(cur, now);

    // 唯一桥迟到观察窗：等 WM Field 闭合，超时才 FakeFireStop
    if (gUniqueLate.active) {
        if (cur != gUniqueLate.fromMap && !gUniqueLate.fromMap.empty()) {
            if (!WmFieldClosed() || !PlayReadyStable(now)) {
                if (MapLeftPendingFrom(cur)) NoteTransferStarted(cur, now, "unique_late");
                if (NoteInterStageStuck(now, cur)) return;
                gLastMsg = "unique_bridge_enter_stable";
                TickSettleHover(now, "unique_bridge_enter_stable");
                return;
            }
            x::runtime::LogI("Travel", "uniqueBridge late map-change %s -> %s (wm Field)",
                             gUniqueLate.fromMap.c_str(), cur.c_str());
            gPendingFrom = gUniqueLate.fromMap;
            gPendingName = gUniqueLate.hintName;
            gPendingSeedId = "seed:" + gUniqueLate.fromMap + "/" + gUniqueLate.hintName;
            gFiredPortal = gUniqueLate.hintName;
            ClearLateWait();
            OnMapEnterConfirmed(cur, now);
            return;
        }
        if (static_cast<int>(now - gUniqueLate.untilMs) >= 0) {
            const std::string lateFrom = gUniqueLate.fromMap;
            const std::string lateName = gUniqueLate.hintName;
            SetIdle("fake_fire_stop", FailKind::FakeFireStop);
            x::runtime::LogW("Travel", "uniqueBridge late-wait timeout from=%s name=%s",
                             lateFrom.c_str(), lateName.c_str());
            NotifyTravelOutcome(FailKind::FakeFireStop, lateFrom, gTarget);
            return;
        }
        gLastMsg = "unique_bridge_late_wait";
        TickSettleHover(now, "unique_bridge_late_wait");
        return;
    }

    // 等待换图确认：到站真源 = WM Field 闭合；MapId 闪变只记 transfer。
    if (!gExpectMap.empty()) {
        if (MapLeftPendingFrom(cur)) {
            NoteTransferStarted(cur, now, "map_id_play");
            if (!WmFieldClosed() || !PlayReadyStable(now)) {
                gLastMsg = "map_enter_stable";
                TickSettleHover(now, "map_enter_stable");
                return;
            }
            OnMapEnterConfirmed(cur, now);
            return;
        }
        // 仍在出发图：静默窗内不假火；真卸图则等 Field。
        if (gTransferSeen) {
            gLastMsg = "wait_wm_field";
            TickSettleHover(now, "wait_wm_field");
            // BIN cb18f8：假 MAP_TRANSITION 同图仍 PlayReady → 旧逻辑无限等，补给 180s 超时。
            // MapId 未离出发图且 Field 已稳：清假 latch，走假火软确认重试贴门。
            if (!MapLeftPendingFrom(cur) && WmFieldClosed() && PlayReadyStable(now)) {
                const DWORD wall = GetTickCount();
                const DWORD waitMs =
                    (!gPendingName.empty() &&
                     gGraph.IsUniqueBridgeName(gPendingFrom, gTarget, gPendingName))
                        ? kHopWaitUniqueBridgeMs
                        : kHopWaitMs;
                const DWORD needMs = waitMs > kPostFireQuietMs ? waitMs : kPostFireQuietMs;
                if (gHopStartedMs != 0 && wall - gHopStartedMs > needMs) {
                    x::runtime::LogW(
                        "Travel",
                        "wait_wm_field same-map timeout from=%s expect=%s → clear latch+soft",
                        gPendingFrom.c_str(),
                        gExpectMap.empty() ? "?" : gExpectMap.c_str());
                    ClearTransferWatch();
                    if (NoteNoMapChangeTimeout(wall)) return;
                }
            } else if (NoteInterStageStuck(now, cur)) {
                return;
            }
            return;
        }
        const DWORD wall = GetTickCount();
        const DWORD waitMs =
            (!gPendingName.empty() &&
             gGraph.IsUniqueBridgeName(gPendingFrom, gTarget, gPendingName))
                ? kHopWaitUniqueBridgeMs
                : kHopWaitMs;
        const DWORD needMs = waitMs > kPostFireQuietMs ? waitMs : kPostFireQuietMs;
        if (gHopStartedMs != 0 && wall - gHopStartedMs > needMs) {
            if (NoteNoMapChangeTimeout(wall)) return;
        }
        return;
    }

    if (cur == gTarget) {
        if (!gArriveReadyAt) gArriveReadyAt = now + kArriveStableMs;
        if (static_cast<int>(now - gArriveReadyAt) < 0 || !WmFieldClosed() ||
            !PlayReadyStable(now)) {
            gLastMsg = "arrive_settle";
            TickSettleHover(now, "arrive_settle");
            return;
        }
        // 稳图够了仍未挂台：继续 hover，超时再交棒 Combat 落台（防到站即坠）。
        ports::teleport::FlightState arriveSt{};
        const bool arriveFlightOk =
            ports::teleport::QueryFlightState(arriveSt) && arriveSt.ok;
        const bool arriveOnFh = arriveFlightOk && arriveSt.onFh;
        const DWORD landDeadline = gArriveReadyAt + kArriveLandExtraMs;
        if (!arriveOnFh && static_cast<int>(now - landDeadline) < 0) {
            gLastMsg = "arrive_land";
            TickSettleHover(now, "arrive_land");
            return;
        }
        SetIdle("arrived");
        x::runtime::LogI("Travel", "arrived %s onFh=%d", cur.c_str(), arriveOnFh ? 1 : 0);
        if (!arriveOnFh) {
            // Travel 已 Idle：交棒 Combat 软着陆（同测谎落台）。
            simple_combat::RequestSafeLand("travel_arrive_airborne");
        }
        SaveGraph();
        return;
    }
    gArriveReadyAt = 0;

    // 硬门禁：hop settle 未完成不准开火（首跳 firstSettle / 换图后 midhop 同窗）
    if (gNextHopReadyAt) {
        if (static_cast<int>(now - gNextHopReadyAt) < 0) {
            gLastMsg = "hop_settle";
            TickSettleHover(now, "hop_settle");
            return;
        }
        if (!PlayReadyStable(now)) {
            gLastMsg = "play_ready_stable";
            TickSettleHover(now, "play_ready_stable");
            return;
        }
        ports::teleport::FlightState hopSt{};
        const bool hopFlightOk = ports::teleport::QueryFlightState(hopSt) && hopSt.ok;
        const bool hopOnFh = hopFlightOk && hopSt.onFh;
        const DWORD hopLandDeadline = gNextHopReadyAt + kHopLandExtraMs;
        if (!hopOnFh && static_cast<int>(now - hopLandDeadline) < 0) {
            gLastMsg = "hop_land";
            TickSettleHover(now, "hop_land");
            return;
        }
        x::runtime::LogI("Travel", "hop settle done map=%s onFh=%d", cur.c_str(),
                         hopOnFh ? 1 : 0);
        gNextHopReadyAt = 0;
    } else if (!PlayReadyStable(now)) {
        gLastMsg = "play_ready_stable";
        TickSettleHover(now, "play_ready_stable");
        return;
    }

    if (gHopIdx >= gHops.size() || gHops[gHopIdx].map != cur) {
        if (!ReplanOrStop(cur)) return;
    }

    const Hop hop = gHops[gHopIdx];
    const std::string hint = PortalNameHint(hop);
    if (hint.empty()) {
        SetIdle("no_portal_name");
        return;
    }

    std::vector<ports::travel::PortalInfo> live;
    const std::string liveName = ResolveLivePortalName(hop.portalId, hint, hop.destMap, &live);
    if (!live.empty()) {
        std::string mk = ports::travel::CurrentMapKey();
        UpsertLiveEdges(mk, live);
    }
    if (liveName.empty()) {
        gLastMsg = "resolve_empty " + hint;
        if (now - gHopStartedMs > 2000) {
            gHopStartedMs = now;
            LogResolveEmptyDiag(hint, hop.portalId, live);
        }
        return;
    }

    // FirePortalByName：Impact 贴门循环 + CheckMove；禁止持 gMu（否则 QuerySnapshot/Stop 卡住）
    const std::string hopPortalId = hop.portalId;
    const std::string hopDest = hop.destMap;
    const uint32_t fireEpoch = gFireEpoch;
    lock.unlock();
    std::string fireResult;
    const bool firedOk = ports::travel::FirePortalByName(liveName, fireResult);
    lock.lock();
    if (gMode != Mode::Goto || fireEpoch != gFireEpoch) {
        x::runtime::LogW("Travel", "discard fire commit name=%s ok=%d epoch=%u/%u mode=%d",
                         liveName.c_str(), firedOk ? 1 : 0, fireEpoch, gFireEpoch,
                         (int)gMode);
        return;
    }

    if (!firedOk) {
        gLastMsg = fireResult;
        if (fireResult == "INVULN_OFF") {
            // Impact 贴门需无敌；Hold 异常时立刻停，禁止空转。
            ClearTransientFire();
            char detail[128]{};
            snprintf(detail, sizeof(detail), "%s INVULN_OFF", liveName.c_str());
            x::runtime::LogW("Travel", "fire stop %s: impact stick needs invuln",
                             liveName.c_str());
            SetIdle("invuln_off", FailKind::FireStuck);
            NotifyTravelOutcome(FailKind::FireStuck, cur, gTarget, detail);
            return;
        }
        if (IsTransientFireFail(fireResult)) {
            (void)NoteTransientFireFail(now, cur, liveName, fireResult);
            return;
        }
        ClearTransientFire();
        // 门禁用等：按假火软确认路径处理一次
        if (fireResult == "PORTAL_DISABLED" || fireResult == "NO_PORTAL" ||
            fireResult == "FH0_FORBID") {
            gPendingFrom = cur;
            gPendingSeedId = hopPortalId;
            gPendingName = hint;
            gFiredPortal = liveName;
            if (NoteNoMapChangeTimeout(now)) return;
            return;
        }
        if (now - gHopStartedMs > 2000) {
            gHopStartedMs = now;
            x::runtime::LogW("Travel", "fire fail %s: %s", liveName.c_str(), fireResult.c_str());
        }
        return;
    }

    ClearTransientFire();
    ClearTransferWatch();
    gPendingFrom = cur;
    gPendingSeedId = hopPortalId;
    gPendingName = hint;
    gFiredPortal = liveName;
    gExpectMap = hopDest;
    // 贴门/↑ 可能耗时数秒；禁止用 Tick 入口的 stale now，否则 uniqueBridge
    // 1.5s 窗几乎一开火就耗尽 → 误 soft → 无意义补 ↑（BIN 16:10 FIRED→452ms soft）。
    gHopStartedMs = GetTickCount();
    gLastMsg = "fire " + liveName + " -> " + hopDest;
    gLastSnapMs = gHopStartedMs;
    if (fireResult == "MAP_CHANGED" || fireResult == "MAP_TRANSITION") {
        const std::string to = ports::travel::CurrentMapKey();
        const auto scene = ports::world::GetSceneState();
        const bool left = MapLeftPendingFrom(to);
        // 真换图：MapId 已离出发，或已进 InterStage / 非 PlayReady。
        // BIN cb18f8：勇士 east00 freefall 也会报 MAP_TRANSITION，图号未变且仍 Field
        // → 若仍 NoteTransferStarted，PlayReady 同图 wait_wm_field 会死等。
        const bool unloading =
            scene == ports::world::SceneState::InterStage || !ports::world::IsPlayReady();
        if (left || unloading) {
            NoteTransferStarted(to.empty() ? hopDest : to, gHopStartedMs,
                                fireResult == "MAP_CHANGED" ? "fire_map_changed"
                                                            : "fire_map_transition");
        } else {
            x::runtime::LogW(
                "Travel",
                "ignore same-map %s name=%s from=%s (no transfer latch)",
                fireResult.c_str(), liveName.c_str(), cur.c_str());
        }
    }
    const DWORD sinceGoto =
        (gGotoAtMs != 0) ? (gHopStartedMs - gGotoAtMs) : 0;
    x::runtime::LogI("Travel",
                     "FIRED name=%s hint=%s expect=%s mode=%s sinceGoto=%ums path=%s",
                     liveName.c_str(), hint.c_str(), hopDest.c_str(),
                     ports::travel::FireModeName(ports::travel::GetFireMode()),
                     (unsigned)sinceGoto,
                     fireResult.rfind("FIRED_", 0) == 0 ? fireResult.c_str() : "ok");
}

void Tick(DWORD now) {
    std::string disk;
    if (ConsumeDiskCmd(disk)) EnqueueCmd(disk);
    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(gCmdMu);
        cmd.swap(gPendingCmd);
    }
    {
        std::unique_lock<std::mutex> lock(gMu);
        if (!cmd.empty()) HandleCmd(cmd);
        TryFlushPendingGoto();
        if (gMode == Mode::Goto) TickGoto(now, lock);
    }
}

DWORD WINAPI Worker(LPVOID) {
    timeBeginPeriod(1);
    x::runtime::LogI("Travel", "worker start");
    EnsureGraphLoaded();
    while (!gWorkerStop.load()) {
        Tick(GetTickCount());
        Sleep(kTickMs);
    }
    x::runtime::LogI("Travel", "worker stop");
    timeEndPeriod(1);
    return 0;
}

}  // namespace

void Init() {
    if (!kEnabled) return;
    gWorkerStop.store(false);
    x::runtime::LogI("Travel", "feature init (classic same-plate goto)");
    ports::travel::Init();
}

void PreloadGraph() {
    if (!kEnabled) return;
    EnsureGraphLoaded();
}

void Shutdown() {
    StopWorker();
    // worker 已停：清状态并释放赶路无敌，避免进程退出/重载后仍卡住 Hold
    std::lock_guard<std::mutex> lock(gMu);
    SetIdle("shutdown");
}

void StartWorker() {
    if (!kEnabled) return;
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    gWorkerThread.store(th);
}

void StopWorker() {
    gWorkerStop.store(true);
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

void RequestGoto(const char* target) {
    if (!target || !target[0]) return;
    EnqueueCmd(std::string("goto ") + target);
}

void RequestStop() { EnqueueCmd("stop"); }

void RequestSave() { EnqueueCmd("save"); }

bool IsFeatureEnabled() { return kEnabled; }

bool IsActive() {
    // 排队中的 goto 也算在途：遇人/换频必须让路，不能等 worker 50ms 才置 Goto。
    std::string queued;
    {
        std::lock_guard<std::mutex> lock(gCmdMu);
        queued = gPendingCmd;
    }
    if (queued.rfind("goto", 0) == 0) return true;
    std::lock_guard<std::mutex> lock(gMu);
    return gMode != Mode::Idle || !gPendingGoto.empty();
}

bool QuerySnapshot(Snapshot& out) {
    std::lock_guard<std::mutex> lock(gMu);
    out = Snapshot{};
    out.mode = (gMode == Mode::Goto) ? UiMode::Goto : UiMode::Idle;
    out.failKind = gFailKind;
    out.mapCount = gGraph.MapCount();
    out.edgeCount = gGraph.EdgeCount();
    out.hopIdx = static_cast<int>(gHopIdx);
    out.hopTotal = static_cast<int>(gHops.size());
    out.fakeFireCount = gFakeFire.count;
    out.playReady = ports::world::IsPlayReady();
    out.lastUpdateMs = gLastSnapMs ? gLastSnapMs : GetTickCount();
    const std::string cur = ports::travel::CurrentMapKey();
    strncpy_s(out.curMap, cur.c_str(), _TRUNCATE);
    strncpy_s(out.gotoTarget, gTarget.c_str(), _TRUNCATE);
    strncpy_s(out.lastMsg, gLastMsg.c_str(), _TRUNCATE);
    return true;
}

int PathHopCount(const char* srcMap, const char* dstMap) {
    if (!srcMap || !dstMap || !srcMap[0] || !dstMap[0]) return -1;
    EnsureGraphLoaded();
    const std::string a = ResolveTarget(srcMap);
    const std::string b = ResolveTarget(dstMap);
    if (a.empty() || b.empty()) return -1;
    if (a == b) return 0;
    std::lock_guard<std::mutex> lock(gMu);
    return gGraph.PathDistance(a, b);
}

namespace {

bool PrefixTownOutdoor(const char* fromMap, char* out, size_t outSz) {
    if (!fromMap || !fromMap[0] || !out || outSz < 10) return false;
    const int id = atoi(fromMap);
    if (id < 100000000) return false;
    // 仅作「同大区前缀」候选种子；是否真城镇由 IsMapInfoTown 再筛。
    const int town = (id / 1000000) * 1000000;
    snprintf(out, outSz, "%d", town);
    return true;
}

}  // namespace

bool PredictReturnScrollTownOutdoor(const char* fromMap, char* out, size_t outSz) {
    if (!out || outSz < 10 || !fromMap || !fromMap[0]) return false;
    out[0] = '\0';

    char prefix[16]{};
    const bool havePrefix = PrefixTownOutdoor(fromMap, prefix, sizeof(prefix));
    const char* bin = x::runtime::GetBinDir();

    EnsureGraphLoaded();
    std::vector<std::string> maps;
    {
        std::lock_guard<std::mutex> lock(gMu);
        gGraph.ListMaps(maps);
    }

    int bestHops = INT_MAX;
    std::string bestMap;
    auto consider = [&](const char* cand) {
        if (!cand || !cand[0]) return;
        const int id = atoi(cand);
        if (!xcat::IsMapInfoTown(bin, id)) return;
        const int hops = PathHopCount(fromMap, cand);
        if (hops < 0) return;
        if (hops < bestHops) {
            bestHops = hops;
            bestMap = cand;
        }
    };

    for (const auto& m : maps) consider(m.c_str());
    if (havePrefix) consider(prefix);

    if (!bestMap.empty()) {
        strncpy_s(out, outSz, bestMap.c_str(), _TRUNCATE);
        x::runtime::LogI("Travel", "predictScrollTown from=%s -> %s hops=%d (prefix=%s)", fromMap,
                         out, bestHops, havePrefix ? prefix : "-");
        return true;
    }
    if (havePrefix && xcat::IsMapInfoTown(bin, atoi(prefix))) {
        strncpy_s(out, outSz, prefix, _TRUNCATE);
        x::runtime::LogW("Travel", "predictScrollTown from=%s fallback prefix=%s (no hop town)",
                         fromMap, out);
        return true;
    }
    return false;
}

}  // namespace x::features::travel
