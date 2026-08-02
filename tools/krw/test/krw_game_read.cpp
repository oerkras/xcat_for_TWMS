// Cross-process game READ smoke via XCatKrw (ETW-only). No XCat payload / no WM logs.
// PID by process name snapshot only. Module bases + all memory bytes via KRW (PEB/LDR).
// No Toolhelp SNAPMODULE.
#include "../client/xcat_krw_compat.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <winternl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using NtQueryInformationProcess_t = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG,
                                                     PULONG);

constexpr std::uint64_t kPebImageBase = 0x10;
constexpr std::uint64_t kPebLdr = 0x18;
constexpr std::uint64_t kLdrInMemoryOrder = 0x20;
constexpr std::uint64_t kLdrEntryDllBase = 0x30;
constexpr std::uint64_t kLdrEntryInMemory = 0x10;
constexpr std::uint64_t kLdrEntryBaseDllName = 0x58;

bool EnableDebugPrivilege() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
    return false;
  }
  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
    CloseHandle(tok);
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  CloseHandle(tok);
  return ok && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

std::uint32_t FindPidByExe(const wchar_t* exe) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  std::uint32_t pid = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, exe) == 0) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return pid;
}

bool KrwRead(DWORD pid, UINT64 addr, void* buf, DWORD size, bool use_phys) {
  DWORD n = 0;
  return Read(pid, addr, reinterpret_cast<UINT64>(buf), size, n, use_phys ? TRUE : FALSE) &&
         n == size;
}

std::uint64_t QueryPeb(DWORD pid) {
  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) {
    std::printf("OpenProcess(QUERY_LIMITED) err=%lu\n", GetLastError());
    return 0;
  }
  PROCESS_BASIC_INFORMATION pbi{};
  auto NtQip = reinterpret_cast<NtQueryInformationProcess_t>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  ULONG ret = 0;
  const NTSTATUS st =
      NtQip ? NtQip(proc, ProcessBasicInformation, &pbi, sizeof(pbi), &ret) : static_cast<NTSTATUS>(-1);
  CloseHandle(proc);
  if (st < 0 || !pbi.PebBaseAddress) {
    std::printf("NtQueryInformationProcess PEB failed status=0x%08lX\n",
                static_cast<unsigned long>(st));
    return 0;
  }
  return reinterpret_cast<std::uint64_t>(pbi.PebBaseAddress);
}

bool ReadRemoteU16String(DWORD pid, std::uint64_t us_addr, wchar_t* out, size_t out_chars,
                         bool use_phys) {
  struct {
    std::uint16_t Length;
    std::uint16_t MaximumLength;
    std::uint32_t _pad;
    std::uint64_t Buffer;
  } us{};
  if (!KrwRead(pid, us_addr, &us, sizeof(us), use_phys) || !us.Buffer || us.Length == 0) return false;
  const size_t bytes = us.Length;
  if (bytes + sizeof(wchar_t) > out_chars * sizeof(wchar_t)) return false;
  if (!KrwRead(pid, us.Buffer, out, static_cast<DWORD>(bytes), use_phys)) return false;
  out[bytes / sizeof(wchar_t)] = L'\0';
  return true;
}

struct ModBases {
  std::uint64_t exe = 0;
  std::uint64_t ga = 0;
};

ModBases FindModulesViaPeb(DWORD pid, bool use_phys) {
  ModBases out{};
  const std::uint64_t peb = QueryPeb(pid);
  if (!peb) return out;
  std::printf("PEB=0x%llx\n", static_cast<unsigned long long>(peb));

  std::uint64_t image_base = 0;
  if (KrwRead(pid, peb + kPebImageBase, &image_base, sizeof(image_base), use_phys) && image_base) {
    out.exe = image_base;
    std::printf("PEB.ImageBaseAddress=0x%llx (KRW)\n", static_cast<unsigned long long>(image_base));
  }

  std::uint64_t ldr = 0;
  if (!KrwRead(pid, peb + kPebLdr, &ldr, sizeof(ldr), use_phys) || ldr < 0x10000) {
    std::printf("PEB.Ldr read FAIL\n");
    return out;
  }

  std::uint64_t list_head = ldr + kLdrInMemoryOrder;
  std::uint64_t flink = 0;
  if (!KrwRead(pid, list_head, &flink, sizeof(flink), use_phys) || !flink) {
    std::printf("InMemoryOrderModuleList FAIL\n");
    return out;
  }

  int walked = 0;
  for (std::uint64_t link = flink; link && link != list_head && walked < 512; ++walked) {
    const std::uint64_t entry = link - kLdrEntryInMemory;
    std::uint64_t dll_base = 0;
    if (!KrwRead(pid, entry + kLdrEntryDllBase, &dll_base, sizeof(dll_base), use_phys)) break;

    wchar_t name[260]{};
    if (ReadRemoteU16String(pid, entry + kLdrEntryBaseDllName, name, 260, use_phys)) {
      if (!out.exe && _wcsicmp(name, L"Maplestory_Classic.exe") == 0) out.exe = dll_base;
      if (_wcsicmp(name, L"GameAssembly.dll") == 0) out.ga = dll_base;
      if (out.exe && out.ga) break;
    }

    std::uint64_t next = 0;
    if (!KrwRead(pid, link, &next, sizeof(next), use_phys) || !next || next == link) break;
    link = next;
  }
  std::printf("LDR walked=%d (KRW)\n", walked);
  return out;
}

bool CheckPeMz(DWORD pid, UINT64 base, const char* label, bool use_phys) {
  unsigned char hdr[0x40]{};
  if (!KrwRead(pid, base, hdr, sizeof(hdr), use_phys)) {
    std::printf("[FAIL] %s read base 0x%llx\n", label, static_cast<unsigned long long>(base));
    return false;
  }
  if (hdr[0] != 'M' || hdr[1] != 'Z') {
    std::printf("[FAIL] %s MZ mismatch @0x%llx bytes=%02X %02X\n", label,
                static_cast<unsigned long long>(base), hdr[0], hdr[1]);
    return false;
  }
  const auto e_lfanew = *reinterpret_cast<std::uint32_t*>(hdr + 0x3C);
  unsigned char pe[4]{};
  if (!KrwRead(pid, base + e_lfanew, pe, sizeof(pe), use_phys) || pe[0] != 'P' || pe[1] != 'E' ||
      pe[2] != 0 || pe[3] != 0) {
    std::printf("[FAIL] %s PE signature @0x%llx+0x%X\n", label,
                static_cast<unsigned long long>(base), e_lfanew);
    return false;
  }
  std::printf("[OK] %s base=0x%llx MZ+PE (e_lfanew=0x%X) path=%s\n", label,
              static_cast<unsigned long long>(base), e_lfanew, use_phys ? "phys" : "cr3");
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool use_phys = false;
  std::uint32_t pid = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--phys") == 0) {
      use_phys = true;
    } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
      pid = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    } else if (argv[i][0] != '-') {
      pid = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 0));
    } else {
      std::printf(
          "Usage: krw_game_read.exe [--phys] [--pid N]\n"
          "  No XCat. Module bases via KRW PEB/LDR; PE bytes via KRW.\n");
      return 1;
    }
  }

  EnableDebugPrivilege();

  if (pid == 0) pid = FindPidByExe(L"Maplestory_Classic.exe");
  if (pid == 0) {
    std::printf("Maplestory_Classic.exe not running\n");
    return 1;
  }
  std::printf("pid=%u\n", pid);

  if (!Init(0x7654321)) {
    std::printf("Init failed (ETW covert required)\n");
    return 3;
  }

  const ModBases mods = FindModulesViaPeb(pid, use_phys);
  if (!mods.exe) {
    std::printf("Maplestory_Classic.exe base unknown\n");
    UnInit();
    return 2;
  }
  if (mods.ga) {
    std::printf("module GameAssembly.dll=0x%llx\n", static_cast<unsigned long long>(mods.ga));
  } else {
    std::printf("module GameAssembly.dll=MISSING\n");
  }

  int fails = 0;
  if (!CheckPeMz(pid, mods.exe, "Maplestory_Classic.exe", use_phys)) ++fails;
  if (mods.ga && !CheckPeMz(pid, mods.ga, "GameAssembly.dll", use_phys)) ++fails;

  unsigned char sample[16]{};
  if (KrwRead(pid, mods.exe + 0x1000, sample, sizeof(sample), use_phys)) {
    std::printf("sample exe+0x1000:");
    for (unsigned char b : sample) std::printf(" %02X", b);
    std::printf("\n");
  } else {
    std::printf("[WARN] sample exe+0x1000 read failed\n");
  }

  UnInit();
  if (fails) {
    std::printf("game_read FAIL (%d)\n", fails);
    return 4;
  }
  std::printf("game_read OK (no XCat; mem=KRW)\n");
  return 0;
}
