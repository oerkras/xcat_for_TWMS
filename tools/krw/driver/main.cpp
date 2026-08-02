// tools/krw/driver - XCatKrw.sys (WDK)
// Stealth path (RW_Driver-aligned): ETW hook NtCreateFile only — no Device / IOCTL.

#include <ntifs.h>

#include "covert_ipc.hpp"
#include "phys_pte.hpp"

#include <etwhook_manager.hpp>

extern "C" {
  NTSTATUS NTAPI NtCreateFile(_Out_ PHANDLE FileHandle, _In_ ACCESS_MASK DesiredAccess,
                              _In_ POBJECT_ATTRIBUTES ObjectAttributes,
                              _Out_ PIO_STATUS_BLOCK IoStatusBlock,
                              _In_opt_ PLARGE_INTEGER AllocationSize, _In_ ULONG FileAttributes,
                              _In_ ULONG ShareAccess, _In_ ULONG CreateDisposition,
                              _In_ ULONG CreateOptions,
                              _In_reads_bytes_opt_(EaLength) PVOID EaBuffer, _In_ ULONG EaLength);
}

namespace {

void driver_unload(PDRIVER_OBJECT driver) {
  UNREFERENCED_PARAMETER(driver);
  if (auto* etw = EtwHookManager::get_instance()) {
    etw->destory();
  }
}

}  // namespace

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING registry_path) {
  UNREFERENCED_PARAMETER(registry_path);

  // Warm PTE self-map bases used by phys path (Anti4heat-style).
  (void)xcat_krw::phys_pte::InitializePteBase();

  driver->DriverUnload = driver_unload;

  kstd::Logger::init("XCatKrw", nullptr);

  auto* etw = EtwHookManager::get_instance();
  if (!etw) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  const NTSTATUS etw_st = etw->init();
  if (!NT_SUCCESS(etw_st)) {
    etw->destory();
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[XCatKrw] EtwHookManager::init failed %08X (fail-closed, no Device)\n", etw_st);
    return etw_st;
  }

  UNICODE_STRING nt_name = RTL_CONSTANT_STRING(L"NtCreateFile");
  void* nt_create = MmGetSystemRoutineAddress(&nt_name);
  if (!nt_create) {
    nt_create = reinterpret_cast<void*>(NtCreateFile);
  }
  xcat_krw::covert::SetOriginalNtCreateFile(nt_create);

  const NTSTATUS hook_st =
      etw->add_hook(nt_create, reinterpret_cast<void*>(&xcat_krw::covert::DetourNtCreateFile));
  if (!NT_SUCCESS(hook_st)) {
    etw->destory();
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[XCatKrw] add_hook NtCreateFile failed %08X\n", hook_st);
    return hook_st;
  }

  DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,
             "[XCatKrw] ETW-only IPC ready NtCreateFile=%p detour=%p\n", nt_create,
             &xcat_krw::covert::DetourNtCreateFile);
  return STATUS_SUCCESS;
}
