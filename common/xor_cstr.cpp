#include "xor_cstr.h"

#include <cstring>

namespace xcat::xor_cstr {
namespace {

constexpr size_t kMax = 96;

void DecodeA(const unsigned char* enc, size_t n, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (!enc || n + 1 > cap) {
        out[0] = 0;
        return;
    }
    volatile unsigned char key = 0x5A;
    for (size_t i = 0; i < n; ++i) out[i] = static_cast<char>(enc[i] ^ key);
    out[n] = 0;
}

void DecodeW(const unsigned char* enc, size_t n, wchar_t* out, size_t cap) {
    if (!out || cap == 0) return;
    if (!enc || n + 1 > cap) {
        out[0] = 0;
        return;
    }
    volatile unsigned char key = 0x5A;
    for (size_t i = 0; i < n; ++i)
        out[i] = static_cast<wchar_t>(static_cast<unsigned char>(enc[i] ^ key));
    out[n] = 0;
}

}  // namespace

FARPROC GetProc(HMODULE mod, const unsigned char* enc, size_t n) {
    if (!mod || !enc) return nullptr;
    char buf[kMax]{};
    DecodeA(enc, n, buf, sizeof(buf));
    FARPROC p = GetProcAddress(mod, buf);
    SecureZeroMemory(buf, sizeof(buf));
    return p;
}

HMODULE GetModuleW(const unsigned char* encAscii, size_t n) {
    if (!encAscii) return nullptr;
    wchar_t buf[kMax]{};
    DecodeW(encAscii, n, buf, kMax);
    HMODULE h = GetModuleHandleW(buf);
    SecureZeroMemory(buf, sizeof(buf));
    return h;
}

bool WideEqualsI(const wchar_t* s, const unsigned char* encAscii, size_t n) {
    if (!s || !encAscii) return false;
    wchar_t buf[kMax]{};
    DecodeW(encAscii, n, buf, kMax);
    const bool eq = _wcsicmp(s, buf) == 0;
    SecureZeroMemory(buf, sizeof(buf));
    return eq;
}

bool WideContains(const wchar_t* hay, const unsigned char* encAscii, size_t n) {
    if (!hay || !encAscii) return false;
    wchar_t buf[kMax]{};
    DecodeW(encAscii, n, buf, kMax);
    const bool hit = wcsstr(hay, buf) != nullptr;
    SecureZeroMemory(buf, sizeof(buf));
    return hit;
}

bool AsciiContains(const char* hay, const unsigned char* encAscii, size_t n) {
    if (!hay || !encAscii) return false;
    char buf[kMax]{};
    DecodeA(encAscii, n, buf, sizeof(buf));
    const bool hit = strstr(hay, buf) != nullptr;
    SecureZeroMemory(buf, sizeof(buf));
    return hit;
}

DWORD GetEnvA(const unsigned char* enc, size_t n, char* out, DWORD cap) {
    if (!enc) return 0;
    char name[kMax]{};
    DecodeA(enc, n, name, sizeof(name));
    const DWORD r = GetEnvironmentVariableA(name, out, cap);
    SecureZeroMemory(name, sizeof(name));
    return r;
}

DWORD GetEnvW(const unsigned char* encAscii, size_t n, wchar_t* out, DWORD cap) {
    if (!encAscii) return 0;
    wchar_t name[kMax]{};
    DecodeW(encAscii, n, name, kMax);
    const DWORD r = GetEnvironmentVariableW(name, out, cap);
    SecureZeroMemory(name, sizeof(name));
    return r;
}

bool SetEnvA(const unsigned char* enc, size_t n, const char* value) {
    if (!enc) return false;
    char name[kMax]{};
    DecodeA(enc, n, name, sizeof(name));
    const BOOL ok = SetEnvironmentVariableA(name, value);
    SecureZeroMemory(name, sizeof(name));
    return ok != 0;
}

bool EnvOn(const unsigned char* enc, size_t n) {
    char buf[16]{};
    const DWORD got = GetEnvA(enc, n, buf, sizeof(buf));
    if (!got || got >= sizeof(buf)) return false;
    return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T';
}

bool EnvOff(const unsigned char* enc, size_t n) {
    char buf[16]{};
    const DWORD got = GetEnvA(enc, n, buf, sizeof(buf));
    if (!got || got >= sizeof(buf)) return false;
    return buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' || buf[0] == 'f' || buf[0] == 'F';
}

bool EnvNonZero(const unsigned char* enc, size_t n) {
    char buf[16]{};
    const DWORD got = GetEnvA(enc, n, buf, sizeof(buf));
    if (!got || got >= sizeof(buf)) return false;
    return buf[0] != '\0' && buf[0] != '0';
}

}  // namespace xcat::xor_cstr
