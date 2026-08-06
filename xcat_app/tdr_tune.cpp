#include "tdr_tune.h"

#include <Windows.h>

#include <cstdio>
#include <string>

namespace xcat::app::tdr {
namespace {

constexpr wchar_t kGraphicsDrivers[] =
    L"SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers";

std::string WinErr(LONG code) {
    char buf[96]{};
    std::snprintf(buf, sizeof(buf), "winerr=%ld", static_cast<long>(code));
    return buf;
}

bool QueryDword(HKEY key, const wchar_t* name, DWORD* out, bool* present) {
    DWORD ty = 0;
    DWORD cb = sizeof(DWORD);
    DWORD v = 0;
    const LONG st = RegQueryValueExW(key, name, nullptr, &ty, reinterpret_cast<LPBYTE>(&v), &cb);
    if (st == ERROR_FILE_NOT_FOUND) {
        *present = false;
        return true;
    }
    if (st != ERROR_SUCCESS || ty != REG_DWORD || cb != sizeof(DWORD)) return false;
    *present = true;
    *out = v;
    return true;
}

bool SetDword(HKEY key, const wchar_t* name, DWORD v) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v),
                          sizeof(v)) == ERROR_SUCCESS;
}

bool DeleteValueIfPresent(HKEY key, const wchar_t* name) {
    const LONG st = RegDeleteValueW(key, name);
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

}  // namespace

Snapshot Read() {
    Snapshot s;
    HKEY key = nullptr;
    const LONG st =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, kGraphicsDrivers, 0, KEY_READ | KEY_WOW64_64KEY, &key);
    if (st != ERROR_SUCCESS) {
        s.err = "无法打开 GraphicsDrivers（" + WinErr(st) + "）";
        return s;
    }
    DWORD delay = 2;
    DWORD ddi = 5;
    const bool delayOk = QueryDword(key, L"TdrDelay", &delay, &s.delayPresent);
    const bool ddiOk = QueryDword(key, L"TdrDdiDelay", &ddi, &s.ddiPresent);
    RegCloseKey(key);
    if (!delayOk || !ddiOk) {
        s.err = "读取 TdrDelay/TdrDdiDelay 失败";
        return s;
    }
    s.delaySec = s.delayPresent ? delay : 2;
    s.ddiSec = s.ddiPresent ? ddi : 5;
    s.readable = true;
    return s;
}

bool ApplyRecommended(uint32_t recommendedSec, std::string* errOut) {
    if (recommendedSec < 2 || recommendedSec > 60) {
        if (errOut) *errOut = "秒数超出范围（2–60）";
        return false;
    }
    HKEY key = nullptr;
    DWORD disp = 0;
    const LONG st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kGraphicsDrivers, 0, nullptr, 0,
                                    KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disp);
    if (st != ERROR_SUCCESS) {
        if (errOut) *errOut = "无法打开/创建 GraphicsDrivers（" + WinErr(st) + "）";
        return false;
    }
    const DWORD v = static_cast<DWORD>(recommendedSec);
    const bool ok = SetDword(key, L"TdrDelay", v) && SetDword(key, L"TdrDdiDelay", v);
    RegCloseKey(key);
    if (!ok) {
        if (errOut) *errOut = "写入 TdrDelay/TdrDdiDelay 失败";
        return false;
    }
    return true;
}

bool RestoreDefaults(std::string* errOut) {
    HKEY key = nullptr;
    const LONG st = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kGraphicsDrivers, 0,
                                  KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (st != ERROR_SUCCESS) {
        if (errOut) *errOut = "无法打开 GraphicsDrivers（" + WinErr(st) + "）";
        return false;
    }
    const bool ok =
        DeleteValueIfPresent(key, L"TdrDelay") && DeleteValueIfPresent(key, L"TdrDdiDelay");
    RegCloseKey(key);
    if (!ok) {
        if (errOut) *errOut = "删除 TdrDelay/TdrDdiDelay 失败";
        return false;
    }
    return true;
}

}  // namespace xcat::app::tdr
