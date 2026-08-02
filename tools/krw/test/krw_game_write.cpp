// Cross-process game WRITE smoke via XCatKrw only (ETW-only). No XCat payload.
// PID lookup uses process snapshot (name only). All memory R/W goes through KRW.
// Module bases: PEB/LDR via KRW (no Toolhelp SNAPMODULE).
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

constexpr std::uint64_t kPebBeingDebugged = 0x02;
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

bool KrwWrite(DWORD pid, UINT64 addr, const void* buf, DWORD size, bool use_phys) {
  DWORD n = 0;
  return Write(pid, addr, reinterpret_cast<UINT64>(const_cast<void*>(buf)), size, n,
               use_phys ? TRUE : FALSE) &&
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

std::uint64_t FindModuleViaPeb(DWORD pid, const wchar_t* want, bool use_phys) {
  const std::uint64_t peb = QueryPeb(pid);
  if (!peb) return 0;

  if (_wcsicmp(want, L"Maplestory_Classic.exe") == 0) {
    std::uint64_t image_base = 0;
    if (KrwRead(pid, peb + kPebImageBase, &image_base, sizeof(image_base), use_phys) && image_base)
      return image_base;
  }

  std::uint64_t ldr = 0;
  if (!KrwRead(pid, peb + kPebLdr, &ldr, sizeof(ldr), use_phys) || ldr < 0x10000) return 0;

  const std::uint64_t list_head = ldr + kLdrInMemoryOrder;
  std::uint64_t flink = 0;
  if (!KrwRead(pid, list_head, &flink, sizeof(flink), use_phys) || !flink) return 0;

  int walked = 0;
  for (std::uint64_t link = flink; link && link != list_head && walked < 512; ++walked) {
    const std::uint64_t entry = link - kLdrEntryInMemory;
    std::uint64_t dll_base = 0;
    if (!KrwRead(pid, entry + kLdrEntryDllBase, &dll_base, sizeof(dll_base), use_phys)) break;
    wchar_t name[260]{};
    if (ReadRemoteU16String(pid, entry + kLdrEntryBaseDllName, name, 260, use_phys)) {
      if (_wcsicmp(name, want) == 0) return dll_base;
    }
    std::uint64_t next = 0;
    if (!KrwRead(pid, link, &next, sizeof(next), use_phys) || !next || next == link) break;
    link = next;
  }
  return 0;
}

// Safe write target: PEB.BeingDebugged (1 byte). Restore always.
bool TestWriteBeingDebugged(DWORD pid, std::uint64_t peb, bool use_phys) {
  const UINT64 addr = peb + kPebBeingDebugged;
  std::uint8_t orig = 0;
  if (!KrwRead(pid, addr, &orig, 1, use_phys)) {
    std::printf("[FAIL] read BeingDebugged\n");
    return false;
  }
  const std::uint8_t marker = static_cast<std::uint8_t>(orig ^ 0xA5);
  if (!KrwWrite(pid, addr, &marker, 1, use_phys)) {
    std::printf("[FAIL] write BeingDebugged\n");
    return false;
  }
  std::uint8_t got = 0;
  if (!KrwRead(pid, addr, &got, 1, use_phys) || got != marker) {
    std::printf("[FAIL] verify BeingDebugged got=0x%02X want=0x%02X\n", got, marker);
    KrwWrite(pid, addr, &orig, 1, use_phys);
    return false;
  }
  if (!KrwWrite(pid, addr, &orig, 1, use_phys)) {
    std::printf("[FAIL] restore BeingDebugged\n");
    return false;
  }
  std::printf("[OK] PEB.BeingDebugged write/verify/restore (orig=0x%02X) path=%s\n", orig,
              use_phys ? "phys" : "cr3");
  return true;
}

// Safe write #2: find a writable PE section via KRW (already resident), patch 16 bytes, restore.
bool FindWritableVa(DWORD pid, std::uint64_t image_base, std::uint64_t* out_va, bool use_phys) {
  unsigned char dos[0x40]{};
  if (!KrwRead(pid, image_base, dos, sizeof(dos), use_phys) || dos[0] != 'M' || dos[1] != 'Z') {
    return false;
  }
  const auto e_lfanew = *reinterpret_cast<std::uint32_t*>(dos + 0x3C);
  unsigned char pe_hdr[0x18 + 0xF0]{};  // signature + file header + optional (pe32+)
  if (!KrwRead(pid, image_base + e_lfanew, pe_hdr, sizeof(pe_hdr), use_phys) || pe_hdr[0] != 'P' ||
      pe_hdr[1] != 'E') {
    return false;
  }
  const auto size_opt = *reinterpret_cast<std::uint16_t*>(pe_hdr + 0x14);  // SizeOfOptionalHeader
  const auto num_sec = *reinterpret_cast<std::uint16_t*>(pe_hdr + 0x06);
  const std::uint64_t sec_table = image_base + e_lfanew + 0x18 + size_opt;
  for (std::uint16_t i = 0; i < num_sec && i < 96; ++i) {
    unsigned char sec[40]{};
    if (!KrwRead(pid, sec_table + static_cast<std::uint64_t>(i) * 40, sec, 40, use_phys)) {
      return false;
    }
    const auto vsize = *reinterpret_cast<std::uint32_t*>(sec + 0x08);
    const auto va = *reinterpret_cast<std::uint32_t*>(sec + 0x0C);
    const auto chars = *reinterpret_cast<std::uint32_t*>(sec + 0x24);
    constexpr std::uint32_t kMemWrite = 0x80000000u;
    constexpr std::uint32_t kMemRead = 0x40000000u;
    if ((chars & kMemWrite) && (chars & kMemRead) && vsize >= 0x40 && va != 0) {
      // Skip section head; use a mid-page slot already faulted by the loader.
      *out_va = image_base + va + 0x20;
      return true;
    }
  }
  return false;
}

bool TestWritePeSection(DWORD pid, std::uint64_t image_base, const char* label, bool use_phys) {
  std::uint64_t va = 0;
  if (!FindWritableVa(pid, image_base, &va, use_phys)) {
    std::printf("[FAIL] %s: no writable PE section\n", label);
    return false;
  }
  unsigned char orig[16]{};
  unsigned char pat[16]{};
  unsigned char got[16]{};
  for (int i = 0; i < 16; ++i) pat[i] = static_cast<unsigned char>(0x5A ^ i);
  if (!KrwRead(pid, va, orig, sizeof(orig), use_phys)) {
    std::printf("[FAIL] %s: KRW read writable @0x%llx\n", label,
                static_cast<unsigned long long>(va));
    return false;
  }
  if (!KrwWrite(pid, va, pat, sizeof(pat), use_phys)) {
    std::printf("[FAIL] %s: KRW write @0x%llx\n", label, static_cast<unsigned long long>(va));
    return false;
  }
  if (!KrwRead(pid, va, got, sizeof(got), use_phys) || std::memcmp(got, pat, sizeof(pat)) != 0) {
    std::printf("[FAIL] %s: verify mismatch @0x%llx\n", label, static_cast<unsigned long long>(va));
    KrwWrite(pid, va, orig, sizeof(orig), use_phys);
    return false;
  }
  if (!KrwWrite(pid, va, orig, sizeof(orig), use_phys)) {
    std::printf("[FAIL] %s: restore @0x%llx\n", label, static_cast<unsigned long long>(va));
    return false;
  }
  std::printf("[OK] %s writable PE @0x%llx write/verify/restore path=%s\n", label,
              static_cast<unsigned long long>(va), use_phys ? "phys" : "cr3");
  return true;
}

bool CheckPeMz(DWORD pid, UINT64 base, const char* label, bool use_phys) {
  unsigned char hdr[0x40]{};
  if (!KrwRead(pid, base, hdr, sizeof(hdr), use_phys)) {
    std::printf("[FAIL] %s read base 0x%llx\n", label, static_cast<unsigned long long>(base));
    return false;
  }
  if (hdr[0] != 'M' || hdr[1] != 'Z') {
    std::printf("[FAIL] %s MZ mismatch\n", label);
    return false;
  }
  const auto e_lfanew = *reinterpret_cast<std::uint32_t*>(hdr + 0x3C);
  unsigned char pe[4]{};
  if (!KrwRead(pid, base + e_lfanew, pe, sizeof(pe), use_phys) || pe[0] != 'P' || pe[1] != 'E') {
    std::printf("[FAIL] %s PE signature\n", label);
    return false;
  }
  std::printf("[OK] %s base=0x%llx MZ+PE path=%s\n", label, static_cast<unsigned long long>(base),
              use_phys ? "phys" : "cr3");
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
          "Usage: krw_game_write.exe [--phys] [--pid N]\n"
          "  No XCat. KRW write: PEB.BeingDebugged + writable PE section (no VirtualAllocEx).\n");
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

  const std::uint64_t peb = QueryPeb(pid);
  if (!peb) {
    UnInit();
    return 2;
  }
  std::printf("PEB=0x%llx (addr from NtQuery; bytes via KRW)\n",
              static_cast<unsigned long long>(peb));

  const std::uint64_t exe = FindModuleViaPeb(pid, L"Maplestory_Classic.exe", use_phys);
  const std::uint64_t ga = FindModuleViaPeb(pid, L"GameAssembly.dll", use_phys);
  int fails = 0;
  if (exe) {
    if (!CheckPeMz(pid, exe, "Maplestory_Classic.exe", use_phys)) ++fails;
  } else {
    std::printf("[FAIL] exe base via PEB/LDR\n");
    ++fails;
  }
  if (ga) {
    if (!CheckPeMz(pid, ga, "GameAssembly.dll", use_phys)) ++fails;
  } else {
    std::printf("[WARN] GameAssembly.dll not found in LDR\n");
  }

  if (!TestWriteBeingDebugged(pid, peb, use_phys)) ++fails;
  // Prefer GameAssembly .data (loader-faulted). Avoid VirtualAllocEx: demand-zero PTE
  // is often not present, so phys walk write fails until the page is touched.
  if (ga) {
    if (!TestWritePeSection(pid, ga, "GameAssembly.dll", use_phys)) ++fails;
  } else if (exe) {
    if (!TestWritePeSection(pid, exe, "Maplestory_Classic.exe", use_phys)) ++fails;
  } else {
    std::printf("[FAIL] no image base for PE write test\n");
    ++fails;
  }

  UnInit();
  if (fails) {
    std::printf("game_write FAIL (%d)\n", fails);
    return 4;
  }
  std::printf("game_write OK (no XCat; mem ops=KRW)\n");
  return 0;
}
