#include "xcat_auto_stat.h"
#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>
#include <string>

namespace xcat {
namespace {

constexpr uint32_t kAutoStatIniVersion = 1u;
constexpr char kSec[] = "auto_stat";

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

uint32_t ClampRatio(uint32_t v) {
    if (v > kAutoStatRatioMax) return kAutoStatRatioMax;
    return v;
}

bool ReadAutoStatIni(const char* binDir, AutoStatConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;
    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;
    uint32_t version = 0;
    if (!IniGetU32(ini, kSec, "version", version) || version != kAutoStatIniVersion) return false;
    AutoStatSetDefaults(out);
    bool b = false;
    if (IniGetBool(ini, kSec, "enabled", b)) out.enabled = b ? 1u : 0u;
    IniGetU32(ini, kSec, "str", out.str);
    IniGetU32(ini, kSec, "dex", out.dex);
    IniGetU32(ini, kSec, "int", out.intel);
    IniGetU32(ini, kSec, "luk", out.luk);
    AutoStatNormalize(out);
    if (outWriteTick) IniGetU64(ini, kSec, "writeTickMs", *outWriteTick);
    return true;
}

bool WriteAutoStatIni(const char* binDir, const AutoStatConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;
    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, kSec, "version", kAutoStatIniVersion);
        IniSetU64(ini, kSec, "writeTickMs", writeTickMs);
        IniSetBool(ini, kSec, "enabled", cfg.enabled != 0);
        IniSetU32(ini, kSec, "str", cfg.str);
        IniSetU32(ini, kSec, "dex", cfg.dex);
        IniSetU32(ini, kSec, "int", cfg.intel);
        IniSetU32(ini, kSec, "luk", cfg.luk);
    });
}

}  // namespace

void AutoStatSetDefaults(AutoStatConfig& out) {
    out = AutoStatConfig{};
    out.magic = kAutoStatMagic;
    out.version = kAutoStatVersion;
    out.enabled = kAutoStatDefaultEnabled;
}

void AutoStatNormalize(AutoStatConfig& cfg) {
    cfg.magic = kAutoStatMagic;
    cfg.version = kAutoStatVersion;
    cfg.enabled = cfg.enabled ? 1u : 0u;
    cfg.str = ClampRatio(cfg.str);
    cfg.dex = ClampRatio(cfg.dex);
    cfg.intel = ClampRatio(cfg.intel);
    cfg.luk = ClampRatio(cfg.luk);
}

uint32_t AutoStatRatioSumOf(const AutoStatConfig& cfg) {
    return cfg.str + cfg.dex + cfg.intel + cfg.luk;
}

bool AutoStatRatioOk(const AutoStatConfig& cfg) {
    return AutoStatRatioSumOf(cfg) == kAutoStatRatioSum;
}

bool ReadAutoStat(const char* binDir, AutoStatConfig& out) {
    AutoStatSetDefaults(out);
    uint64_t tick = 0;
    if (!ReadAutoStatIni(binDir, out, &tick)) return false;
    out.writeTickMs = tick;
    AutoStatNormalize(out);
    return true;
}

bool WriteAutoStat(const char* binDir, const AutoStatConfig& cfg) {
    AutoStatConfig normalized = cfg;
    AutoStatNormalize(normalized);
    const uint64_t tick = cfg.writeTickMs ? cfg.writeTickMs : GetTickCount64();
    normalized.writeTickMs = tick;
    return WriteAutoStatIni(binDir, normalized, tick);
}

}  // namespace xcat
