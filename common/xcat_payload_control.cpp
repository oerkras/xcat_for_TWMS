#include "xcat_payload_control.h"

#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace xcat {
namespace {

uint64_t NowTickMs() { return GetTickCount64(); }

bool EnsurePayloadStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

void CopyWorldName(char* dst, size_t dstCap, const char* src) {
    if (!dst || dstCap == 0) return;
    dst[0] = 0;
    if (!src || !src[0]) return;
    strncpy_s(dst, dstCap, src, _TRUNCATE);
}

// 飞行武装会话文件（跨进程 live IPC）；不进 user.ini，启动清零。
std::string FlyArmedSessionPath(const char* binDir) {
    return JoinBinPath(binDir, "state\\fly_armed");
}

bool ReadFlyArmedSession(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    const std::string path = FlyArmedSessionPath(binDir);
    FILE* fp = nullptr;
    if (FopenUtf8(&fp, path, L"rb") != 0 || !fp) return false;
    char buf[8]{};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return false;
    return buf[0] == '1';
}

bool WriteFlyArmedSession(const char* binDir, bool on) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsurePayloadStateDir(binDir)) return false;
    const std::string path = FlyArmedSessionPath(binDir);
    FILE* fp = nullptr;
    if (FopenUtf8(&fp, path, L"wb") != 0 || !fp) return false;
    const int rc = fputc(on ? '1' : '0', fp);
    const int flushRc = fflush(fp);
    fclose(fp);
    return rc != EOF && flushRc == 0;
}

}  // namespace

void ClearFlyArmedSession(const char* binDir) {
    (void)WriteFlyArmedSession(binDir, false);
}

void PayloadControlSetDefaults(PayloadControl& out) {
    out = PayloadControl{};
    out.magic = kPayloadControlMagic;
    out.version = kPayloadControlVersion;
    out.invuln = 1;  // 默认开；ini 显式 0 仍可关
    out.attackAccel = 0;
    out.attackAccelClearBusy = 0;  // 默认关（实验·清忙锁）
    out.attackAccelClearBusyMinIntervalMs = kAttackAccelClearBusyMinIntervalDefaultMs;
    out.attackAccelCutLayer = 0;
    out.attackAccelSkipPrepare = 0;  // 默认关
    out.attackAccelBooster = 0;      // 默认关
    out.attackAccelActionSpeed = 0;  // 默认关（A 系 nSpeed_）
    out.attackAccelPartyBooster = 0; // 默认关（TempStats[4]）
    out.attackAccelPartyBoosterValue = kAttackAccelPartyBoosterValueDefault;
    out.attackAccelBreakDegreeFloor = 0; // 默认关（degree 下限破限）
    out.attackAccelBreakDegreeFloorLo = kAttackAccelBreakDegreeFloorLoDefault;
    out.attackSameFrameBurst = kAttackSameFrameBurstDefault;
    out.fly = 0;
    out.flyMode = kFlyModeDefault;
    out.flyHopCdMs = kFlyHopCdDefaultMs;
    out.flySpeedPct = kFlySpeedPctDefault;
    out.autoEnter = 1;  // 默认开（与面板图例一致：1 雪吉拉 / 槽1）
    out.charSlot = 1;
    out.worldId = kDefaultWorldId;
    CopyWorldName(out.worldName, sizeof(out.worldName), kDefaultWorldName);
    out.hpPotion = 1;
    out.mpPotion = 1;
    out.hpThresholdPct = 50;
    out.mpThresholdPct = 30;
    out.petSummon = 0;
    out.petSummonRequireFood = 0;
    out.multiSkill = 0;
    out.multiSkillGapMs = kMultiSkillGapDefaultMs;
    out.multiSkillSafeStagger = 1;
    out.multiSkillSendUseRequest = 0;
    out.simpleCombat = 0;
    out.simpleCombatSmartInterval = 0;
    out.simpleCombatAttackIntervalMs = kSimpleCombatAttackIntervalDefaultMs;
    out.simpleCombatTickMs = kSimpleCombatTickDefaultMs;
    out.mobScanIntervalMs = kMobScanIntervalDefaultMs;
    out.simpleCombatAttackHoldMs = kAttackHoldDefaultMs;
    out.clusterWeight = kClusterWeightDefault;
    out.simpleCombatTeleport = 0;
    out.simpleCombatImpactApproach = 1;
    out.simpleCombatFlySpeedPct = kHeliSpeedPctDefault;
    out.simpleCombatHumanWalk = 0;  // 与 Impact 互斥；面板单选
    out.simpleCombatTeleportMinDx = kCombatTeleportMinDxDefault;
    out.simpleCombatTeleportStandOff = kCombatTeleportStandOffDefault;
    out.simpleCombatStandOffCustom = kCombatStandOffCustomDefault;
    out.simpleCombatStandOffX = kCombatStandOffXDefault;
    out.simpleCombatStandOffY = kCombatStandOffYDefault;
    out.simpleCombatGroundSpoof = kCombatGroundSpoofDefault;
    out.simpleCombatAntiJitter = kCombatAntiJitterDefault;
    out.simpleCombatAntiHug = 0;
    out.meleeVeto = 0;
    out.simpleCombatTeleportCooldownMs = kCombatTeleportCooldownDefaultMs;
    out.simpleCombatTeleportMaxHop = kCombatTeleportMaxHopDefault;
    out.simpleCombatLiveStep = 0;
    out.simpleCombatOneshotMaxHp = kCombatOneshotMaxHpDefault;
    out.simpleCombatOneshotMinBumps = kCombatOneshotMinBumpsDefault;
    out.simpleCombatOneshotMinFires = kCombatOneshotMinFiresDefault;
    out.simpleCombatOneshotMinLagMs = kCombatOneshotMinLagMsDefault;
    out.simpleCombatOneshotFoxFillGapMs = kCombatOneshotFoxFillGapDefaultMs;
    out.pumpCongestionThreshold = kPumpCongestionDefault;
    out.pumpDrainBudget = kPumpDrainBudgetDefault;
    out.attackRpc = 0;
    out.attackRpcMobs = kAttackRpcMobsDefault;
    out.attackRpcIntervalMs = kAttackRpcIntervalDefaultMs;
    out.attackRpcDamage = kAttackRpcDamageDefault;
    out.curFhGateBypass = 0;
    out.autoLie = 1;  // 默认开启；ini 有显式项时仍以 ini 为准
    out.autoLieDryRun = 0;
    out.autoLieMouseRegionOverlay = 0;
    out.autoLieAlarmTestSeq = 0;
    out.autoLieMouseSmokeSeq = 0;
    out.autoLieMouseSimSeq = 0;
    out.manualRejoinSeq = 0;
    out.teleportTestSeq = 0;
    out.teleportNativeTestSeq = 0;
    out.teleportKickStressSeq = 0;
    out.teleportKickStressFineSeq = 0;
    out.teleportKickStressFine10Seq = 0;
    out.teleportKickStressLocalSeq = 0;
    out.impactNockBackTestSeq = 0;
    out.impactSetNextTestSeq = 0;
    out.impactImpulseDir = kImpactImpulseDirDefault;
    out.impactImpulseVx = kImpactImpulseVxDefault;
    out.impactImpulseVy = kImpactImpulseVyDefault;
    out.impactHopTestSeq = 0;
    out.impactHopDeltaX = kImpactHopDeltaXDefault;
    out.impactHopForce = 0;
    out.softLoginDismissSeq = 0;
    out.autoRelogin = 1;
    out.autoReloginStopCombat = 0;
    out.autoReloginReconnect = 0;
    out.autoReloginGmEscalate = 1;
    out.hideOtherPlayers = 0;
    out.frameLock = 1;  // 默认开
    out.frameLockFps = kFrameLockFpsDefault;
    out.dropAlertBypass = 0;  // 默认关
    out.auctionTownBypass = 1;  // 默认开
    out.restMpAccel = 0;  // 实验·默认关
    out.restMpAccelIntervalMs = kRestMpAccelIntervalDefaultMs;
    out.infiniteStars = 0;  // 实验·默认关
    out.autoSell = 0;
    out.autoSellShopMap[0] = '\0';
    out.autoSellReturnFarmSeq = 0;
    out.autoSellAbortSeq = 0;
    out.launcherHangupSchedule = 0;
    out.launcherHangupScheduleMask = kHangupScheduleMaskAll;
    out.launcherWatchdog = 1;
    out.launcherWatchdogNoExpSec = kWatchdogNoExpSecDefault;
    out.launcherWatchdogCooldownSec = kWatchdogCooldownSecDefault;
    out.writeTickMs = 0;
}

bool ReadPayloadControl(const char* binDir, PayloadControl& out) {
    PayloadControlSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    bool b = false;
    uint32_t u = 0;
    if (IniGetBool(ini, "core", "invuln", b)) out.invuln = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "attackAccel", b)) out.attackAccel = b ? 1u : 0u;
    if (!kAttackAccelUserEnabled) out.attackAccel = 0;
    // 实验清忙锁（独立字段）。旧版曾把本 key 误并进 attackAccel；现各自读写。
    if (IniGetBool(ini, "core", "attackAccelClearBusy", b))
        out.attackAccelClearBusy = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "attackAccelClearBusyMinIntervalMs", u))
        out.attackAccelClearBusyMinIntervalMs = ClampAttackAccelClearBusyMinIntervalMs(u);
    else
        out.attackAccelClearBusyMinIntervalMs = kAttackAccelClearBusyMinIntervalDefaultMs;
    if (IniGetBool(ini, "core", "attackAccelCutLayer", b)) out.attackAccelCutLayer = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "attackAccelSkipPrepare", b))
        out.attackAccelSkipPrepare = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "attackAccelBooster", b))
        out.attackAccelBooster = b ? 1u : 0u;
    if (!kAttackAccelBoosterUserEnabled) out.attackAccelBooster = 0;
    if (IniGetBool(ini, "core", "attackAccelActionSpeed", b))
        out.attackAccelActionSpeed = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "attackAccelPartyBooster", b))
        out.attackAccelPartyBooster = b ? 1u : 0u;
    {
        int32_t pv = kAttackAccelPartyBoosterValueDefault;
        if (IniGetI32(ini, "core", "attackAccelPartyBoosterValue", pv))
            out.attackAccelPartyBoosterValue = ClampAttackAccelPartyBoosterValue(pv);
        else
            out.attackAccelPartyBoosterValue = kAttackAccelPartyBoosterValueDefault;
    }
    if (IniGetBool(ini, "core", "attackAccelBreakDegreeFloor", b))
        out.attackAccelBreakDegreeFloor = b ? 1u : 0u;
    {
        int32_t lo = kAttackAccelBreakDegreeFloorLoDefault;
        if (IniGetI32(ini, "core", "attackAccelBreakDegreeFloorLo", lo))
            out.attackAccelBreakDegreeFloorLo = ClampAttackAccelBreakDegreeFloorLo(lo);
        else
            out.attackAccelBreakDegreeFloorLo = kAttackAccelBreakDegreeFloorLoDefault;
    }
    if (IniGetBool(ini, "core", "finalAttackForce", b)) out.finalAttackForce = b ? 1u : 0u;
    if (!kFinalAttackForceUserEnabled) out.finalAttackForce = 0;
    if (IniGetBool(ini, "core", "skillMaxLevel", b)) out.skillMaxLevel = b ? 1u : 0u;
    if (!kSkillMaxLevelUserEnabled) out.skillMaxLevel = 0;
    // 同帧连打已关停：无视落盘值，强制 1。
    out.attackSameFrameBurst = kAttackSameFrameBurstDefault;
    // fly 开关不读 ini（历史 key 忽略）；只读会话态 state/fly_armed。
    out.fly = ReadFlyArmedSession(binDir) ? 1u : 0u;
    if (IniGetU32(ini, "core", "flyMode", u)) out.flyMode = ClampFlyMode(u);
    if (IniGetU32(ini, "core", "flyHopCdMs", u)) out.flyHopCdMs = ClampFlyHopCdMs(u);
    if (IniGetU32(ini, "core", "flySpeedPct", u))
        out.flySpeedPct = ClampHeliSpeedPct(u);
    else
        out.flySpeedPct = kFlySpeedPctDefault;
    if (IniGetBool(ini, "core", "autoEnter", b)) out.autoEnter = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "hpPotion", b)) out.hpPotion = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "mpPotion", b)) out.mpPotion = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "petSummon", b)) out.petSummon = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "petSummonRequireFood", b)) out.petSummonRequireFood = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "multiSkill", b)) out.multiSkill = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "multiSkillSafeStagger", b))
        out.multiSkillSafeStagger = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "multiSkillSendUseRequest", b))
        out.multiSkillSendUseRequest = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "simpleCombat", b)) out.simpleCombat = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "simpleCombatSmartInterval", b))
        out.simpleCombatSmartInterval = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "multiSkillGapMs", u))
        out.multiSkillGapMs = ClampMultiSkillGapMs(u);
    if (IniGetU32(ini, "core", "simpleCombatAttackIntervalMs", u)) {
        // 旧默认 50 / 46 → 123；显式调过其它值保留。
        if (u == kSimpleCombatAttackIntervalLegacyDefaultMs ||
            u == kSimpleCombatAttackIntervalLegacyDefaultMs46)
            u = kSimpleCombatAttackIntervalDefaultMs;
        out.simpleCombatAttackIntervalMs = ClampSimpleCombatAttackIntervalMs(u);
    }
    out.simpleCombatAttackIntervalMs = EffectiveSimpleCombatAttackIntervalMs(
        out.simpleCombatAttackIntervalMs, out.attackAccel);
    if (IniGetU32(ini, "core", "simpleCombatTickMs", u))
        out.simpleCombatTickMs = ClampSimpleCombatTickMs(u);
    else
        out.simpleCombatTickMs = kSimpleCombatTickDefaultMs;
    out.simpleCombatTickMs = ClampSimpleCombatTickMs(out.simpleCombatTickMs);
    if (IniGetU32(ini, "core", "mobScanIntervalMs", u)) {
        // 旧默认 50 → 20；显式调过其它值保留。
        if (u == kMobScanIntervalLegacyDefaultMs) u = kMobScanIntervalDefaultMs;
        out.mobScanIntervalMs = ClampMobScanIntervalMs(u);
    } else
        out.mobScanIntervalMs = kMobScanIntervalDefaultMs;
    out.mobScanIntervalMs = ClampMobScanIntervalMs(out.mobScanIntervalMs);
    if (IniGetU32(ini, "core", "simpleCombatAttackHoldMs", u))
        out.simpleCombatAttackHoldMs = ClampAttackHoldMs(u);
    else
        out.simpleCombatAttackHoldMs = kAttackHoldDefaultMs;
    if (IniGetU32(ini, "core", "clusterWeight", u)) out.clusterWeight = ClampClusterWeight(u);
    // fill+Doing 贴怪已禁用（封禁风险）：读盘亦强制关。
    out.simpleCombatTeleport = 0u;
    // 追怪位移：新键 simpleCombatAirApproach；旧键 simpleCombatImpactApproach 仅兜底。
    if (IniGetBool(ini, "core", "simpleCombatAirApproach", b))
        out.simpleCombatImpactApproach = b ? 1u : 0u;
    else if (IniGetBool(ini, "core", "simpleCombatImpactApproach", b))
        out.simpleCombatImpactApproach = b ? 1u : 0u;
    else
        out.simpleCombatImpactApproach = 1u;
    if (IniGetU32(ini, "core", "simpleCombatFlySpeedPct", u))
        out.simpleCombatFlySpeedPct = ClampHeliSpeedPct(u);
    else
        out.simpleCombatFlySpeedPct = kHeliSpeedPctDefault;
    if (IniGetBool(ini, "core", "simpleCombatHumanWalk", b))
        out.simpleCombatHumanWalk = b ? 1u : 0u;
    else
        out.simpleCombatHumanWalk = 0u;  // 与空中贴怪默认互斥；Normalize 亦会压排他
    if (IniGetBool(ini, "core", "simpleCombatLiveStep", b))
        out.simpleCombatLiveStep = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "attackRpc", b)) out.attackRpc = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "attackRpcMobs", u)) out.attackRpcMobs = ClampAttackRpcMobs(u);
    if (IniGetU32(ini, "core", "attackRpcIntervalMs", u))
        out.attackRpcIntervalMs = ClampAttackRpcIntervalMs(u);
    if (IniGetU32(ini, "core", "attackRpcDamage", u))
        out.attackRpcDamage = ClampAttackRpcDamage(u);
    if (IniGetBool(ini, "core", "curFhGateBypass", b)) out.curFhGateBypass = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoLie", b)) out.autoLie = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoLieDryRun", b)) out.autoLieDryRun = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoLieMouseRegionOverlay", b))
        out.autoLieMouseRegionOverlay = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "movepathFlushProbe", b)) out.movepathFlushProbe = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "galaxyTokenProbe", b)) out.galaxyTokenProbe = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "softLoginProbe", b)) out.softLoginProbe = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "softLoginDismissSeq", u)) out.softLoginDismissSeq = u;
    if (IniGetU32(ini, "core", "autoLieAlarmTestSeq", u)) out.autoLieAlarmTestSeq = u;
    if (IniGetU32(ini, "core", "autoLieMouseSmokeSeq", u)) out.autoLieMouseSmokeSeq = u;
    if (IniGetU32(ini, "core", "autoLieMouseSimSeq", u)) out.autoLieMouseSimSeq = u;
    if (IniGetU32(ini, "core", "manualRejoinSeq", u)) out.manualRejoinSeq = u;
    if (IniGetU32(ini, "core", "teleportTestSeq", u)) out.teleportTestSeq = u;
    if (IniGetU32(ini, "core", "teleportNativeTestSeq", u)) out.teleportNativeTestSeq = u;
    if (IniGetU32(ini, "core", "teleportKickStressSeq", u)) out.teleportKickStressSeq = u;
    if (IniGetU32(ini, "core", "teleportKickStressFineSeq", u)) out.teleportKickStressFineSeq = u;
    if (IniGetU32(ini, "core", "teleportKickStressFine10Seq", u))
        out.teleportKickStressFine10Seq = u;
    if (IniGetU32(ini, "core", "teleportKickStressLocalSeq", u))
        out.teleportKickStressLocalSeq = u;
    // 位移试推 seq/参数：新键 moveProbe* / moveHop*；旧 impact* 仅兜底。
    if (IniGetU32(ini, "core", "moveProbeASeq", u))
        out.impactNockBackTestSeq = u;
    else if (IniGetU32(ini, "core", "impactNockBackTestSeq", u))
        out.impactNockBackTestSeq = u;
    if (IniGetU32(ini, "core", "moveProbeBSeq", u))
        out.impactSetNextTestSeq = u;
    else if (IniGetU32(ini, "core", "impactSetNextTestSeq", u))
        out.impactSetNextTestSeq = u;
    if (IniGetU32(ini, "core", "moveProbeDir", u))
        out.impactImpulseDir = ClampImpactImpulseDir(static_cast<int32_t>(u));
    else if (IniGetU32(ini, "core", "impactImpulseDir", u))
        out.impactImpulseDir = ClampImpactImpulseDir(static_cast<int32_t>(u));
    if (IniGetU32(ini, "core", "moveProbeVx", u))
        out.impactImpulseVx = ClampImpactImpulseSpeed(u);
    else if (IniGetU32(ini, "core", "impactImpulseVx", u))
        out.impactImpulseVx = ClampImpactImpulseSpeed(u);
    if (IniGetU32(ini, "core", "moveProbeVy", u))
        out.impactImpulseVy = ClampImpactImpulseSpeed(u);
    else if (IniGetU32(ini, "core", "impactImpulseVy", u))
        out.impactImpulseVy = ClampImpactImpulseSpeed(u);
    if (IniGetU32(ini, "core", "moveHopTestSeq", u))
        out.impactHopTestSeq = u;
    else if (IniGetU32(ini, "core", "impactHopTestSeq", u))
        out.impactHopTestSeq = u;
    {
        std::string dxStr;
        if ((IniGetString(ini, "core", "moveHopDeltaX", dxStr) ||
             IniGetString(ini, "core", "impactHopDeltaX", dxStr)) &&
            !dxStr.empty()) {
            out.impactHopDeltaX = ClampImpactHopDeltaX(static_cast<int32_t>(atoi(dxStr.c_str())));
        }
    }
    if (IniGetBool(ini, "core", "moveHopForce", b))
        out.impactHopForce = b ? 1u : 0u;
    else if (IniGetBool(ini, "core", "impactHopForce", b))
        out.impactHopForce = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoRelogin", b)) out.autoRelogin = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoReloginStopCombat", b))
        out.autoReloginStopCombat = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoReloginReconnect", b))
        out.autoReloginReconnect = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoReloginGmEscalate", b))
        out.autoReloginGmEscalate = b ? 1u : 0u;
    // v82: 旧厂默（关检测+开停手+开换频）→ 新厂默（开检测+关停手+关换频）；显式改过保留
    if (out.autoRelogin == 0 && out.autoReloginStopCombat == 1 &&
        out.autoReloginReconnect == 1) {
        out.autoRelogin = 1;
        out.autoReloginStopCombat = 0;
        out.autoReloginReconnect = 0;
    }
    if (IniGetBool(ini, "core", "hideOtherPlayers", b)) out.hideOtherPlayers = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "frameLock", b)) out.frameLock = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "frameLockFps", u)) out.frameLockFps = ClampFrameLockFps(u);
    if (IniGetBool(ini, "core", "dropAlertBypass", b)) out.dropAlertBypass = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "auctionTownBypass", b)) out.auctionTownBypass = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "restMpAccel", b)) out.restMpAccel = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "restMpAccelIntervalMs", u))
        out.restMpAccelIntervalMs = ClampRestMpAccelIntervalMs(u);
    if (IniGetBool(ini, "core", "infiniteStars", b)) out.infiniteStars = b ? 1u : 0u;
    if (!kInfiniteStarsUserEnabled) out.infiniteStars = 0;
    // core.autoSell* 已废弃：真源 [auto_supply]；此处强制清零，避免旧 key 干扰。
    out.autoSell = 0;
    out.autoSellShopMap[0] = '\0';
    out.autoSellReturnFarmSeq = 0;
    out.autoSellAbortSeq = 0;
    // Missing hangup key => stay at SetDefaults (0). Do not infer from mask alone.
    if (IniGetBool(ini, "core", "launcherHangupSchedule", b))
        out.launcherHangupSchedule = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "launcherHangupScheduleMask", u))
        out.launcherHangupScheduleMask = ClampHangupScheduleMask(u);
    if (IniGetBool(ini, "core", "launcherWatchdog", b)) out.launcherWatchdog = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "launcherWatchdogNoExpSec", u))
        out.launcherWatchdogNoExpSec = ClampWatchdogNoExpSec(u);
    if (IniGetU32(ini, "core", "launcherWatchdogCooldownSec", u))
        out.launcherWatchdogCooldownSec = ClampWatchdogCooldownSec(u);
    if (IniGetU32(ini, "core", "simpleCombatTeleportMinDx", u))
        out.simpleCombatTeleportMinDx = ClampCombatTeleportMinDx(u);
    if (IniGetU32(ini, "core", "simpleCombatTeleportStandOff", u)) {
        // 旧默认 25 → 12；显式调过其它值保留。
        if (u == kCombatTeleportStandOffLegacyDefault) u = kCombatTeleportStandOffDefault;
        out.simpleCombatTeleportStandOff = ClampCombatTeleportStandOff(u);
    }
    if (IniGetBool(ini, "core", "simpleCombatStandOffCustom", b))
        out.simpleCombatStandOffCustom = b ? 1u : 0u;
    // X 允许 0（贴着怪心），所以不能用「读到 0 就回默认」那套 —— 那是给
    // 「0 = 旧盘没这个键」的字段用的。这里靠 Custom 开关本身区分新旧盘：
    // 旧盘没有 Custom 键 ⇒ 恒 0 ⇒ X/Y 根本不参与计算。
    if (IniGetU32(ini, "core", "simpleCombatStandOffX", u))
        out.simpleCombatStandOffX = ClampCombatStandOffX(u);
    {
        int32_t sy = 0;
        if (IniGetI32(ini, "core", "simpleCombatStandOffY", sy))
            out.simpleCombatStandOffY = ClampCombatStandOffY(sy);
    }
    if (IniGetBool(ini, "core", "simpleCombatGroundSpoof", b))
        out.simpleCombatGroundSpoof = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "simpleCombatAntiJitter", b))
        out.simpleCombatAntiJitter = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "simpleCombatAntiHug", b))
        out.simpleCombatAntiHug = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "meleeVeto", b)) out.meleeVeto = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "simpleCombatTeleportCooldownMs", u))
        out.simpleCombatTeleportCooldownMs = ClampCombatTeleportCooldownMs(u);
    if (IniGetU32(ini, "core", "simpleCombatTeleportMaxHop", u)) {
        // 旧默认 400 / 520 → 550；显式调过其它值保留。
        if (u == kCombatTeleportMaxHopLegacyDefault || u == kCombatTeleportMaxHopPrevDefault)
            u = kCombatTeleportMaxHopDefault;
        out.simpleCombatTeleportMaxHop = ClampCombatTeleportMaxHop(u);
    }
    if (IniGetU32(ini, "core", "simpleCombatOneshotMaxHp", u))
        out.simpleCombatOneshotMaxHp = ClampCombatOneshotMaxHp(u);
    if (IniGetU32(ini, "core", "simpleCombatOneshotMinBumps", u))
        out.simpleCombatOneshotMinBumps = ClampCombatOneshotMinBumps(u);
    if (IniGetU32(ini, "core", "simpleCombatOneshotMinFires", u))
        out.simpleCombatOneshotMinFires = ClampCombatOneshotMinFires(u);
    if (IniGetU32(ini, "core", "simpleCombatOneshotMinLagMs", u))
        out.simpleCombatOneshotMinLagMs = ClampCombatOneshotMinLagMs(u);
    if (IniGetU32(ini, "core", "simpleCombatOneshotFoxFillGapMs", u)) {
        // 旧默认 280 → 0；用户显式调过其它值保留。
        if (u == kCombatOneshotFoxFillGapLegacyDefaultMs) u = kCombatOneshotFoxFillGapDefaultMs;
        out.simpleCombatOneshotFoxFillGapMs = ClampCombatOneshotFoxFillGapMs(u);
    }
    if (IniGetU32(ini, "core", "pumpCongestionThreshold", u))
        out.pumpCongestionThreshold = ClampPumpCongestion(u);
    if (IniGetU32(ini, "core", "pumpDrainBudget", u))
        out.pumpDrainBudget = ClampPumpDrainBudget(u);
    else
        out.pumpDrainBudget = kPumpDrainBudgetDefault;
    if (IniGetU32(ini, "core", "charSlot", u) && u >= 1 && u <= 32) out.charSlot = u;
    if (IniGetU32(ini, "core", "hpThresholdPct", u) && u >= 1 && u <= 99) out.hpThresholdPct = u;
    if (IniGetU32(ini, "core", "mpThresholdPct", u) && u >= 1 && u <= 99) out.mpThresholdPct = u;
    int32_t wid = 0;
    // worldId stored as u32 in ini; cast back.
    if (IniGetU32(ini, "core", "worldId", u)) {
        wid = static_cast<int32_t>(u);
        out.worldId = wid;
    }
    std::string name;
    if (IniGetString(ini, "core", "worldName", name)) CopyWorldName(out.worldName, sizeof(out.worldName), name.c_str());
    IniGetU64(ini, "core", "writeTickMs", out.writeTickMs);
    out.magic = kPayloadControlMagic;
    out.version = kPayloadControlVersion;
    return true;
}

bool WritePayloadControl(const char* binDir, const PayloadControl& control) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsurePayloadStateDir(binDir)) return false;

    PayloadControl normalized = control;
    normalized.magic = kPayloadControlMagic;
    normalized.version = kPayloadControlVersion;
    normalized.invuln = normalized.invuln ? 1u : 0u;
    normalized.attackAccel =
        (kAttackAccelUserEnabled && normalized.attackAccel) ? 1u : 0u;
    normalized.attackAccelClearBusy = normalized.attackAccelClearBusy ? 1u : 0u;
    normalized.attackAccelClearBusyMinIntervalMs = ClampAttackAccelClearBusyMinIntervalMs(
        normalized.attackAccelClearBusyMinIntervalMs
            ? normalized.attackAccelClearBusyMinIntervalMs
            : kAttackAccelClearBusyMinIntervalDefaultMs);
    normalized.attackAccelCutLayer = normalized.attackAccelCutLayer ? 1u : 0u;
    normalized.attackAccelSkipPrepare = normalized.attackAccelSkipPrepare ? 1u : 0u;
    normalized.attackAccelBooster =
        (kAttackAccelBoosterUserEnabled && normalized.attackAccelBooster) ? 1u : 0u;
    normalized.attackAccelActionSpeed = normalized.attackAccelActionSpeed ? 1u : 0u;
    normalized.attackAccelPartyBooster = normalized.attackAccelPartyBooster ? 1u : 0u;
    normalized.attackAccelPartyBoosterValue =
        ClampAttackAccelPartyBoosterValue(normalized.attackAccelPartyBoosterValue);
    normalized.attackAccelBreakDegreeFloor =
        normalized.attackAccelBreakDegreeFloor ? 1u : 0u;
    normalized.attackAccelBreakDegreeFloorLo =
        ClampAttackAccelBreakDegreeFloorLo(normalized.attackAccelBreakDegreeFloorLo);
    normalized.finalAttackForce =
        (kFinalAttackForceUserEnabled && normalized.finalAttackForce) ? 1u : 0u;
    normalized.skillMaxLevel =
        (kSkillMaxLevelUserEnabled && normalized.skillMaxLevel) ? 1u : 0u;
    normalized.attackSameFrameBurst = kAttackSameFrameBurstDefault;
    normalized.fly = normalized.fly ? 1u : 0u;
    normalized.flyMode = ClampFlyMode(normalized.flyMode);
    normalized.flyHopCdMs = ClampFlyHopCdMs(
        normalized.flyHopCdMs ? normalized.flyHopCdMs : kFlyHopCdDefaultMs);
    // 0 = 旧档没有这个字段：回默认，而不是被 Clamp 抬到 Min（那会静默改掉用户没设过的值）。
    normalized.flySpeedPct =
        ClampHeliSpeedPct(normalized.flySpeedPct ? normalized.flySpeedPct : kFlySpeedPctDefault);
    normalized.autoEnter = normalized.autoEnter ? 1u : 0u;
    normalized.hpPotion = normalized.hpPotion ? 1u : 0u;
    normalized.mpPotion = normalized.mpPotion ? 1u : 0u;
    normalized.petSummon = normalized.petSummon ? 1u : 0u;
    normalized.petSummonRequireFood = normalized.petSummonRequireFood ? 1u : 0u;
    normalized.multiSkill = normalized.multiSkill ? 1u : 0u;
    normalized.multiSkillSafeStagger = normalized.multiSkillSafeStagger ? 1u : 0u;
    normalized.multiSkillSendUseRequest = normalized.multiSkillSendUseRequest ? 1u : 0u;
    normalized.multiSkillGapMs = ClampMultiSkillGapMs(normalized.multiSkillGapMs);
    normalized.simpleCombat = normalized.simpleCombat ? 1u : 0u;
    normalized.simpleCombatSmartInterval = normalized.simpleCombatSmartInterval ? 1u : 0u;
    normalized.simpleCombatAttackIntervalMs = EffectiveSimpleCombatAttackIntervalMs(
        normalized.simpleCombatAttackIntervalMs
            ? ((normalized.simpleCombatAttackIntervalMs ==
                    kSimpleCombatAttackIntervalLegacyDefaultMs ||
                normalized.simpleCombatAttackIntervalMs ==
                    kSimpleCombatAttackIntervalLegacyDefaultMs46)
                   ? kSimpleCombatAttackIntervalDefaultMs
                   : normalized.simpleCombatAttackIntervalMs)
            : kSimpleCombatAttackIntervalDefaultMs,
        normalized.attackAccel);
    normalized.simpleCombatTickMs = ClampSimpleCombatTickMs(
        normalized.simpleCombatTickMs ? normalized.simpleCombatTickMs
                                      : kSimpleCombatTickDefaultMs);
    normalized.mobScanIntervalMs = ClampMobScanIntervalMs(
        !normalized.mobScanIntervalMs
            ? kMobScanIntervalDefaultMs
            : (normalized.mobScanIntervalMs == kMobScanIntervalLegacyDefaultMs
                   ? kMobScanIntervalDefaultMs
                   : normalized.mobScanIntervalMs));
    normalized.simpleCombatAttackHoldMs = ClampAttackHoldMs(
        normalized.simpleCombatAttackHoldMs ? normalized.simpleCombatAttackHoldMs
                                            : kAttackHoldDefaultMs);
    normalized.clusterWeight = normalized.clusterWeight ? 1u : 0u;
    normalized.simpleCombatTeleport = 0u;
    normalized.simpleCombatImpactApproach = normalized.simpleCombatImpactApproach ? 1u : 0u;
    // 0 视为「旧盘没有这个键」，回默认 100 而非被 Clamp 抬到下限 25——
    // 后者会让老配置一升级就悄悄变成 0.25X。
    normalized.simpleCombatFlySpeedPct = ClampHeliSpeedPct(
        normalized.simpleCombatFlySpeedPct ? normalized.simpleCombatFlySpeedPct
                                           : kHeliSpeedPctDefault);
    normalized.simpleCombatHumanWalk = normalized.simpleCombatHumanWalk ? 1u : 0u;
    // 追怪位移单选：空中贴怪开则压掉拟人（旧盘两者同开时等价于仅空中）。
    if (normalized.simpleCombatImpactApproach) normalized.simpleCombatHumanWalk = 0u;
    normalized.simpleCombatLiveStep = normalized.simpleCombatLiveStep ? 1u : 0u;
    normalized.attackRpc = normalized.attackRpc ? 1u : 0u;
    normalized.attackRpcMobs = ClampAttackRpcMobs(
        normalized.attackRpcMobs ? normalized.attackRpcMobs : kAttackRpcMobsDefault);
    normalized.attackRpcIntervalMs = ClampAttackRpcIntervalMs(
        normalized.attackRpcIntervalMs ? normalized.attackRpcIntervalMs
                                       : kAttackRpcIntervalDefaultMs);
    normalized.attackRpcDamage = ClampAttackRpcDamage(
        normalized.attackRpcDamage ? normalized.attackRpcDamage : kAttackRpcDamageDefault);
    normalized.curFhGateBypass = normalized.curFhGateBypass ? 1u : 0u;
    normalized.autoLie = normalized.autoLie ? 1u : 0u;
    normalized.autoLieDryRun = normalized.autoLieDryRun ? 1u : 0u;
    normalized.autoLieMouseRegionOverlay = normalized.autoLieMouseRegionOverlay ? 1u : 0u;
    normalized.dropAlertBypass = normalized.dropAlertBypass ? 1u : 0u;
    normalized.auctionTownBypass = normalized.auctionTownBypass ? 1u : 0u;
    normalized.restMpAccel = normalized.restMpAccel ? 1u : 0u;
    normalized.restMpAccelIntervalMs = ClampRestMpAccelIntervalMs(
        normalized.restMpAccelIntervalMs ? normalized.restMpAccelIntervalMs
                                         : kRestMpAccelIntervalDefaultMs);
    normalized.infiniteStars =
        (kInfiniteStarsUserEnabled && normalized.infiniteStars) ? 1u : 0u;
    normalized.autoSell = normalized.autoSell ? 1u : 0u;
    normalized.launcherHangupSchedule = normalized.launcherHangupSchedule ? 1u : 0u;
    normalized.launcherHangupScheduleMask =
        ClampHangupScheduleMask(normalized.launcherHangupScheduleMask);
    normalized.launcherWatchdog = normalized.launcherWatchdog ? 1u : 0u;
    normalized.launcherWatchdogNoExpSec =
        ClampWatchdogNoExpSec(normalized.launcherWatchdogNoExpSec
                                  ? normalized.launcherWatchdogNoExpSec
                                  : kWatchdogNoExpSecDefault);
    normalized.launcherWatchdogCooldownSec =
        ClampWatchdogCooldownSec(normalized.launcherWatchdogCooldownSec
                                    ? normalized.launcherWatchdogCooldownSec
                                    : kWatchdogCooldownSecDefault);
    normalized.simpleCombatTeleportMinDx =
        ClampCombatTeleportMinDx(normalized.simpleCombatTeleportMinDx
                                     ? normalized.simpleCombatTeleportMinDx
                                     : kCombatTeleportMinDxDefault);
    {
        uint32_t off = normalized.simpleCombatTeleportStandOff;
        if (!off || off == kCombatTeleportStandOffLegacyDefault)
            off = kCombatTeleportStandOffDefault;
        normalized.simpleCombatTeleportStandOff = ClampCombatTeleportStandOff(off);
    }
    normalized.simpleCombatStandOffCustom = normalized.simpleCombatStandOffCustom ? 1u : 0u;
    normalized.simpleCombatStandOffX = ClampCombatStandOffX(normalized.simpleCombatStandOffX);
    normalized.simpleCombatStandOffY = ClampCombatStandOffY(normalized.simpleCombatStandOffY);
    normalized.simpleCombatGroundSpoof = normalized.simpleCombatGroundSpoof ? 1u : 0u;
    normalized.simpleCombatAntiJitter = normalized.simpleCombatAntiJitter ? 1u : 0u;
    normalized.simpleCombatAntiHug = normalized.simpleCombatAntiHug ? 1u : 0u;
    normalized.meleeVeto = normalized.meleeVeto ? 1u : 0u;
    normalized.simpleCombatTeleportCooldownMs =
        ClampCombatTeleportCooldownMs(normalized.simpleCombatTeleportCooldownMs
                                         ? normalized.simpleCombatTeleportCooldownMs
                                         : kCombatTeleportCooldownDefaultMs);
    // 0 合法（关门控）；勿把 0 当成缺省。
    // 旧默认 400/520：抬到 550（=上限）。显式调过其它值保留。
    {
        uint32_t hop = normalized.simpleCombatTeleportMaxHop;
        if (!hop || hop == kCombatTeleportMaxHopLegacyDefault ||
            hop == kCombatTeleportMaxHopPrevDefault)
            hop = kCombatTeleportMaxHopDefault;
        normalized.simpleCombatTeleportMaxHop = ClampCombatTeleportMaxHop(hop);
    }
    // oneshotMaxHp 允许 0（关秒杀道）；缺省用默认，勿把 0 当成缺 key。
    normalized.simpleCombatOneshotMaxHp =
        ClampCombatOneshotMaxHp(normalized.simpleCombatOneshotMaxHp);
    // bumps=0 合法（射后不管）；勿把 0 当成缺 key。
    normalized.simpleCombatOneshotMinBumps =
        ClampCombatOneshotMinBumps(normalized.simpleCombatOneshotMinBumps);
    normalized.simpleCombatOneshotMinFires = ClampCombatOneshotMinFires(
        normalized.simpleCombatOneshotMinFires ? normalized.simpleCombatOneshotMinFires
                                              : kCombatOneshotMinFiresDefault);
    normalized.simpleCombatOneshotMinLagMs =
        ClampCombatOneshotMinLagMs(normalized.simpleCombatOneshotMinLagMs);
    normalized.simpleCombatOneshotFoxFillGapMs =
        ClampCombatOneshotFoxFillGapMs(normalized.simpleCombatOneshotFoxFillGapMs);
    normalized.pumpCongestionThreshold = ClampPumpCongestion(normalized.pumpCongestionThreshold);
    normalized.pumpDrainBudget = ClampPumpDrainBudget(
        normalized.pumpDrainBudget ? normalized.pumpDrainBudget : kPumpDrainBudgetDefault);
    normalized.frameLock = normalized.frameLock ? 1u : 0u;
    normalized.frameLockFps = ClampFrameLockFps(
        normalized.frameLockFps ? normalized.frameLockFps : kFrameLockFpsDefault);
    if (normalized.charSlot < 1) normalized.charSlot = 1;
    if (normalized.charSlot > 32) normalized.charSlot = 32;
    if (normalized.hpThresholdPct < 1) normalized.hpThresholdPct = 1;
    if (normalized.hpThresholdPct > 99) normalized.hpThresholdPct = 99;
    if (normalized.mpThresholdPct < 1) normalized.mpThresholdPct = 1;
    if (normalized.mpThresholdPct > 99) normalized.mpThresholdPct = 99;
    // 同毫秒连续写会让 payload 因 writeTickMs 未变而跳过 Apply；强制单调递增。
    // （与 WriteAutoSupply / WriteSellbag 同款；换频/impact 等 bump seq 依赖此字段变更。）
    static uint64_t s_lastTick = 0;
    uint64_t tick = normalized.writeTickMs ? normalized.writeTickMs : NowTickMs();
    if (tick <= s_lastTick) tick = s_lastTick + 1;
    s_lastTick = tick;
    normalized.writeTickMs = tick;
    normalized.worldName[sizeof(normalized.worldName) - 1] = 0;
    normalized.impactHopDeltaX = ClampImpactHopDeltaX(normalized.impactHopDeltaX);
    normalized.impactHopForce = normalized.impactHopForce ? 1u : 0u;

    const std::string path = UserConfigIniPath(binDir);
    const bool ok = UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "core", "version", kPayloadControlCoreIniVersion);
        IniSetBool(ini, "core", "invuln", normalized.invuln != 0);
        IniSetBool(ini, "core", "attackAccel", normalized.attackAccel != 0);
        IniSetBool(ini, "core", "attackAccelClearBusy",
                   normalized.attackAccelClearBusy != 0);
        IniSetU32(ini, "core", "attackAccelClearBusyMinIntervalMs",
                  normalized.attackAccelClearBusyMinIntervalMs);
        IniSetBool(ini, "core", "attackAccelCutLayer", normalized.attackAccelCutLayer != 0);
        IniSetBool(ini, "core", "attackAccelSkipPrepare",
                   normalized.attackAccelSkipPrepare != 0);
        IniSetBool(ini, "core", "attackAccelBooster", normalized.attackAccelBooster != 0);
        IniSetBool(ini, "core", "attackAccelActionSpeed",
                   normalized.attackAccelActionSpeed != 0);
        IniSetBool(ini, "core", "attackAccelPartyBooster",
                   normalized.attackAccelPartyBooster != 0);
        IniSetI32(ini, "core", "attackAccelPartyBoosterValue",
                  normalized.attackAccelPartyBoosterValue);
        IniSetBool(ini, "core", "attackAccelBreakDegreeFloor",
                   normalized.attackAccelBreakDegreeFloor != 0);
        IniSetI32(ini, "core", "attackAccelBreakDegreeFloorLo",
                  normalized.attackAccelBreakDegreeFloorLo);
        IniSetBool(ini, "core", "finalAttackForce", normalized.finalAttackForce != 0);
        IniSetBool(ini, "core", "skillMaxLevel", normalized.skillMaxLevel != 0);
        IniSetU32(ini, "core", "attackSameFrameBurst", normalized.attackSameFrameBurst);
        // 策略可持久化；武装开关永不落盘（强制 false，清历史 key）。
        IniSetBool(ini, "core", "fly", false);
        IniSetU32(ini, "core", "flyMode", normalized.flyMode);
        IniSetU32(ini, "core", "flyHopCdMs", normalized.flyHopCdMs);
        IniSetU32(ini, "core", "flySpeedPct", normalized.flySpeedPct);
        IniSetBool(ini, "core", "autoEnter", normalized.autoEnter != 0);
        IniSetBool(ini, "core", "hpPotion", normalized.hpPotion != 0);
        IniSetBool(ini, "core", "mpPotion", normalized.mpPotion != 0);
        IniSetBool(ini, "core", "petSummon", normalized.petSummon != 0);
        IniSetBool(ini, "core", "petSummonRequireFood", normalized.petSummonRequireFood != 0);
        IniSetBool(ini, "core", "multiSkill", normalized.multiSkill != 0);
        IniSetU32(ini, "core", "multiSkillGapMs", normalized.multiSkillGapMs);
        IniSetBool(ini, "core", "multiSkillSafeStagger", normalized.multiSkillSafeStagger != 0);
        IniSetBool(ini, "core", "multiSkillSendUseRequest",
                   normalized.multiSkillSendUseRequest != 0);
        IniSetBool(ini, "core", "simpleCombat", normalized.simpleCombat != 0);
        IniSetBool(ini, "core", "simpleCombatSmartInterval",
                   normalized.simpleCombatSmartInterval != 0);
        IniSetU32(ini, "core", "simpleCombatAttackIntervalMs",
                  normalized.simpleCombatAttackIntervalMs);
        IniSetU32(ini, "core", "simpleCombatTickMs", normalized.simpleCombatTickMs);
        IniSetU32(ini, "core", "mobScanIntervalMs", normalized.mobScanIntervalMs);
        IniSetU32(ini, "core", "simpleCombatAttackHoldMs", normalized.simpleCombatAttackHoldMs);
        IniSetU32(ini, "core", "clusterWeight", normalized.clusterWeight);
        IniSetBool(ini, "core", "simpleCombatTeleport", normalized.simpleCombatTeleport != 0);
        // 落盘中性键；擦掉旧 Impact* 键名。
        IniSetBool(ini, "core", "simpleCombatAirApproach",
                   normalized.simpleCombatImpactApproach != 0);
        IniEraseKey(ini, "core", "simpleCombatImpactApproach");
        IniSetU32(ini, "core", "simpleCombatFlySpeedPct", normalized.simpleCombatFlySpeedPct);
        IniSetBool(ini, "core", "simpleCombatHumanWalk", normalized.simpleCombatHumanWalk != 0);
        IniSetBool(ini, "core", "simpleCombatLiveStep", normalized.simpleCombatLiveStep != 0);
        IniSetBool(ini, "core", "attackRpc", normalized.attackRpc != 0);
        IniSetU32(ini, "core", "attackRpcMobs", normalized.attackRpcMobs);
        IniSetU32(ini, "core", "attackRpcIntervalMs", normalized.attackRpcIntervalMs);
        IniSetU32(ini, "core", "attackRpcDamage", normalized.attackRpcDamage);
        IniSetBool(ini, "core", "curFhGateBypass", normalized.curFhGateBypass != 0);
        IniSetBool(ini, "core", "autoLie", normalized.autoLie != 0);
        IniSetBool(ini, "core", "autoLieDryRun", normalized.autoLieDryRun != 0);
        IniSetBool(ini, "core", "autoLieMouseRegionOverlay",
                   normalized.autoLieMouseRegionOverlay != 0);
        IniSetBool(ini, "core", "movepathFlushProbe", normalized.movepathFlushProbe != 0);
        IniSetBool(ini, "core", "galaxyTokenProbe", normalized.galaxyTokenProbe != 0);
        IniSetBool(ini, "core", "softLoginProbe", normalized.softLoginProbe != 0);
        IniSetU32(ini, "core", "softLoginDismissSeq", normalized.softLoginDismissSeq);
        IniSetU32(ini, "core", "autoLieAlarmTestSeq", normalized.autoLieAlarmTestSeq);
        IniSetU32(ini, "core", "autoLieMouseSmokeSeq", normalized.autoLieMouseSmokeSeq);
        IniSetU32(ini, "core", "autoLieMouseSimSeq", normalized.autoLieMouseSimSeq);
        IniSetU32(ini, "core", "manualRejoinSeq", normalized.manualRejoinSeq);
        IniSetU32(ini, "core", "teleportTestSeq", normalized.teleportTestSeq);
        IniSetU32(ini, "core", "teleportNativeTestSeq", normalized.teleportNativeTestSeq);
        IniSetU32(ini, "core", "teleportKickStressSeq", normalized.teleportKickStressSeq);
        IniSetU32(ini, "core", "teleportKickStressFineSeq", normalized.teleportKickStressFineSeq);
        IniSetU32(ini, "core", "teleportKickStressFine10Seq",
                  normalized.teleportKickStressFine10Seq);
        IniSetU32(ini, "core", "teleportKickStressLocalSeq",
                  normalized.teleportKickStressLocalSeq);
        IniSetU32(ini, "core", "moveProbeASeq", normalized.impactNockBackTestSeq);
        IniSetU32(ini, "core", "moveProbeBSeq", normalized.impactSetNextTestSeq);
        IniSetU32(ini, "core", "moveProbeDir",
                  static_cast<uint32_t>(ClampImpactImpulseDir(normalized.impactImpulseDir)));
        IniSetU32(ini, "core", "moveProbeVx",
                  ClampImpactImpulseSpeed(normalized.impactImpulseVx));
        IniSetU32(ini, "core", "moveProbeVy",
                  ClampImpactImpulseSpeed(normalized.impactImpulseVy));
        IniSetU32(ini, "core", "moveHopTestSeq", normalized.impactHopTestSeq);
        {
            char dxBuf[16]{};
            snprintf(dxBuf, sizeof(dxBuf), "%d",
                     (int)ClampImpactHopDeltaX(normalized.impactHopDeltaX));
            IniSetString(ini, "core", "moveHopDeltaX", dxBuf);
        }
        IniSetBool(ini, "core", "moveHopForce", normalized.impactHopForce != 0);
        IniEraseKey(ini, "core", "impactNockBackTestSeq");
        IniEraseKey(ini, "core", "impactSetNextTestSeq");
        IniEraseKey(ini, "core", "impactImpulseDir");
        IniEraseKey(ini, "core", "impactImpulseVx");
        IniEraseKey(ini, "core", "impactImpulseVy");
        IniEraseKey(ini, "core", "impactHopTestSeq");
        IniEraseKey(ini, "core", "impactHopDeltaX");
        IniEraseKey(ini, "core", "impactHopForce");
        IniSetBool(ini, "core", "autoRelogin", normalized.autoRelogin != 0);
        IniSetBool(ini, "core", "autoReloginStopCombat",
                   normalized.autoReloginStopCombat != 0);
        IniSetBool(ini, "core", "autoReloginReconnect",
                   normalized.autoReloginReconnect != 0);
        IniSetBool(ini, "core", "autoReloginGmEscalate",
                   normalized.autoReloginGmEscalate != 0);
        IniSetBool(ini, "core", "hideOtherPlayers", normalized.hideOtherPlayers != 0);
        IniSetBool(ini, "core", "frameLock", normalized.frameLock != 0);
        IniSetU32(ini, "core", "frameLockFps", ClampFrameLockFps(normalized.frameLockFps));
        IniSetBool(ini, "core", "dropAlertBypass", normalized.dropAlertBypass != 0);
        // 「不挥弓」已拆除（v70）：清掉历史 key，免得旧 ini 一直留着个没人读的开关。
        IniEraseKey(ini, "core", "pointBlankShoot");
        IniSetBool(ini, "core", "auctionTownBypass", normalized.auctionTownBypass != 0);
        IniSetBool(ini, "core", "restMpAccel", normalized.restMpAccel != 0);
        IniSetU32(ini, "core", "restMpAccelIntervalMs",
                  ClampRestMpAccelIntervalMs(normalized.restMpAccelIntervalMs
                                                 ? normalized.restMpAccelIntervalMs
                                                 : kRestMpAccelIntervalDefaultMs));
        IniSetBool(ini, "core", "infiniteStars", normalized.infiniteStars != 0);
        // 剥离双轨：不再写 core.autoSell*，并清掉历史 key。
        IniEraseKeysWithPrefix(ini, "core", "autoSell");
        IniSetBool(ini, "core", "launcherHangupSchedule",
                   normalized.launcherHangupSchedule != 0);
        IniSetU32(ini, "core", "launcherHangupScheduleMask",
                  normalized.launcherHangupScheduleMask);
        IniSetBool(ini, "core", "launcherWatchdog", normalized.launcherWatchdog != 0);
        IniSetU32(ini, "core", "launcherWatchdogNoExpSec",
                  normalized.launcherWatchdogNoExpSec);
        IniSetU32(ini, "core", "launcherWatchdogCooldownSec",
                  normalized.launcherWatchdogCooldownSec);
        IniSetU32(ini, "core", "simpleCombatTeleportMinDx",
                  normalized.simpleCombatTeleportMinDx);
        IniSetU32(ini, "core", "simpleCombatTeleportStandOff",
                  normalized.simpleCombatTeleportStandOff);
        IniSetBool(ini, "core", "simpleCombatStandOffCustom",
                   normalized.simpleCombatStandOffCustom != 0);
        IniSetU32(ini, "core", "simpleCombatStandOffX", normalized.simpleCombatStandOffX);
        IniSetI32(ini, "core", "simpleCombatStandOffY", normalized.simpleCombatStandOffY);
        IniSetBool(ini, "core", "simpleCombatGroundSpoof",
                   normalized.simpleCombatGroundSpoof != 0);
        IniSetBool(ini, "core", "simpleCombatAntiJitter",
                   normalized.simpleCombatAntiJitter != 0);
        IniSetBool(ini, "core", "simpleCombatAntiHug", normalized.simpleCombatAntiHug != 0);
        IniSetBool(ini, "core", "meleeVeto", normalized.meleeVeto != 0);
        IniSetU32(ini, "core", "simpleCombatTeleportCooldownMs",
                  normalized.simpleCombatTeleportCooldownMs);
        // fill+Doing 已废：清掉历史跨层门控 / 位移预算 key。
        IniEraseKey(ini, "core", "simpleCombatCrossLayerFillGateMs");
        IniEraseKey(ini, "core", "simpleCombatFillBudgetPx");
        IniSetU32(ini, "core", "simpleCombatTeleportMaxHop",
                  normalized.simpleCombatTeleportMaxHop);
        IniSetU32(ini, "core", "simpleCombatOneshotMaxHp",
                  normalized.simpleCombatOneshotMaxHp);
        IniSetU32(ini, "core", "simpleCombatOneshotMinBumps",
                  normalized.simpleCombatOneshotMinBumps);
        IniSetU32(ini, "core", "simpleCombatOneshotMinFires",
                  normalized.simpleCombatOneshotMinFires);
        IniSetU32(ini, "core", "simpleCombatOneshotMinLagMs",
                  normalized.simpleCombatOneshotMinLagMs);
        IniSetU32(ini, "core", "simpleCombatOneshotFoxFillGapMs",
                  normalized.simpleCombatOneshotFoxFillGapMs);
        IniSetU32(ini, "core", "pumpCongestionThreshold", normalized.pumpCongestionThreshold);
        IniSetU32(ini, "core", "pumpDrainBudget",
                  ClampPumpDrainBudget(normalized.pumpDrainBudget));
        IniSetU32(ini, "core", "hpThresholdPct", normalized.hpThresholdPct);
        IniSetU32(ini, "core", "mpThresholdPct", normalized.mpThresholdPct);
        IniSetU32(ini, "core", "charSlot", normalized.charSlot);
        IniSetU32(ini, "core", "worldId", static_cast<uint32_t>(normalized.worldId));
        IniSetString(ini, "core", "worldName", normalized.worldName);
        IniSetU64(ini, "core", "writeTickMs", normalized.writeTickMs);
        IniSetBool(ini, "core", "autopot",
                   normalized.hpPotion != 0 || normalized.mpPotion != 0);
    });
    // 武装开关只写会话文件，供面板↔DLL 热切换；重启后由 ClearFlyArmedSession 清零。
    if (ok) (void)WriteFlyArmedSession(binDir, normalized.fly != 0);
    return ok;
}

}  // namespace xcat
