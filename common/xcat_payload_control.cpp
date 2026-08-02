#include "xcat_payload_control.h"

#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

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
    out.fly = 0;
    out.flyMode = kFlyModeDefault;
    out.flyHopCdMs = kFlyHopCdDefaultMs;
    out.autoEnter = 0;
    out.charSlot = 1;
    out.worldId = kDefaultWorldId;
    CopyWorldName(out.worldName, sizeof(out.worldName), kDefaultWorldName);
    out.hpPotion = 1;
    out.mpPotion = 1;
    out.hpThresholdPct = 50;
    out.mpThresholdPct = 30;
    out.petSummon = 1;
    out.petSummonRequireFood = 1;
    out.multiSkill = 0;
    out.multiSkillGapMs = kMultiSkillGapDefaultMs;
    out.multiSkillSafeStagger = 1;
    out.simpleCombat = 0;
    out.simpleCombatSmartInterval = 0;
    out.simpleCombatAttackIntervalMs = kSimpleCombatAttackIntervalDefaultMs;
    out.simpleCombatTickMs = kSimpleCombatTickDefaultMs;
    out.clusterWeight = kClusterWeightDefault;
    out.simpleCombatTeleport = 1;
    out.simpleCombatTeleportMinDx = kCombatTeleportMinDxDefault;
    out.simpleCombatTeleportStandOff = kCombatTeleportStandOffDefault;
    out.simpleCombatTeleportCooldownMs = kCombatTeleportCooldownDefaultMs;
    out.simpleCombatLiveStep = 0;
    out.attackRpc = 0;
    out.attackRpcMobs = kAttackRpcMobsDefault;
    out.attackRpcIntervalMs = kAttackRpcIntervalDefaultMs;
    out.attackRpcDamage = kAttackRpcDamageDefault;
    out.autoLie = 0;
    out.autoLieDryRun = 0;
    out.autoLieAlarmTestSeq = 0;
    out.autoLieMouseSmokeSeq = 0;
    out.manualRejoinSeq = 0;
    out.teleportTestSeq = 0;
    out.teleportNativeTestSeq = 0;
    out.teleportKickStressSeq = 0;
    out.teleportKickStressFineSeq = 0;
    out.teleportKickStressFine10Seq = 0;
    out.teleportKickStressLocalSeq = 0;
    out.autoRelogin = 0;
    out.autoReloginStopCombat = 1;
    out.autoReloginReconnect = 1;
    out.dropAlertBypass = 1;
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
    // v30 兼容：旧「跳过动作等待」开着则视为攻击加速开
    if (IniGetBool(ini, "core", "attackAccelClearBusy", b) && b) out.attackAccel = 1u;
    // fly 开关不读 ini（历史 key 忽略）；只读会话态 state/fly_armed。
    out.fly = ReadFlyArmedSession(binDir) ? 1u : 0u;
    if (IniGetU32(ini, "core", "flyMode", u)) out.flyMode = ClampFlyMode(u);
    if (IniGetU32(ini, "core", "flyHopCdMs", u)) out.flyHopCdMs = ClampFlyHopCdMs(u);
    if (IniGetBool(ini, "core", "autoEnter", b)) out.autoEnter = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "hpPotion", b)) out.hpPotion = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "mpPotion", b)) out.mpPotion = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "petSummon", b)) out.petSummon = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "petSummonRequireFood", b)) out.petSummonRequireFood = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "multiSkill", b)) out.multiSkill = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "multiSkillSafeStagger", b))
        out.multiSkillSafeStagger = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "simpleCombat", b)) out.simpleCombat = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "simpleCombatSmartInterval", b))
        out.simpleCombatSmartInterval = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "multiSkillGapMs", u))
        out.multiSkillGapMs = ClampMultiSkillGapMs(u);
    if (IniGetU32(ini, "core", "simpleCombatAttackIntervalMs", u))
        out.simpleCombatAttackIntervalMs = ClampSimpleCombatAttackIntervalMs(u);
    out.simpleCombatAttackIntervalMs = EffectiveSimpleCombatAttackIntervalMs(
        out.simpleCombatAttackIntervalMs, out.attackAccel);
    if (IniGetU32(ini, "core", "simpleCombatTickMs", u))
        out.simpleCombatTickMs = ClampSimpleCombatTickMs(u);
    else
        out.simpleCombatTickMs = kSimpleCombatTickDefaultMs;
    out.simpleCombatTickMs = ClampSimpleCombatTickMs(out.simpleCombatTickMs);
    if (IniGetU32(ini, "core", "clusterWeight", u)) out.clusterWeight = ClampClusterWeight(u);
    // 面板已撤开关：读盘亦强制开，避免旧 ini=0 让 DLL 站桩。
    out.simpleCombatTeleport = 1u;
    if (IniGetBool(ini, "core", "simpleCombatLiveStep", b))
        out.simpleCombatLiveStep = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "attackRpc", b)) out.attackRpc = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "attackRpcMobs", u)) out.attackRpcMobs = ClampAttackRpcMobs(u);
    if (IniGetU32(ini, "core", "attackRpcIntervalMs", u))
        out.attackRpcIntervalMs = ClampAttackRpcIntervalMs(u);
    if (IniGetU32(ini, "core", "attackRpcDamage", u))
        out.attackRpcDamage = ClampAttackRpcDamage(u);
    if (IniGetBool(ini, "core", "autoLie", b)) out.autoLie = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoLieDryRun", b)) out.autoLieDryRun = b ? 1u : 0u;
    if (IniGetU32(ini, "core", "autoLieAlarmTestSeq", u)) out.autoLieAlarmTestSeq = u;
    if (IniGetU32(ini, "core", "autoLieMouseSmokeSeq", u)) out.autoLieMouseSmokeSeq = u;
    if (IniGetU32(ini, "core", "manualRejoinSeq", u)) out.manualRejoinSeq = u;
    if (IniGetU32(ini, "core", "teleportTestSeq", u)) out.teleportTestSeq = u;
    if (IniGetU32(ini, "core", "teleportNativeTestSeq", u)) out.teleportNativeTestSeq = u;
    if (IniGetU32(ini, "core", "teleportKickStressSeq", u)) out.teleportKickStressSeq = u;
    if (IniGetU32(ini, "core", "teleportKickStressFineSeq", u)) out.teleportKickStressFineSeq = u;
    if (IniGetU32(ini, "core", "teleportKickStressFine10Seq", u))
        out.teleportKickStressFine10Seq = u;
    if (IniGetU32(ini, "core", "teleportKickStressLocalSeq", u))
        out.teleportKickStressLocalSeq = u;
    if (IniGetBool(ini, "core", "autoRelogin", b)) out.autoRelogin = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoReloginStopCombat", b))
        out.autoReloginStopCombat = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoReloginReconnect", b))
        out.autoReloginReconnect = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "dropAlertBypass", b)) out.dropAlertBypass = b ? 1u : 0u;
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
    if (IniGetU32(ini, "core", "simpleCombatTeleportStandOff", u))
        out.simpleCombatTeleportStandOff = ClampCombatTeleportStandOff(u);
    if (IniGetU32(ini, "core", "simpleCombatTeleportCooldownMs", u))
        out.simpleCombatTeleportCooldownMs = ClampCombatTeleportCooldownMs(u);
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
    normalized.attackAccel = normalized.attackAccel ? 1u : 0u;
    normalized.fly = normalized.fly ? 1u : 0u;
    normalized.flyMode = ClampFlyMode(normalized.flyMode);
    normalized.flyHopCdMs = ClampFlyHopCdMs(
        normalized.flyHopCdMs ? normalized.flyHopCdMs : kFlyHopCdDefaultMs);
    normalized.autoEnter = normalized.autoEnter ? 1u : 0u;
    normalized.hpPotion = normalized.hpPotion ? 1u : 0u;
    normalized.mpPotion = normalized.mpPotion ? 1u : 0u;
    normalized.petSummon = normalized.petSummon ? 1u : 0u;
    normalized.petSummonRequireFood = normalized.petSummonRequireFood ? 1u : 0u;
    normalized.multiSkill = normalized.multiSkill ? 1u : 0u;
    normalized.multiSkillSafeStagger = normalized.multiSkillSafeStagger ? 1u : 0u;
    normalized.multiSkillGapMs = ClampMultiSkillGapMs(normalized.multiSkillGapMs);
    normalized.simpleCombat = normalized.simpleCombat ? 1u : 0u;
    normalized.simpleCombatSmartInterval = normalized.simpleCombatSmartInterval ? 1u : 0u;
    normalized.simpleCombatAttackIntervalMs = EffectiveSimpleCombatAttackIntervalMs(
        normalized.simpleCombatAttackIntervalMs, normalized.attackAccel);
    normalized.simpleCombatTickMs = ClampSimpleCombatTickMs(
        normalized.simpleCombatTickMs ? normalized.simpleCombatTickMs
                                      : kSimpleCombatTickDefaultMs);
    normalized.clusterWeight = normalized.clusterWeight ? 1u : 0u;
    normalized.simpleCombatTeleport = 1u;
    normalized.simpleCombatLiveStep = normalized.simpleCombatLiveStep ? 1u : 0u;
    normalized.attackRpc = normalized.attackRpc ? 1u : 0u;
    normalized.attackRpcMobs = ClampAttackRpcMobs(
        normalized.attackRpcMobs ? normalized.attackRpcMobs : kAttackRpcMobsDefault);
    normalized.attackRpcIntervalMs = ClampAttackRpcIntervalMs(
        normalized.attackRpcIntervalMs ? normalized.attackRpcIntervalMs
                                       : kAttackRpcIntervalDefaultMs);
    normalized.attackRpcDamage = ClampAttackRpcDamage(
        normalized.attackRpcDamage ? normalized.attackRpcDamage : kAttackRpcDamageDefault);
    normalized.autoLie = normalized.autoLie ? 1u : 0u;
    normalized.autoLieDryRun = normalized.autoLieDryRun ? 1u : 0u;
    normalized.dropAlertBypass = normalized.dropAlertBypass ? 1u : 0u;
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
    normalized.simpleCombatTeleportStandOff =
        ClampCombatTeleportStandOff(normalized.simpleCombatTeleportStandOff
                                        ? normalized.simpleCombatTeleportStandOff
                                        : kCombatTeleportStandOffDefault);
    normalized.simpleCombatTeleportCooldownMs =
        ClampCombatTeleportCooldownMs(normalized.simpleCombatTeleportCooldownMs
                                         ? normalized.simpleCombatTeleportCooldownMs
                                         : kCombatTeleportCooldownDefaultMs);
    if (normalized.charSlot < 1) normalized.charSlot = 1;
    if (normalized.charSlot > 32) normalized.charSlot = 32;
    if (normalized.hpThresholdPct < 1) normalized.hpThresholdPct = 1;
    if (normalized.hpThresholdPct > 99) normalized.hpThresholdPct = 99;
    if (normalized.mpThresholdPct < 1) normalized.mpThresholdPct = 1;
    if (normalized.mpThresholdPct > 99) normalized.mpThresholdPct = 99;
    if (normalized.writeTickMs == 0) normalized.writeTickMs = NowTickMs();
    normalized.worldName[sizeof(normalized.worldName) - 1] = 0;

    const std::string path = UserConfigIniPath(binDir);
    const bool ok = UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "core", "version", kPayloadControlCoreIniVersion);
        IniSetBool(ini, "core", "invuln", normalized.invuln != 0);
        IniSetBool(ini, "core", "attackAccel", normalized.attackAccel != 0);
        // 策略可持久化；武装开关永不落盘（强制 false，清历史 key）。
        IniSetBool(ini, "core", "fly", false);
        IniSetU32(ini, "core", "flyMode", normalized.flyMode);
        IniSetU32(ini, "core", "flyHopCdMs", normalized.flyHopCdMs);
        IniSetBool(ini, "core", "autoEnter", normalized.autoEnter != 0);
        IniSetBool(ini, "core", "hpPotion", normalized.hpPotion != 0);
        IniSetBool(ini, "core", "mpPotion", normalized.mpPotion != 0);
        IniSetBool(ini, "core", "petSummon", normalized.petSummon != 0);
        IniSetBool(ini, "core", "petSummonRequireFood", normalized.petSummonRequireFood != 0);
        IniSetBool(ini, "core", "multiSkill", normalized.multiSkill != 0);
        IniSetU32(ini, "core", "multiSkillGapMs", normalized.multiSkillGapMs);
        IniSetBool(ini, "core", "multiSkillSafeStagger", normalized.multiSkillSafeStagger != 0);
        IniSetBool(ini, "core", "simpleCombat", normalized.simpleCombat != 0);
        IniSetBool(ini, "core", "simpleCombatSmartInterval",
                   normalized.simpleCombatSmartInterval != 0);
        IniSetU32(ini, "core", "simpleCombatAttackIntervalMs",
                  normalized.simpleCombatAttackIntervalMs);
        IniSetU32(ini, "core", "simpleCombatTickMs", normalized.simpleCombatTickMs);
        IniSetU32(ini, "core", "clusterWeight", normalized.clusterWeight);
        IniSetBool(ini, "core", "simpleCombatTeleport", normalized.simpleCombatTeleport != 0);
        IniSetBool(ini, "core", "simpleCombatLiveStep", normalized.simpleCombatLiveStep != 0);
        IniSetBool(ini, "core", "attackRpc", normalized.attackRpc != 0);
        IniSetU32(ini, "core", "attackRpcMobs", normalized.attackRpcMobs);
        IniSetU32(ini, "core", "attackRpcIntervalMs", normalized.attackRpcIntervalMs);
        IniSetU32(ini, "core", "attackRpcDamage", normalized.attackRpcDamage);
        IniSetBool(ini, "core", "autoLie", normalized.autoLie != 0);
        IniSetBool(ini, "core", "autoLieDryRun", normalized.autoLieDryRun != 0);
        IniSetU32(ini, "core", "autoLieAlarmTestSeq", normalized.autoLieAlarmTestSeq);
        IniSetU32(ini, "core", "autoLieMouseSmokeSeq", normalized.autoLieMouseSmokeSeq);
        IniSetU32(ini, "core", "manualRejoinSeq", normalized.manualRejoinSeq);
        IniSetU32(ini, "core", "teleportTestSeq", normalized.teleportTestSeq);
        IniSetU32(ini, "core", "teleportNativeTestSeq", normalized.teleportNativeTestSeq);
        IniSetU32(ini, "core", "teleportKickStressSeq", normalized.teleportKickStressSeq);
        IniSetU32(ini, "core", "teleportKickStressFineSeq", normalized.teleportKickStressFineSeq);
        IniSetU32(ini, "core", "teleportKickStressFine10Seq",
                  normalized.teleportKickStressFine10Seq);
        IniSetU32(ini, "core", "teleportKickStressLocalSeq",
                  normalized.teleportKickStressLocalSeq);
        IniSetBool(ini, "core", "autoRelogin", normalized.autoRelogin != 0);
        IniSetBool(ini, "core", "autoReloginStopCombat",
                   normalized.autoReloginStopCombat != 0);
        IniSetBool(ini, "core", "autoReloginReconnect",
                   normalized.autoReloginReconnect != 0);
        IniSetBool(ini, "core", "dropAlertBypass", normalized.dropAlertBypass != 0);
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
        IniSetU32(ini, "core", "simpleCombatTeleportCooldownMs",
                  normalized.simpleCombatTeleportCooldownMs);
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
