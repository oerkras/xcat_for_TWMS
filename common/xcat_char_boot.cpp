#include "xcat_char_boot.h"
#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>
#include <cstring>
#include <string>

namespace xcat {
namespace {

constexpr uint32_t kCharBootIniVersion = 1u;
constexpr char kSec[] = "char_boot";
constexpr char kSecSt[] = "char_boot_status";

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

uint32_t ClampU32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool ReadCfgIni(const char* binDir, CharBootConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;
    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;
    uint32_t version = 0;
    if (!IniGetU32(ini, kSec, "version", version) || version != kCharBootIniVersion) return false;
    CharBootSetDefaults(out);
    IniGetU32(ini, kSec, "farmMap", out.farmMap);
    IniGetU32(ini, kSec, "hangupMap", out.hangupMap);
    IniGetU32(ini, kSec, "departKind", out.departKind);
    IniGetU32(ini, kSec, "levelMin", out.levelMin);
    IniGetU32(ini, kSec, "mesoMin", out.mesoMin);
    bool b = false;
    IniGetU32(ini, kSec, "farmTimeoutMin", out.farmTimeoutMin);
    if (IniGetBool(ini, kSec, "autoCreateChar", b)) out.autoCreateChar = b ? 1u : 0u;
    IniGetU32(ini, kSec, "manualSeq", out.manualSeq);
    IniGetU32(ini, kSec, "manualKind", out.manualKind);
    CharBootNormalize(out);
    if (outWriteTick) IniGetU64(ini, kSec, "writeTickMs", *outWriteTick);
    return true;
}

bool WriteCfgIni(const char* binDir, const CharBootConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;
    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, kSec, "version", kCharBootIniVersion);
        IniSetU64(ini, kSec, "writeTickMs", writeTickMs);
        IniSetU32(ini, kSec, "farmMap", cfg.farmMap);
        IniSetU32(ini, kSec, "hangupMap", cfg.hangupMap);
        IniSetU32(ini, kSec, "departKind", cfg.departKind);
        IniSetU32(ini, kSec, "levelMin", cfg.levelMin);
        IniSetU32(ini, kSec, "mesoMin", cfg.mesoMin);
        IniSetBool(ini, kSec, "requireInt20", false);
        IniSetU32(ini, kSec, "farmTimeoutMin", cfg.farmTimeoutMin);
        IniSetBool(ini, kSec, "autoCreateChar", cfg.autoCreateChar != 0);
        IniSetU32(ini, kSec, "manualSeq", cfg.manualSeq);
        IniSetU32(ini, kSec, "manualKind", cfg.manualKind);
    });
}

}  // namespace

void CharBootSetDefaults(CharBootConfig& out) {
    out = CharBootConfig{};
    out.magic = kCharBootMagic;
    out.version = kCharBootVersion;
}

void CharBootNormalize(CharBootConfig& cfg) {
    cfg.magic = kCharBootMagic;
    cfg.version = kCharBootVersion;
    if (cfg.farmMap == 0) cfg.farmMap = kCharBootDefaultFarmMap;
    if (cfg.hangupMap == 0) cfg.hangupMap = kCharBootDefaultHangupMap;
    cfg.departKind = (cfg.departKind == kCharBootDepartMeso) ? kCharBootDepartMeso
                                                            : kCharBootDepartLevel;
    cfg.levelMin = ClampU32(cfg.levelMin, kCharBootLevelMinLo, kCharBootLevelMinHi);
    cfg.mesoMin = ClampU32(cfg.mesoMin, kCharBootMesoMinLo, kCharBootMesoMinHi);
    cfg.requireInt20 = 0;
    cfg.autoCreateChar = cfg.autoCreateChar ? 1u : 0u;
    if (cfg.manualKind > kCharBootManualStop) cfg.manualKind = kCharBootManualNone;
}

void CharBootStatusSetDefaults(CharBootStatus& out) {
    out = CharBootStatus{};
    strncpy_s(out.state, "Idle", _TRUNCATE);
}

bool CharBootStateIsBusy(const char* state) {
    if (!state || !state[0]) return false;
    return strcmp(state, "Idle") != 0;
}

bool ReadCharBoot(const char* binDir, CharBootConfig& out) {
    CharBootSetDefaults(out);
    uint64_t tick = 0;
    if (!ReadCfgIni(binDir, out, &tick)) return false;
    out.writeTickMs = tick;
    CharBootNormalize(out);
    return true;
}

bool WriteCharBoot(const char* binDir, const CharBootConfig& cfg) {
    CharBootConfig normalized = cfg;
    CharBootNormalize(normalized);
    const uint64_t tick = cfg.writeTickMs ? cfg.writeTickMs : GetTickCount64();
    normalized.writeTickMs = tick;
    return WriteCfgIni(binDir, normalized, tick);
}

bool ReadCharBootStatus(const char* binDir, CharBootStatus& out) {
    CharBootStatusSetDefaults(out);
    if (!binDir || !binDir[0]) return false;
    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;
    std::string s;
    if (IniGetString(ini, kSecSt, "state", s) && !s.empty())
        strncpy_s(out.state, s.c_str(), _TRUNCATE);
    if (IniGetString(ini, kSecSt, "message", s)) strncpy_s(out.message, s.c_str(), _TRUNCATE);
    if (IniGetString(ini, kSecSt, "lastWhy", s)) strncpy_s(out.lastWhy, s.c_str(), _TRUNCATE);
    IniGetU32(ini, kSecSt, "mapId", out.mapId);
    int32_t i32 = 0;
    if (IniGetI32(ini, kSecSt, "level", i32)) out.level = i32;
    if (IniGetI32(ini, kSecSt, "job", i32)) out.job = i32;
    if (IniGetI32(ini, kSecSt, "intel", i32)) out.intel = i32;
    std::string meso;
    if (IniGetString(ini, kSecSt, "meso", meso) && !meso.empty())
        out.meso = _strtoi64(meso.c_str(), nullptr, 10);
    IniGetU32(ini, kSecSt, "ready", out.ready);
    IniGetU32(ini, kSecSt, "hangupMap", out.hangupMap);
    IniGetU64(ini, kSecSt, "writeTickMs", out.writeTickMs);
    return true;
}

bool WriteCharBootStatus(const char* binDir, const CharBootStatus& status) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;
    const std::string path = UserConfigIniPath(binDir);
    const uint64_t tick = status.writeTickMs ? status.writeTickMs : GetTickCount64();
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU64(ini, kSecSt, "writeTickMs", tick);
        IniSetString(ini, kSecSt, "state", status.state[0] ? status.state : "Idle");
        IniSetString(ini, kSecSt, "message", status.message);
        IniSetString(ini, kSecSt, "lastWhy", status.lastWhy);
        IniSetU32(ini, kSecSt, "mapId", status.mapId);
        IniSetI32(ini, kSecSt, "level", status.level);
        IniSetI32(ini, kSecSt, "job", status.job);
        IniSetI32(ini, kSecSt, "intel", status.intel);
        char mesoBuf[32]{};
        snprintf(mesoBuf, sizeof(mesoBuf), "%lld", static_cast<long long>(status.meso));
        IniSetString(ini, kSecSt, "meso", mesoBuf);
        IniSetU32(ini, kSecSt, "ready", status.ready ? 1u : 0u);
        IniSetU32(ini, kSecSt, "hangupMap", status.hangupMap);
    });
}

}  // namespace xcat
