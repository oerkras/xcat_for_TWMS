#include "physmem.hpp"

#include "phys_pte.hpp"

#include <intrin.h>

namespace xcat_krw {
namespace {

ULONG64 ReadCr3Attached(PEPROCESS process) {
  KAPC_STATE apc{};
  KeStackAttachProcess(process, &apc);
  const ULONG64 cr3 = __readcr3();
  KeUnstackDetachProcess(&apc);
  return cr3;
}

}  // namespace

NTSTATUS PhysCopyVirtual(PEPROCESS target, PVOID remote_va, PVOID buffer, SIZE_T size, BOOLEAN write,
                         PSIZE_T transferred) {
  if (!target || !remote_va || !buffer || size == 0 || size > 0xFFFFFFFFu) {
    return STATUS_INVALID_PARAMETER;
  }
  if (KeGetCurrentIrql() > APC_LEVEL) {
    return STATUS_INVALID_DEVICE_STATE;
  }

  const ULONG64 cr3 = ReadCr3Attached(target);
  if (cr3 == 0) {
    return STATUS_UNSUCCESSFUL;
  }

  ULONG n = static_cast<ULONG>(size);
  const ULONG err =
      phys_pte::ProcessVirtualMemory(cr3, remote_va, buffer, &n, write ? FALSE : TRUE);
  if (transferred) {
    *transferred = n;
  }
  if (err != 0 || n == 0) {
    return STATUS_UNSUCCESSFUL;
  }
  if (n != size) {
    return STATUS_PARTIAL_COPY;
  }
  return STATUS_SUCCESS;
}

}  // namespace xcat_krw
