#include "xcat_payload_control.h"

#include "process_util.h"
#include "xcat_config_ini.h"

#include <Windows.h>

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

}  // namespace

void PayloadControlSetDefaults(PayloadControl& out) {
    out = PayloadControl{};
    out.magic = kPayloadControlMagic;
    out.version = kPayloadControlVersion;
    out.fly = 0;
    out.invuln = 0;
    out.autoEnter = 0;
    out.charSlot = 1;
    out.worldId = 0;
    out.worldName[0] = 0;
    out.writeTickMs = 0;
}

bool ReadPayloadControl(const char* binDir, PayloadControl& out) {
    PayloadControlSetDefaults(out);
    if (!binDir || !binDir[0]) return false;

    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;

    bool b = false;
    if (IniGetBool(ini, "core", "fly", b)) out.fly = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "invuln", b)) out.invuln = b ? 1u : 0u;
    if (IniGetBool(ini, "core", "autoEnter", b)) out.autoEnter = b ? 1u : 0u;
    uint32_t u = 0;
    if (IniGetU32(ini, "core", "charSlot", u) && u >= 1 && u <= 32) out.charSlot = u;
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
    normalized.fly = normalized.fly ? 1u : 0u;
    normalized.invuln = normalized.invuln ? 1u : 0u;
    normalized.autoEnter = normalized.autoEnter ? 1u : 0u;
    if (normalized.charSlot < 1) normalized.charSlot = 1;
    if (normalized.charSlot > 32) normalized.charSlot = 32;
    if (normalized.writeTickMs == 0) normalized.writeTickMs = NowTickMs();
    normalized.worldName[sizeof(normalized.worldName) - 1] = 0;

    const std::string path = UserConfigIniPath(binDir);
    return UpdateIniFile(path.c_str(), [&](IniStore& ini) {
        IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
        IniSetU32(ini, "core", "version", kPayloadControlCoreIniVersion);
        IniSetBool(ini, "core", "fly", normalized.fly != 0);
        IniSetBool(ini, "core", "invuln", normalized.invuln != 0);
        IniSetBool(ini, "core", "autoEnter", normalized.autoEnter != 0);
        IniSetU32(ini, "core", "charSlot", normalized.charSlot);
        IniSetU32(ini, "core", "worldId", static_cast<uint32_t>(normalized.worldId));
        IniSetString(ini, "core", "worldName", normalized.worldName);
        IniSetU64(ini, "core", "writeTickMs", normalized.writeTickMs);
    });
}

}  // namespace xcat
