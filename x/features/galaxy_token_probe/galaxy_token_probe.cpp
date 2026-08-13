// Classic TWMS — Galaxy_* PlayerPrefs read-only probe (default OFF).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "galaxy_token_probe.h"

#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/managed_main.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace x::features::galaxy_token_probe {
namespace {

constexpr wchar_t kMarkerName[] = L"galaxy_token_probe.on";

// dump.cs TypeDef 1706 key names (PlayerPrefs).
constexpr const char* kKeyObjectId = "Galaxy_UserObjectID";
constexpr const char* kKeySessionToken = "Galaxy_UserSessionToken";
constexpr const char* kKeyExpire = "Galaxy_ExpireTime";
constexpr const char* kKeyDevice = "Galaxy_UniqueDevice";

constexpr int kTokenPrefixChars = 8;  // never log full token

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

// UnityEngine.PlayerPrefs statics (thiscall-less; MI last).
using FnHasKey = uint8_t(__fastcall*)(void* key, const void* method);
using FnGetString1 = void*(__fastcall*)(void* key, const void* method);

std::atomic<bool> gInited{false};
std::atomic<bool> gUiEnabled{false};
std::atomic<DWORD> lastSampleTick{0};
std::atomic<bool> gSampleBusy{false};

char gPendingWhy[64]{};

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
        (void)x::runtime::AppendDbgLog(dir + L"\\galaxy_token.log", buf, static_cast<DWORD>(n));
    x::runtime::LogI("GalaxyTokenProbe", "%s", body);
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
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u [galaxy_token] %s\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    const std::wstring dir = ResolveLogDir();
    if (!dir.empty())
        (void)x::runtime::AppendDbgLog(dir + L"\\kick.log", buf, static_cast<DWORD>(n));
}

MethodInfoHead* FindMi(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethodFromName) return nullptr;
    void* mi = nullptr;
    __try {
        mi = e.classGetMethodFromName(klass, name, argc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mi = nullptr;
    }
    return reinterpret_cast<MethodInfoHead*>(mi);
}

bool ManagedToUtf8(void* str, char* out, size_t outCap, int* outLen) {
    if (outCap == 0) return false;
    out[0] = '\0';
    if (outLen) *outLen = 0;
    if (!str || !x::runtime::il2cpp::LooksLikeHeapPtr(str)) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.stringLength || !e.stringChars) return false;
    int len = 0;
    const wchar_t* chars = nullptr;
    __try {
        len = e.stringLength(str);
        chars = e.stringChars(str);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!chars || len < 0) return false;
    if (len == 0) {
        if (outLen) *outLen = 0;
        return true;
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, chars, len, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    if (outLen) *outLen = need;
    if (static_cast<size_t>(need) >= outCap) {
        // Fit a truncated UTF-8 prefix (token redaction path).
        WideCharToMultiByte(CP_UTF8, 0, chars, len, out, static_cast<int>(outCap - 1), nullptr,
                            nullptr);
        out[outCap - 1] = '\0';
        return true;
    }
    WideCharToMultiByte(CP_UTF8, 0, chars, len, out, need, nullptr, nullptr);
    out[need] = '\0';
    return true;
}

void SanitizePrefix(char* s, int maxChars) {
    if (!s) return;
    int n = 0;
    for (; s[n] && n < maxChars; ++n) {
        const unsigned char c = static_cast<unsigned char>(s[n]);
        if (c < 0x20 || c > 0x7E) s[n] = '.';
    }
    s[n] = '\0';
}

struct KeySnap {
    const char* key;
    int has = -1;  // -1=err 0=no 1=yes
    int len = -1;
    char prefix[16]{};
    char fullSafe[96]{};  // only for non-token keys
};

void SampleOne(FnHasKey hasFn, void* hasMi, FnGetString1 getFn, void* getMi, KeySnap* snap,
               bool redactToken) {
    if (!snap || !snap->key) return;
    snap->has = -1;
    snap->len = -1;
    snap->prefix[0] = '\0';
    snap->fullSafe[0] = '\0';

    void* keyStr = x::runtime::il2cpp::NewString(snap->key);
    if (!keyStr) {
        snap->has = -1;
        return;
    }

    uint8_t has = 0;
    __try {
        has = hasFn(keyStr, hasMi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snap->has = -1;
        return;
    }
    snap->has = has ? 1 : 0;
    if (!has) {
        snap->len = 0;
        return;
    }

    void* val = nullptr;
    __try {
        val = getFn(keyStr, getMi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        snap->len = -1;
        return;
    }

    char full[512]{};
    int fullLen = 0;
    if (!ManagedToUtf8(val, full, sizeof(full), &fullLen)) {
        snap->len = -1;
        return;
    }
    snap->len = fullLen;
    if (redactToken) {
        strncpy_s(snap->prefix, full, _TRUNCATE);
        SanitizePrefix(snap->prefix, kTokenPrefixChars);
    } else {
        strncpy_s(snap->fullSafe, full, _TRUNCATE);
        SanitizePrefix(snap->fullSafe, 80);
        strncpy_s(snap->prefix, full, _TRUNCATE);
        SanitizePrefix(snap->prefix, kTokenPrefixChars);
    }
}

void SampleOnPump(void* /*user*/) {
    if (!x::runtime::main_thread::AssertOnPumpThread("GalaxyTokenProbe")) {
        LogLine("SAMPLE abort: not on pump thread why=%s", gPendingWhy);
        gSampleBusy.store(false);
        return;
    }
    if (!x::runtime::il2cpp::Ensure() || !x::runtime::il2cpp::ManagedAllocSafe()) {
        LogLine("SAMPLE abort: il2cpp/ManagedAllocSafe why=%s", gPendingWhy);
        KickLogLine("sample_fail why=%s reason=il2cpp", gPendingWhy);
        gSampleBusy.store(false);
        return;
    }

    void* klass = x::runtime::il2cpp::FindClass("UnityEngine", "PlayerPrefs");
    if (!klass) {
        LogLine("SAMPLE abort: PlayerPrefs klass miss why=%s", gPendingWhy);
        KickLogLine("sample_fail why=%s reason=klass", gPendingWhy);
        gSampleBusy.store(false);
        return;
    }
    x::runtime::il2cpp::RuntimeClassInit(klass);

    MethodInfoHead* hasMi = FindMi(klass, "HasKey", 1);
    MethodInfoHead* getMi = FindMi(klass, "GetString", 1);
    if (!hasMi || !hasMi->methodPointer || !getMi || !getMi->methodPointer) {
        LogLine("SAMPLE abort: HasKey/GetString MI miss why=%s has=%p get=%p", gPendingWhy, hasMi,
                getMi);
        KickLogLine("sample_fail why=%s reason=mi", gPendingWhy);
        gSampleBusy.store(false);
        return;
    }

    auto hasFn = reinterpret_cast<FnHasKey>(hasMi->methodPointer);
    auto getFn = reinterpret_cast<FnGetString1>(getMi->methodPointer);

    KeySnap oid{kKeyObjectId};
    KeySnap tok{kKeySessionToken};
    KeySnap exp{kKeyExpire};
    KeySnap dev{kKeyDevice};
    SampleOne(hasFn, hasMi, getFn, getMi, &oid, false);
    SampleOne(hasFn, hasMi, getFn, getMi, &tok, true);
    SampleOne(hasFn, hasMi, getFn, getMi, &exp, false);
    SampleOne(hasFn, hasMi, getFn, getMi, &dev, false);

    LogLine(
        "SAMPLE why=%s "
        "oid(has=%d len=%d val=%s) "
        "token(has=%d len=%d prefix=%s) "
        "expire(has=%d len=%d val=%s) "
        "device(has=%d len=%d val=%s)",
        gPendingWhy, oid.has, oid.len, oid.fullSafe[0] ? oid.fullSafe : "-", tok.has, tok.len,
        tok.prefix[0] ? tok.prefix : "-", exp.has, exp.len, exp.fullSafe[0] ? exp.fullSafe : "-",
        dev.has, dev.len, dev.fullSafe[0] ? dev.fullSafe : "-");

    KickLogLine("why=%s token_has=%d token_len=%d token_prefix=%s oid_has=%d expire_has=%d",
                gPendingWhy, tok.has, tok.len, tok.prefix[0] ? tok.prefix : "-", oid.has, exp.has);

    lastSampleTick.store(GetTickCount());
    gSampleBusy.store(false);
}

}  // namespace

void Init() {
    if (gInited.exchange(true)) return;
    if (IsArmed()) {
        LogLine("armed — will sample on connected/disconnect edges");
        KickLogLine("armed");
    } else {
        LogLine("idle (soft-login UI / galaxy_token_probe.on / GALAXY_TOKEN_PROBE=1)");
    }
}

void Shutdown() {
    gInited.store(false);
}

void SetEnabled(bool on) {
    const bool prev = gUiEnabled.exchange(on);
    if (prev == on) return;
    if (on) {
        LogLine("UI enable — sample on connected/disconnect");
        KickLogLine("armed ui");
    } else if (!MarkerArmed() && !EnvOn("GALAXY_TOKEN_PROBE")) {
        LogLine("UI disable — idle (marker/env still override if present)");
    }
}

bool IsArmed() {
    return gUiEnabled.load(std::memory_order_acquire) || MarkerArmed() ||
           EnvOn("GALAXY_TOKEN_PROBE");
}

void RequestSample(const char* why) {
    if (!IsArmed()) return;
    if (gSampleBusy.exchange(true)) {
        LogLine("SAMPLE skip busy why=%s", why ? why : "?");
        return;
    }
    const DWORD now = GetTickCount();
    const DWORD last = lastSampleTick.load();
    // Debounce non-disconnect samples; disconnect always attempts.
    const bool isDisc =
        why && (strstr(why, "disconnect") != nullptr || strstr(why, "Disconnect") != nullptr);
    if (!isDisc && last != 0 && (now - last) < 1500) {
        gSampleBusy.store(false);
        return;
    }

    memset(gPendingWhy, 0, sizeof(gPendingWhy));
    strncpy_s(gPendingWhy, why ? why : "?", _TRUNCATE);

    if (!x::runtime::managed_main::Call(&SampleOnPump, nullptr, 2500)) {
        LogLine("SAMPLE Call timeout/fail why=%s", gPendingWhy);
        KickLogLine("sample_fail why=%s reason=pump", gPendingWhy);
        gSampleBusy.store(false);
    }
}

}  // namespace x::features::galaxy_token_probe
