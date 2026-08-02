#pragma once

// PTE-window physical RW, algorithm aligned with KraSlayer RW_Driver
// Anti4heatExpert (read-only reference). No VMP. No ETW / inject.

#include <ntifs.h>

namespace xcat_krw {
namespace phys_pte {

struct PhysicalPageInfo {
  PVOID BaseAddress;
  SIZE_T Size;
  PVOID PteAddress;
};

struct PageTableInfo {
  ULONG64 Pxe;
  ULONG64 Ppe;
  ULONG64 Pde;
  ULONG64 Pte;
  ULONG PageType;  // 5=4K, 6=2M, 7=1G
};

// 0 = success; non-zero = Anti4heat-style error codes.
ULONG InitializePteBase();

ULONG ProcessVirtualMemory(ULONG64 directory_table_base, PVOID address, PVOID buffer,
                           PULONG size_inout, BOOLEAN read);

}  // namespace phys_pte
}  // namespace xcat_krw
