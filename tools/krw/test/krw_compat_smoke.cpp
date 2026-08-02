// Covert IPC smoke: Init(0x7654321) + self Read/Write via NtCreateFile protocol.
#include "../client/xcat_krw_compat.h"

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  const bool do_inject = (argc >= 2 && std::strcmp(argv[1], "--inject") == 0);

  if (!Init(0x7654321)) {
    std::printf("Init failed (is xcat_krw.sys loaded with ETW hook?)\n");
    return 1;
  }

  if (do_inject) {
    if (argc < 4) {
      std::printf("Usage: krw_compat_smoke.exe --inject <pid> <abs_dll_path>\n");
      UnInit();
      return 10;
    }
    const DWORD pid = static_cast<DWORD>(std::strtoul(argv[2], nullptr, 0));
    char* dll = argv[3];
    wchar_t marker[MAX_PATH]{};
    GetTempPathW(MAX_PATH, marker);
    wcscat_s(marker, L"xcat_krw_inject.ok");
    DeleteFileW(marker);
    if (!Inject(pid, dll)) {
      std::printf("Inject failed\n");
      UnInit();
      return 11;
    }
    std::printf("Inject issued pid=%lu dll=%s\n", static_cast<unsigned long>(pid), dll);
    for (int i = 0; i < 40; ++i) {
      if (GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES) {
        std::printf("inject marker OK\n");
        UnInit();
        return 0;
      }
      Sleep(250);
    }
    std::printf("inject marker timeout\n");
    UnInit();
    return 12;
  }

  unsigned char marker[32]{};
  for (int i = 0; i < 32; ++i) marker[i] = static_cast<unsigned char>(0xB0 + i);
  unsigned char got[32]{};
  DWORD n = 0;
  const DWORD pid = GetCurrentProcessId();
  if (!Read(pid, reinterpret_cast<UINT64>(marker), reinterpret_cast<UINT64>(got), sizeof(got), n,
            TRUE)) {
    std::printf("compat Read failed\n");
    UnInit();
    return 2;
  }
  if (n != sizeof(got) || std::memcmp(got, marker, sizeof(got)) != 0) {
    std::printf("compat Read mismatch n=%lu\n", static_cast<unsigned long>(n));
    UnInit();
    return 3;
  }
  std::printf("compat Read OK (%lu bytes)\n", static_cast<unsigned long>(n));

  unsigned char patch[4] = {0x11, 0x22, 0x33, 0x44};
  unsigned char verify[4]{};
  DWORD wn = 0;
  if (!Write(pid, reinterpret_cast<UINT64>(marker), reinterpret_cast<UINT64>(patch), sizeof(patch),
             wn, TRUE) ||
      wn != sizeof(patch)) {
    std::printf("compat Write failed\n");
    UnInit();
    return 4;
  }
  DWORD rn = 0;
  if (!Read(pid, reinterpret_cast<UINT64>(marker), reinterpret_cast<UINT64>(verify), sizeof(verify),
            rn, TRUE) ||
      rn != sizeof(verify) || std::memcmp(verify, patch, sizeof(patch)) != 0) {
    std::printf("compat Write verify failed\n");
    UnInit();
    return 5;
  }
  std::printf("compat Write OK\n");
  UnInit();
  std::printf("compat smoke OK\n");
  return 0;
}
