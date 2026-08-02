#include "lie_ai_config.h"

#include "../../common/process_util.h"
#include "../../common/xcat_config_ini.h"

#include <Windows.h>

#include <filesystem>

namespace xcat::app::anti_bot_ai {
namespace {

constexpr uint32_t kLieAiIniVersion = 1u;

std::wstring LegacyIniPathW(const std::string& binDir) {
    return (std::filesystem::path(binDir) / "lie_ai.ini").wstring();
}

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%sstate", binDir);
    CreateDirectoryA(dir, nullptr);
    return true;
}

void ApplyEndpointMigrations(LlmEndpoint& ep) {
    const auto defaults = LlmDefaultEndpoint(LlmProviderKind::QwenVision);
    if (ep.apiUrl.find("deepseek") != std::string::npos ||
        ep.model.find("deepseek") != std::string::npos) {
        ep.apiUrl = defaults.apiUrl;
        ep.model  = defaults.model;
    }
    if (ep.model == "qwen3-vl-flash") ep.model = defaults.model;
}

bool ReadLegacyStandaloneIni(const std::string& binDir, LieAiConfig& cfg) {
    if (binDir.empty()) return false;
    const std::wstring ini = LegacyIniPathW(binDir);
    if (!std::filesystem::exists(ini)) return false;

    cfg.enabled =
        GetPrivateProfileIntW(L"LieAI", L"enabled", static_cast<int>(cfg.enabled), ini.c_str()) != 0;

    wchar_t buf[1024]{};
    GetPrivateProfileStringW(L"LieAI", L"apiUrl", L"", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                             ini.c_str());
    if (buf[0]) cfg.endpoint.apiUrl = xcat::WideToUtf8(buf);

    GetPrivateProfileStringW(L"LieAI", L"apiKey", L"", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                             ini.c_str());
    cfg.endpoint.apiKey = xcat::WideToUtf8(buf);

    GetPrivateProfileStringW(L"LieAI", L"model", L"", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])),
                             ini.c_str());
    if (buf[0]) cfg.endpoint.model = xcat::WideToUtf8(buf);

    cfg.endpoint.timeoutMs =
        GetPrivateProfileIntW(L"LieAI", L"timeoutMs", cfg.endpoint.timeoutMs, ini.c_str());
    if (cfg.endpoint.timeoutMs < 1000) cfg.endpoint.timeoutMs = 1000;
    if (cfg.endpoint.timeoutMs > 120000) cfg.endpoint.timeoutMs = 120000;
    return true;
}

bool ReadLieAiIni(const char* binDir, LieAiConfig& cfg, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;

    xcat::IniStore ini{};
    const std::string path = xcat::UserConfigIniPath(binDir);
    if (!xcat::LoadIniFile(path.c_str(), ini)) return false;

    uint32_t version = 0;
    if (!xcat::IniGetU32(ini, "lie_ai", "version", version) || version != kLieAiIniVersion)
        return false;

    bool enabled = cfg.enabled;
    if (xcat::IniGetBool(ini, "lie_ai", "enabled", enabled)) cfg.enabled = enabled;

    std::string apiUrl;
    if (xcat::IniGetString(ini, "lie_ai", "apiUrl", apiUrl)) cfg.endpoint.apiUrl = apiUrl;

    std::string apiKey;
    if (xcat::IniGetString(ini, "lie_ai", "apiKey", apiKey)) cfg.endpoint.apiKey = apiKey;

    std::string model;
    if (xcat::IniGetString(ini, "lie_ai", "model", model)) cfg.endpoint.model = model;

    uint32_t timeoutMs = static_cast<uint32_t>(cfg.endpoint.timeoutMs);
    if (xcat::IniGetU32(ini, "lie_ai", "timeoutMs", timeoutMs))
        cfg.endpoint.timeoutMs = static_cast<int>(timeoutMs);
    if (cfg.endpoint.timeoutMs < 1000) cfg.endpoint.timeoutMs = 1000;
    if (cfg.endpoint.timeoutMs > 120000) cfg.endpoint.timeoutMs = 120000;

    if (outWriteTick) xcat::IniGetU64(ini, "lie_ai", "writeTickMs", *outWriteTick);
    return true;
}

bool WriteLieAiIni(const char* binDir, const LieAiConfig& cfg, uint64_t writeTickMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;

    xcat::IniStore ini{};
    const std::string path = xcat::UserConfigIniPath(binDir);
    xcat::LoadIniFile(path.c_str(), ini);

    xcat::IniSetU32(ini, "meta", "version", static_cast<uint32_t>(xcat::kUserConfigIniVersion));
    xcat::IniSetU32(ini, "lie_ai", "version", kLieAiIniVersion);
    xcat::IniSetU64(ini, "lie_ai", "writeTickMs", writeTickMs);
    xcat::IniSetBool(ini, "lie_ai", "enabled", cfg.enabled);
    xcat::IniSetString(ini, "lie_ai", "apiUrl", cfg.endpoint.apiUrl.c_str());
    xcat::IniSetString(ini, "lie_ai", "apiKey", cfg.endpoint.apiKey.c_str());
    xcat::IniSetString(ini, "lie_ai", "model", cfg.endpoint.model.c_str());
    xcat::IniSetU32(ini, "lie_ai", "timeoutMs", static_cast<uint32_t>(cfg.endpoint.timeoutMs));

    return xcat::SaveIniFile(path.c_str(), ini);
}

}  // namespace

void LieAiConfigSetDefaults(LieAiConfig& cfg) {
    // 测谎主路径依赖 LLM；无 ini 时默认开（内置 Qwen key），用户可在 user.ini [lie_ai] 关闭。
    cfg.enabled  = true;
    cfg.endpoint = LlmDefaultEndpoint(LlmProviderKind::QwenVision);
}

void LoadLieAiConfig(const std::string& binDir, LieAiConfig& cfg) {
    LieAiConfigSetDefaults(cfg);
    if (binDir.empty()) return;

    uint64_t iniTick = 0;
    LieAiConfig iniCfg{};
    const bool iniOk = ReadLieAiIni(binDir.c_str(), iniCfg, &iniTick);

    LieAiConfig legacy{};
    const bool legacyOk = ReadLegacyStandaloneIni(binDir, legacy);

    if (iniOk) {
        cfg = iniCfg;
    } else if (legacyOk) {
        cfg = legacy;
        WriteLieAiIni(binDir.c_str(), cfg, GetTickCount64());
    } else {
        return;
    }

    ApplyEndpointMigrations(cfg.endpoint);
}

void SaveLieAiConfig(const std::string& binDir, const LieAiConfig& cfg) {
    if (binDir.empty()) return;
    WriteLieAiIni(binDir.c_str(), cfg, GetTickCount64());
}

LieAiConfig LoadLieAiConfigResolved(const std::string& binDir) {
    LieAiConfig cfg{};
    LoadLieAiConfig(binDir, cfg);
    LlmResolveEndpoint(cfg.endpoint, LlmProviderKind::QwenVision);
    return cfg;
}

}  // namespace xcat::app::anti_bot_ai
