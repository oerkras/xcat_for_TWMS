#include "xcat_krw_compat.h"

#include <winternl.h>

#include <rpc.h>

#include <string>

#pragma comment(lib, "ntdll.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Rpcrt4.lib")

namespace {

enum IoType : ULONG {
  Type_Init = 0xfffa,
  Type_Unint = 0xfffb,
  Type_Read = 0xfffc,
  Type_Write = 0xfffd,
  Type_Inject = 0xfffe,
};

using P_NtCreateFile = NTSTATUS(WINAPI*)(
    _Out_ PHANDLE FileHandle, _In_ ACCESS_MASK DesiredAccess, _In_ POBJECT_ATTRIBUTES ObjectAttributes,
    _Out_ PIO_STATUS_BLOCK IoStatusBlock, _In_opt_ PLARGE_INTEGER AllocationSize,
    _In_ ULONG FileAttributes, _In_ ULONG ShareAccess, _In_ ULONG CreateDisposition,
    _In_ ULONG CreateOptions, _In_reads_bytes_opt_(EaLength) PVOID EaBuffer, _In_ ULONG EaLength);

P_NtCreateFile g_NtCreateFile = nullptr;
DWORD g_key = 0;

HMODULE SelfModule() {
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&Init), &mod);
  return mod;
}

std::wstring ModuleDir() {
  wchar_t path[MAX_PATH]{};
  HMODULE mod = SelfModule();
  const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring s(path);
  const auto slash = s.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return s.substr(0, slash + 1);
}

std::wstring GenerateUniqueServiceName() {
  UUID uuid{};
  if (UuidCreate(&uuid) != RPC_S_OK) return L"SVC_XCatKrwFallback";
  RPC_WSTR str = nullptr;
  if (UuidToStringW(&uuid, &str) != RPC_S_OK || !str) return L"SVC_XCatKrwFallback";
  std::wstring name = L"SVC_";
  name += reinterpret_cast<wchar_t*>(str);
  RpcStringFreeW(&str);
  return name;
}

DWORD LoadDriverUuid() {
  SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
  if (!scm) return GetLastError();

  std::wstring sys = ModuleDir() + L"xcat_krw.sys";
  if (GetFileAttributesW(sys.c_str()) == INVALID_FILE_ATTRIBUTES) {
    CloseServiceHandle(scm);
    return ERROR_FILE_NOT_FOUND;
  }

  const std::wstring service_name = GenerateUniqueServiceName();
  SC_HANDLE svc =
      CreateServiceW(scm, service_name.c_str(), service_name.c_str(), SERVICE_ALL_ACCESS,
                     SERVICE_KERNEL_DRIVER, SERVICE_DEMAND_START, SERVICE_ERROR_IGNORE, sys.c_str(),
                     nullptr, nullptr, nullptr, nullptr, nullptr);
  if (!svc) {
    const DWORD err = GetLastError();
    CloseServiceHandle(scm);
    return err;
  }
  if (!StartServiceW(svc, 0, nullptr)) {
    const DWORD err = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return (err == ERROR_SERVICE_ALREADY_RUNNING) ? ERROR_SUCCESS : err;
  }
  CloseServiceHandle(svc);
  CloseServiceHandle(scm);
  return ERROR_SUCCESS;
}

NTSTATUS CallEtwInit() {
  HANDLE fh = nullptr;
  OBJECT_ATTRIBUTES oa{};
  InitializeObjectAttributes(&oa, nullptr, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  oa.Attributes = g_key;
  // Prefer a real IOSB so an unhooked NtCreateFile does not AV on nullptr.
  IO_STATUS_BLOCK iosb{};
  return g_NtCreateFile(&fh, Type_Init, &oa, &iosb, nullptr, 0, 0, 0, 0, nullptr, 0);
}

}  // namespace

BOOLEAN Init(DWORD Key) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return FALSE;
  g_NtCreateFile = reinterpret_cast<P_NtCreateFile>(GetProcAddress(ntdll, "NtCreateFile"));
  if (!g_NtCreateFile || Key == 0) return FALSE;
  g_key = Key;

  if (CallEtwInit() == 0) return TRUE;
  if (LoadDriverUuid() != ERROR_SUCCESS) return FALSE;
  return CallEtwInit() == 0 ? TRUE : FALSE;
}

VOID UnInit() {
  if (!g_NtCreateFile) return;
  HANDLE fh = nullptr;
  OBJECT_ATTRIBUTES oa{};
  InitializeObjectAttributes(&oa, nullptr, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  oa.Attributes = g_key;
  IO_STATUS_BLOCK iosb{};
  g_NtCreateFile(&fh, Type_Unint, &oa, &iosb, nullptr, 0, 0, 0, 0, nullptr, 0);
}

BOOLEAN Read(DWORD Pid, UINT64 Address, UINT64 Buffer, DWORD NumberOfBytesToRead,
             DWORD& NumberOfBytesReaded, BOOLEAN RealCr3) {
  if (!g_NtCreateFile) return FALSE;
  HANDLE fh = nullptr;
  IO_STATUS_BLOCK iosb{};
  OBJECT_ATTRIBUTES oa{};
  InitializeObjectAttributes(&oa, nullptr, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  oa.Attributes = g_key;
  oa.RootDirectory = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(Buffer >> 32));
  const ULONG share = static_cast<ULONG>(Buffer & 0xffffffffu);
  const ULONG disp = static_cast<ULONG>(Address >> 32);
  const ULONG opts = static_cast<ULONG>(Address & 0xffffffffu);
  const NTSTATUS st =
      g_NtCreateFile(&fh, Type_Read, &oa, &iosb, nullptr, Pid, share, disp, opts,
                     RealCr3 ? reinterpret_cast<PVOID>(1) : nullptr, NumberOfBytesToRead);
  if (st == 0) NumberOfBytesReaded = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(fh));
  return st == 0;
}

BOOLEAN Write(DWORD Pid, UINT64 Address, UINT64 Buffer, DWORD NumberOfBytesToWrite,
              DWORD& NumberOfBytesWritten, BOOLEAN RealCr3) {
  if (!g_NtCreateFile) return FALSE;
  HANDLE fh = nullptr;
  IO_STATUS_BLOCK iosb{};
  OBJECT_ATTRIBUTES oa{};
  InitializeObjectAttributes(&oa, nullptr, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  oa.Attributes = g_key;
  oa.RootDirectory = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(Buffer >> 32));
  const ULONG share = static_cast<ULONG>(Buffer & 0xffffffffu);
  const ULONG disp = static_cast<ULONG>(Address >> 32);
  const ULONG opts = static_cast<ULONG>(Address & 0xffffffffu);
  const NTSTATUS st =
      g_NtCreateFile(&fh, Type_Write, &oa, &iosb, nullptr, Pid, share, disp, opts,
                     RealCr3 ? reinterpret_cast<PVOID>(1) : nullptr, NumberOfBytesToWrite);
  if (st == 0) NumberOfBytesWritten = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(fh));
  return st == 0;
}

BOOLEAN Inject(DWORD Pid, PCHAR DllPath) {
  if (!DllPath || !g_NtCreateFile) return FALSE;
  HANDLE fh = nullptr;
  OBJECT_ATTRIBUTES oa{};
  InitializeObjectAttributes(&oa, nullptr, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
  oa.Attributes = g_key;
  IO_STATUS_BLOCK iosb{};
  const NTSTATUS st =
      g_NtCreateFile(&fh, Type_Inject, &oa, &iosb, nullptr, Pid, 0, 0, 0, DllPath, 0);
  return st == 0;
}
