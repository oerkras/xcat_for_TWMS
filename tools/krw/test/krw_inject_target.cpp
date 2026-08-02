// Lab inject target: idle until marker or timeout.
#include <Windows.h>
#include <cstdio>

int main() {
  std::printf("inject_target pid=%lu waiting...\n", GetCurrentProcessId());
  fflush(stdout);
  for (int i = 0; i < 120; ++i) {
    wchar_t path[MAX_PATH]{};
    GetTempPathW(MAX_PATH, path);
    wcscat_s(path, L"xcat_krw_inject.ok");
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
      std::printf("marker seen\n");
      return 0;
    }
    Sleep(500);
  }
  std::printf("timeout\n");
  return 1;
}
