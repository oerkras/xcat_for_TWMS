#pragma once

#include <ntifs.h>

// Covert IPC via ETW-hooked NtCreateFile (RW_Driver Client ABI).
// Key must match usermode Init(0x7654321).

namespace xcat_krw {
namespace covert {

enum IoType : ULONG {
  Type_Init = 0xfffa,
  Type_Unint = 0xfffb,
  Type_Read = 0xfffc,
  Type_Write = 0xfffd,
  Type_Inject = 0xfffe,
  Type_Max = 0xffff,
};

constexpr ULONG kClientKey = 0x7654321u;

using NtCreateFileFn = NTSTATUS(NTAPI*)(_Out_ PHANDLE FileHandle, _In_ ACCESS_MASK DesiredAccess,
                                        _In_ POBJECT_ATTRIBUTES ObjectAttributes,
                                        _Out_ PIO_STATUS_BLOCK IoStatusBlock,
                                        _In_opt_ PLARGE_INTEGER AllocationSize,
                                        _In_ ULONG FileAttributes, _In_ ULONG ShareAccess,
                                        _In_ ULONG CreateDisposition, _In_ ULONG CreateOptions,
                                        _In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
                                        _In_ ULONG EaLength);

void SetOriginalNtCreateFile(void* original);

NTSTATUS DetourNtCreateFile(_Out_ PHANDLE FileHandle, _In_ ACCESS_MASK DesiredAccess,
                            _In_ POBJECT_ATTRIBUTES ObjectAttributes,
                            _Out_ PIO_STATUS_BLOCK IoStatusBlock,
                            _In_opt_ PLARGE_INTEGER AllocationSize, _In_ ULONG FileAttributes,
                            _In_ ULONG ShareAccess, _In_ ULONG CreateDisposition,
                            _In_ ULONG CreateOptions, _In_reads_bytes_opt_(EaLength) PVOID EaBuffer,
                            _In_ ULONG EaLength);

}  // namespace covert
}  // namespace xcat_krw
