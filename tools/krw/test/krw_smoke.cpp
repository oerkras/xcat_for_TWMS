// Smoke (ETW-only): Init(0x7654321) + self Read via covert channel. No Device/IOCTL.
#include "../client/xcat_krw_compat.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  const bool use_phys = (argc >= 2 && std::strcmp(argv[1], "--phys") == 0);
  if (!Init(0x7654321)) {
    std::printf("Init failed (need ETW-ready xcat_krw.sys; no IOCTL fallback)\n");
    return 1;
  }

  unsigned char marker[32]{};
  for (int i = 0; i < 32; ++i) marker[i] = static_cast<unsigned char>(0xA0 + i);
  unsigned char got[32]{};
  DWORD n = 0;
  const DWORD pid = GetCurrentProcessId();
  if (!Read(pid, reinterpret_cast<UINT64>(marker), reinterpret_cast<UINT64>(got), sizeof(got), n,
            use_phys ? TRUE : FALSE)) {
    std::printf("Read failed (path=%s)\n", use_phys ? "phys" : "cr3+0x28");
    UnInit();
    return 2;
  }
  if (n != sizeof(got) || std::memcmp(got, marker, sizeof(got)) != 0) {
    std::printf("Read mismatch n=%lu\n", static_cast<unsigned long>(n));
    UnInit();
    return 3;
  }
  std::printf("%s OK (read %lu bytes)\n", use_phys ? "phys" : "compat",
              static_cast<unsigned long>(n));
  UnInit();
  std::printf("smoke OK\n");
  return 0;
}
