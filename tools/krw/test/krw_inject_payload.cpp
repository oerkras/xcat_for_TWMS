// Lab inject payload: creates %TEMP%\xcat_krw_inject.ok on load.
#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    wchar_t path[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, path) > 0) {
      wcscat_s(path, L"xcat_krw_inject.ok");
      HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
      if (f != INVALID_HANDLE_VALUE) {
        const char msg[] = "injected\n";
        DWORD w = 0;
        WriteFile(f, msg, sizeof(msg) - 1, &w, nullptr);
        CloseHandle(f);
      }
    }
  }
  return TRUE;
}
