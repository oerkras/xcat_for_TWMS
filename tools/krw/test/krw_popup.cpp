// Simple MessageBox smoke for XCatKrw compat (ETW-only, no Device).
#include "../client/xcat_krw_compat.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

void Popup(bool ok, const std::string& body) {
  MessageBoxA(nullptr, body.c_str(), ok ? "XCatKrw OK" : "XCatKrw FAIL",
              MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR) | MB_TOPMOST);
}

std::string RunOnce(bool use_phys) {
  if (!Init(0x7654321)) {
    return "Init failed:\nETW covert channel not ready\n(no Device/IOCTL fallback)";
  }

  volatile unsigned char marker[32];
  for (int i = 0; i < 32; ++i) marker[i] = static_cast<unsigned char>(0xC0 + i);
  unsigned char got[32]{};
  DWORD n = 0;
  const DWORD pid = GetCurrentProcessId();
  const bool ok =
      Read(pid, reinterpret_cast<UINT64>(const_cast<unsigned char*>(marker)),
           reinterpret_cast<UINT64>(got), sizeof(got), n, use_phys ? TRUE : FALSE) &&
      n == sizeof(got) && std::memcmp(got, const_cast<unsigned char*>(marker), sizeof(got)) == 0;
  UnInit();

  char buf[256]{};
  std::snprintf(buf, sizeof(buf), "pid=%lu path=%s\nread %s (%lu bytes)",
                static_cast<unsigned long>(pid), use_phys ? "phys" : "cr3",
                ok ? "OK" : "FAIL", static_cast<unsigned long>(n));
  return ok ? std::string(buf) : (std::string("Read failed:\n") + buf);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::string a = RunOnce(false);
  const bool ok_a = a.find("OK") != std::string::npos && a.find("FAIL") == std::string::npos;
  Popup(ok_a, a);
  if (!ok_a) return 1;
  const std::string b = RunOnce(true);
  const bool ok_b = b.find("OK") != std::string::npos;
  Popup(ok_b, b);
  return ok_b ? 0 : 2;
}
