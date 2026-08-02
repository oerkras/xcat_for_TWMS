#include "covert_ipc.hpp"

#include "inject/inj.h"
#include "phys_pte.hpp"

#include <intrin.h>
#include <ntstrsafe.h>

#include <VMProtectDDK.h>

namespace xcat_krw {
namespace covert {
namespace {

ULONG g_client_count = 0;
NtCreateFileFn g_original_nt_create_file = nullptr;

ULONG64 ReadCr3(PEPROCESS process, BOOLEAN real_cr3) {
  if (real_cr3) {
    KAPC_STATE apc{};
    KeStackAttachProcess(process, &apc);
    const ULONG64 cr3 = __readcr3();
    KeUnstackDetachProcess(&apc);
    return cr3;
  }
  return *reinterpret_cast<PULONG_PTR>(reinterpret_cast<ULONG_PTR>(process) + 0x28);
}

}  // namespace

void SetOriginalNtCreateFile(void* original) {
  g_original_nt_create_file = reinterpret_cast<NtCreateFileFn>(original);
}

NTSTATUS DetourNtCreateFile(_Out_ PHANDLE FileHandle, _In_ ACCESS_MASK DesiredAccess,
                            _In_ POBJECT_ATTRIBUTES ObjectAttributes,
                            _Out_ PIO_STATUS_BLOCK IoStatusBlock,
                            _In_opt_ PLARGE_INTEGER AllocationSize, _In_ ULONG FileAttributes,
                            _In_ ULONG ShareAccess, _In_ ULONG CreateDisposition,
                            _In_ ULONG CreateOptions, _In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
                            _In_ ULONG EaLength) {
  VM_BEGIN();
  do {
    if (DesiredAccess < Type_Init || DesiredAccess >= Type_Max) {
      break;
    }

    if (!ObjectAttributes || ObjectAttributes->Attributes != kClientKey) {
      return STATUS_ACCESS_DENIED;
    }

    if (DesiredAccess == Type_Init) {
      InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_client_count));
      return STATUS_SUCCESS;
    }
    if (DesiredAccess == Type_Unint) {
      if (g_client_count) {
        InterlockedDecrement(reinterpret_cast<volatile LONG*>(&g_client_count));
      }
      return STATUS_SUCCESS;
    }

    if (!FileAttributes) {
      break;
    }

    const HANDLE pid = ULongToHandle(FileAttributes);

    if (DesiredAccess == Type_Inject) {
      if (!EaBuffer) {
        return STATUS_INVALID_PARAMETER;
      }
      ANSI_STRING ansi{};
      RtlInitAnsiString(&ansi, static_cast<PCSZ>(EaBuffer));
      UNICODE_STRING module_path{};
      NTSTATUS st = RtlAnsiStringToUnicodeString(&module_path, &ansi, TRUE);
      if (!NT_SUCCESS(st)) {
        return STATUS_UNSUCCESSFUL;
      }
      st = InjPerformInjection(pid, &module_path);
      RtlFreeUnicodeString(&module_path);
      return st;
    }

    if (DesiredAccess == Type_Read || DesiredAccess == Type_Write) {
      PEPROCESS process = nullptr;
      NTSTATUS st = PsLookupProcessByProcessId(pid, &process);
      if (!NT_SUCCESS(st) || !process) {
        return STATUS_UNSUCCESSFUL;
      }

      const BOOLEAN real_cr3 = EaBuffer != nullptr;
      const ULONG64 cr3 = ReadCr3(process, real_cr3);
      ULONG size_read = EaLength;
      const UINT64 address =
          (static_cast<UINT64>(CreateDisposition) << 32) | CreateOptions;
      const UINT64 buffer =
          (static_cast<UINT64>(HandleToULong(ObjectAttributes->RootDirectory)) << 32) |
          ShareAccess;

      const BOOLEAN is_read = (DesiredAccess == Type_Read);
      const ULONG err = phys_pte::ProcessVirtualMemory(
          cr3, reinterpret_cast<PVOID>(address), reinterpret_cast<PVOID>(buffer), &size_read,
          is_read ? TRUE : FALSE);

      ObDereferenceObject(process);

      if (err == 0) {
        if (FileHandle) {
          *FileHandle = ULongToHandle(size_read);
        }
        return STATUS_SUCCESS;
      }
      return STATUS_UNSUCCESSFUL;
    }
  } while (0);

  if (!g_original_nt_create_file) {
    UNICODE_STRING n = RTL_CONSTANT_STRING(L"NtCreateFile");
    g_original_nt_create_file =
        reinterpret_cast<NtCreateFileFn>(MmGetSystemRoutineAddress(&n));
  }
  if (!g_original_nt_create_file) {
    return STATUS_INVALID_DEVICE_STATE;
  }
  return g_original_nt_create_file(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock,
                                   AllocationSize, FileAttributes, ShareAccess, CreateDisposition,
                                   CreateOptions, EaBuffer, EaLength);
  VM_END();
}

}  // namespace covert
}  // namespace xcat_krw
