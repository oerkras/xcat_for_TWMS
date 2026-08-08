#include "xcat_config_ini.h"

#include <Windows.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace xcat {
namespace {

std::string JoinBinRelative(const char* binDir, const char* relative) {
    std::string out = binDir ? binDir : "";
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out.push_back('\\');
    out += relative ? relative : "";
    return out;
}

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    const std::string dir = JoinBinRelative(binDir, "state");
    CreateDirectoryA(dir.c_str(), nullptr);
    return true;
}

bool FileExistsA(const char* path) {
    if (!path || !path[0]) return false;
    const DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 互斥量名只认规范化路径，避免 XCat_data\ vs XCat_data/ 拆成两把锁。
std::string NormalizePathForMutex(const char* path) {
    std::string out;
    if (!path || !path[0]) return out;
    out.reserve(std::strlen(path));
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p) {
        char c = static_cast<char>(std::tolower(*p));
        if (c == '/') c = '\\';
        if (c == '\\') {
            if (!out.empty() && out.back() == '\\') continue;
            out.push_back('\\');
            continue;
        }
        out.push_back(c);
    }
    while (out.size() > 3 && out.back() == '\\') out.pop_back();
    return out;
}

uint64_t Fnv1a64(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

// launcher 与 payload 分属不同进程，必须用命名互斥量串行 user.ini 读改写。
class IniPathMutex {
public:
    explicit IniPathMutex(const char* path) {
        const std::string key = NormalizePathForMutex(path);
        wchar_t name[64]{};
        swprintf_s(name, L"Local\\xcat_ini_%016llx",
                   static_cast<unsigned long long>(Fnv1a64(key)));
        mutex_ = CreateMutexW(nullptr, FALSE, name);
    }

    ~IniPathMutex() {
        Unlock();
        if (mutex_) CloseHandle(mutex_);
    }

    bool Lock(DWORD timeoutMs) {
        if (!mutex_ || held_) return held_;
        const DWORD wr = WaitForSingleObject(mutex_, timeoutMs);
        held_ = (wr == WAIT_OBJECT_0 || wr == WAIT_ABANDONED);
        return held_;
    }

    void Unlock() {
        if (mutex_ && held_) {
            ReleaseMutex(mutex_);
            held_ = false;
        }
    }

    bool held() const { return held_; }

    IniPathMutex(const IniPathMutex&) = delete;
    IniPathMutex& operator=(const IniPathMutex&) = delete;

private:
    HANDLE mutex_ = nullptr;
    bool held_ = false;
};

// 析构自动 Unlock，避免 UpdateIniFile 中途 return 漏释放。
class IniPathLockGuard {
public:
    IniPathLockGuard(IniPathMutex& mu, DWORD timeoutMs) : mu_(mu) { locked_ = mu_.Lock(timeoutMs); }
    ~IniPathLockGuard() { mu_.Unlock(); }

    bool locked() const { return locked_; }

    IniPathLockGuard(const IniPathLockGuard&) = delete;
    IniPathLockGuard& operator=(const IniPathLockGuard&) = delete;

private:
    IniPathMutex& mu_;
    bool locked_ = false;
};

bool LoadIniFileUnlocked(const char* path, IniStore& out) {
    out.clear();
    if (!path || !path[0]) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = Trim(line);
        if (t.empty() || t[0] == ';' || t[0] == '#') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(t.substr(0, eq));
        std::string val = Trim(t.substr(eq + 1));
        if (section.empty() || key.empty()) continue;
        out[section][key] = val;
    }
    return true;
}

bool SaveIniFileUnlocked(const char* path, const IniStore& store) {
    if (!path || !path[0]) return false;

    std::string tmp = path;
    tmp += ".tmp.";
    tmp += std::to_string(GetCurrentProcessId());

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const auto& sec : store) {
            out << '[' << sec.first << "]\n";
            for (const auto& kv : sec.second) {
                out << kv.first << '=' << kv.second << '\n';
            }
            out << '\n';
        }
        out.flush();
        if (!out) {
            DeleteFileA(tmp.c_str());
            return false;
        }
    }

    if (!MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace

std::string UserConfigIniRelPath() { return "state\\user.ini"; }

std::string UserConfigIniPath(const char* binDir) {
    return JoinBinRelative(binDir, UserConfigIniRelPath().c_str());
}

bool LoadIniFile(const char* path, IniStore& out) {
    if (!path || !path[0]) {
        out.clear();
        return false;
    }

    // 短重试抢锁：降低「写盘中读到半截/瞬时失败」概率；仍失败再降级裸读。
    for (int attempt = 0; attempt < 3; ++attempt) {
        IniPathMutex mu(path);
        IniPathLockGuard guard(mu, attempt == 0 ? 2000u : 1500u);
        if (!guard.locked()) {
            Sleep(30u * static_cast<DWORD>(attempt + 1));
            continue;
        }
        return LoadIniFileUnlocked(path, out);
    }

    // 最后兜底：热路径不能因锁饿死；可能读到略旧值。
    return LoadIniFileUnlocked(path, out);
}

bool SaveIniFile(const char* path, const IniStore& store) {
    if (!path || !path[0]) return false;
    IniPathMutex mu(path);
    IniPathLockGuard guard(mu, 5000);
    if (!guard.locked()) return false;
    return SaveIniFileUnlocked(path, store);
}

bool UpdateIniFile(const char* path, const std::function<void(IniStore&)>& mutate) {
    if (!path || !path[0] || !mutate) return false;

    IniPathMutex mu(path);
    IniPathLockGuard guard(mu, 8000);
    if (!guard.locked()) return false;

    IniStore ini{};
    const bool existed = FileExistsA(path);
    if (existed) {
        // 文件已在盘上却读失败时绝不能用空表落盘——那会抹掉 [core]/其它 section，
        // 下一轮 ReadPayloadControl 失败后再被 PushPersisted 写成整包默认值。
        if (!LoadIniFileUnlocked(path, ini)) return false;
    }

    mutate(ini);
    return SaveIniFileUnlocked(path, ini);
}

bool IniGetString(const IniStore& ini, const char* section, const char* key, std::string& out) {
    if (!section || !key) return false;
    const auto sit = ini.find(section);
    if (sit == ini.end()) return false;
    const auto kit = sit->second.find(key);
    if (kit == sit->second.end()) return false;
    out = kit->second;
    return true;
}

bool IniGetU64(const IniStore& ini, const char* section, const char* key, uint64_t& out) {
    std::string v;
    if (!IniGetString(ini, section, key, v)) return false;
    try {
        out = std::stoull(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool IniGetU32(const IniStore& ini, const char* section, const char* key, uint32_t& out) {
    uint64_t v = 0;
    if (!IniGetU64(ini, section, key, v)) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool IniGetI32(const IniStore& ini, const char* section, const char* key, int32_t& out) {
    std::string v;
    if (!IniGetString(ini, section, key, v)) return false;
    try {
        out = static_cast<int32_t>(std::stol(v));
        return true;
    } catch (...) {
        return false;
    }
}

bool IniGetBool(const IniStore& ini, const char* section, const char* key, bool& out) {
    std::string v;
    if (!IniGetString(ini, section, key, v)) return false;
    if (v == "1" || v == "true" || v == "True" || v == "TRUE" || v == "yes" || v == "on") {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "False" || v == "FALSE" || v == "no" || v == "off") {
        out = false;
        return true;
    }
    return false;
}

void IniSetString(IniStore& ini, const char* section, const char* key, const char* value) {
    if (!section || !key) return;
    ini[section][key] = value ? value : "";
}

void IniSetU64(IniStore& ini, const char* section, const char* key, uint64_t value) {
    IniSetString(ini, section, key, std::to_string(value).c_str());
}

void IniSetU32(IniStore& ini, const char* section, const char* key, uint32_t value) {
    IniSetU64(ini, section, key, static_cast<uint64_t>(value));
}

void IniSetI32(IniStore& ini, const char* section, const char* key, int32_t value) {
    IniSetString(ini, section, key, std::to_string(value).c_str());
}

void IniSetBool(IniStore& ini, const char* section, const char* key, bool value) {
    IniSetString(ini, section, key, value ? "1" : "0");
}

bool IniGetFloat(const IniStore& ini, const char* section, const char* key, float& out) {
    std::string v;
    if (!IniGetString(ini, section, key, v)) return false;
    try {
        out = std::stof(v);
        return true;
    } catch (...) {
        return false;
    }
}

void IniSetFloat(IniStore& ini, const char* section, const char* key, float value) {
    char buf[64]{};
    snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(value));
    IniSetString(ini, section, key, buf);
}

void IniEraseKey(IniStore& ini, const char* section, const char* key) {
    if (!section || !key || !key[0]) return;
    const auto sit = ini.find(section);
    if (sit == ini.end()) return;
    sit->second.erase(key);
}

void IniEraseKeysWithPrefix(IniStore& ini, const char* section, const char* prefix) {
    if (!section || !prefix || !prefix[0]) return;
    const auto sit = ini.find(section);
    if (sit == ini.end()) return;
    const size_t prefixLen = std::strlen(prefix);
    auto& sec = sit->second;
    for (auto it = sec.begin(); it != sec.end();) {
        if (it->first.size() >= prefixLen &&
            it->first.compare(0, prefixLen, prefix) == 0) {
            it = sec.erase(it);
        } else {
            ++it;
        }
    }
}

namespace {

// 解析「prefix{N}.rest」：成功时 outIndex 为 1-based 下标。
bool ParseIndexedKey(const std::string& key, const char* prefix, uint32_t& outIndex) {
    if (!prefix || !prefix[0]) return false;
    const size_t prefixLen = std::strlen(prefix);
    if (key.size() <= prefixLen || key.compare(0, prefixLen, prefix) != 0) return false;
    size_t i = prefixLen;
    if (i >= key.size() || !std::isdigit(static_cast<unsigned char>(key[i]))) return false;
    uint32_t idx = 0;
    while (i < key.size() && std::isdigit(static_cast<unsigned char>(key[i]))) {
        idx = idx * 10u + static_cast<uint32_t>(key[i] - '0');
        ++i;
    }
    if (i >= key.size() || key[i] != '.') return false;
    outIndex = idx;
    return true;
}

uint32_t ClampCount(uint32_t n, uint32_t maxN) { return n > maxN ? maxN : n; }

}  // namespace

bool IniHasIndexedKeysAbove(const IniStore& ini, const char* section, const char* prefix,
                            uint32_t keepCount) {
    if (!section || !prefix || !prefix[0]) return false;
    const auto sit = ini.find(section);
    if (sit == ini.end()) return false;
    for (const auto& kv : sit->second) {
        uint32_t idx = 0;
        if (!ParseIndexedKey(kv.first, prefix, idx)) continue;
        if (idx == 0 || idx > keepCount) return true;
    }
    return false;
}

size_t IniEraseIndexedKeysAbove(IniStore& ini, const char* section, const char* prefix,
                                uint32_t keepCount) {
    if (!section || !prefix || !prefix[0]) return 0;
    const auto sit = ini.find(section);
    if (sit == ini.end()) return 0;
    size_t erased = 0;
    auto& sec = sit->second;
    for (auto it = sec.begin(); it != sec.end();) {
        uint32_t idx = 0;
        if (ParseIndexedKey(it->first, prefix, idx) && (idx == 0 || idx > keepCount)) {
            it = sec.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    return erased;
}

bool UserConfigMigrateObsolete(const char* /*payloadBinDir*/) {
    // TWMS 尚未接入枫星 feature section；保留 API 兼容。
    return false;
}

}  // namespace xcat
