#include "phys_pte.hpp"

#include <intrin.h>

// Port of KraSlayer/Tools/RW_Driver/Driver/ReadPhys/Anti4heatExpert.cpp (capability A).
// Differences: no VMP; init flag only set on success; pool tag 'xkrw'.

namespace xcat_krw {
namespace phys_pte {
namespace {

ULONG64 g_PteBase = 0;
BOOLEAN g_IsInitPteBaseForSystem = FALSE;
PPHYSICAL_MEMORY_RANGE g_PhysicalMemoryRanges = nullptr;

constexpr ULONG kPoolTag = 'xkrw';

bool IsPhysPageInRange(ULONG64 phys_page_base, ULONG64 size) {
  if (!g_PhysicalMemoryRanges) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
      return false;
    }
    g_PhysicalMemoryRanges = MmGetPhysicalMemoryRanges();
  }
  if (!g_PhysicalMemoryRanges || size == 0) {
    return false;
  }
  const ULONG64 phys_end = phys_page_base + size - 1;
  for (int i = 0;; ++i) {
    const PHYSICAL_MEMORY_RANGE range = g_PhysicalMemoryRanges[i];
    if (!range.BaseAddress.QuadPart || !range.NumberOfBytes.QuadPart) {
      break;
    }
    const ULONG64 base = static_cast<ULONG64>(range.BaseAddress.QuadPart);
    const ULONG64 end = base + static_cast<ULONG64>(range.NumberOfBytes.QuadPart);
    if (phys_page_base >= base && phys_page_base < end && phys_end >= base && phys_end < end) {
      return true;
    }
  }
  return false;
}

bool IsVaPhysicalAddressValid(PVOID va) {
  return MmGetPhysicalAddress(va).QuadPart > 0x1000;
}

PVOID GetPml4Base(PHYSICAL_ADDRESS dtb) {
  PVOID va = MmGetVirtualForPhysical(dtb);
  if (reinterpret_cast<ULONG64>(va) <= 0x1000) {
    return nullptr;
  }
  return va;
}

ULONG64 GetPteAddress(PVOID va) {
  return g_PteBase + 8ull * ((reinterpret_cast<ULONG64>(va) & 0xFFFFFFFFFFFFULL) >> 12);
}

ULONG AllocatePhysicalPage(PhysicalPageInfo* info, SIZE_T size) {
  if (!info || size == 0) {
    return 22;
  }
  if (size != 0x1000) {
    RtlZeroMemory(info, sizeof(*info));
    return 50;
  }

  ULONG err = InitializePteBase();
  if (err != 0) {
    RtlZeroMemory(info, sizeof(*info));
    return err;
  }

  PVOID base = MmAllocateMappingAddress(0x1000, kPoolTag);
  if (!base) {
    RtlZeroMemory(info, sizeof(*info));
    return 0x119;
  }

  PVOID pte = reinterpret_cast<PVOID>(GetPteAddress(base));
  if (!pte || !IsVaPhysicalAddressValid(pte)) {
    MmFreeMappingAddress(base, kPoolTag);
    RtlZeroMemory(info, sizeof(*info));
    return 0x109;
  }

  info->BaseAddress = base;
  info->Size = 0x1000;
  info->PteAddress = pte;
  return 0;
}

void FreePhysicalPage(PhysicalPageInfo* info) {
  if (!info || !info->BaseAddress) {
    return;
  }
  MmFreeMappingAddress(info->BaseAddress, kPoolTag);
  RtlZeroMemory(info, sizeof(*info));
}

ULONG ProcessPhysicalPage(PhysicalPageInfo* xfer, ULONG64 phys_page_base, PVOID buffer, SIZE_T size,
                          BOOLEAN read) {
  if (!phys_page_base || !buffer || size == 0) {
    return 22;
  }
  if (!xfer || !xfer->BaseAddress || !xfer->PteAddress) {
    return 157;
  }
  if (size > xfer->Size) {
    return 279;
  }
  if ((phys_page_base >> 12) != ((phys_page_base + size - 1) >> 12)) {
    return 275;
  }
  if (!IsPhysPageInRange(phys_page_base, size)) {
    return 276;
  }
  if (!IsVaPhysicalAddressValid(xfer->PteAddress)) {
    return 265;
  }

  auto* pte = static_cast<ULONG64*>(xfer->PteAddress);
  const ULONG64 old_pte = *pte;
  *pte = (((phys_page_base >> 12) & 0xFFFFFFFFFULL) << 12) | (old_pte & 0xFFF0000000000EF8ULL) | 0x103ULL;
  __invlpg(xfer->BaseAddress);

  auto* mapped = static_cast<UCHAR*>(xfer->BaseAddress) + (phys_page_base & 0xFFF);
  if (read) {
    RtlCopyMemory(buffer, mapped, size);
  } else {
    RtlCopyMemory(mapped, buffer, size);
  }

  *pte = old_pte;
  __invlpg(xfer->BaseAddress);
  return 0;
}

ULONG GetPageTableInfo(PhysicalPageInfo* xfer, ULONG64 cr3, ULONG64 page_address,
                       PageTableInfo* out) {
  if (!out) {
    return 22;
  }
  RtlZeroMemory(out, sizeof(*out));

  if (ProcessPhysicalPage(xfer, (((cr3 >> 12) & 0xFFFFFFFFFULL) << 12) + 8 * ((page_address >> 39) & 0x1FF),
                          &out->Pxe, 8, TRUE) != 0 ||
      (out->Pxe & 1) == 0) {
    return 262;
  }
  if (((out->Pxe >> 12) & 0xFFFFFFFFFULL) == ((cr3 >> 12) & 0xFFFFFFFFFULL)) {
    return 266;
  }

  if (ProcessPhysicalPage(xfer,
                          (((out->Pxe >> 12) & 0xFFFFFFFFFULL) << 12) + 8 * ((page_address >> 30) & 0x1FF),
                          &out->Ppe, 8, TRUE) != 0 ||
      (out->Ppe & 1) == 0) {
    return 263;
  }
  if (((out->Ppe >> 7) & 1) != 0) {
    out->PageType = 7;
    return 0;
  }

  if (ProcessPhysicalPage(xfer,
                          (((out->Ppe >> 12) & 0xFFFFFFFFFULL) << 12) + 8 * ((page_address >> 21) & 0x1FF),
                          &out->Pde, 8, TRUE) != 0 ||
      (out->Pde & 1) == 0) {
    return 264;
  }
  if (((out->Pde >> 7) & 1) != 0) {
    out->PageType = 6;
    return 0;
  }

  if (ProcessPhysicalPage(xfer,
                          (((out->Pde >> 12) & 0xFFFFFFFFFULL) << 12) + 8 * ((page_address >> 12) & 0x1FF),
                          &out->Pte, 8, TRUE) != 0 ||
      (out->Pte & 1) == 0) {
    return 265;
  }
  out->PageType = 5;
  return 0;
}

ULONG GetPhysPageAddress(PhysicalPageInfo* xfer, ULONG64 target_cr3, PVOID page_va,
                         PULONG64 out_pa) {
  if (!out_pa) {
    return 22;
  }
  *out_pa = 0;

  ULONG64 cr3 = target_cr3;
  if (!cr3) {
    cr3 = __readcr3();
  }

  PageTableInfo info{};
  const ULONG err = GetPageTableInfo(xfer, cr3, reinterpret_cast<ULONG64>(page_va), &info);
  if (err != 0) {
    return err;
  }

  ULONG64 page_phys = 0;
  SIZE_T page_size = 0;
  switch (info.PageType) {
    case 5:
      page_phys = ((info.Pte >> 12) & 0xFFFFFFFFFULL) << 12;
      page_size = 0x1000;
      break;
    case 6:
      page_phys = ((info.Pde >> 21) & 0x7FFFFFFFULL) << 21;
      page_size = 0x200000;
      break;
    case 7:
      page_phys = ((info.Ppe >> 30) & 0x3FFFFFULL) << 30;
      page_size = 0x40000000;
      break;
    default:
      return 276;
  }

  if (!page_phys) {
    return 276;
  }
  const ULONG64 va = reinterpret_cast<ULONG64>(page_va);
  *out_pa = va + page_phys - (~(page_size - 1) & va);
  return 0;
}

}  // namespace

ULONG InitializePteBase() {
  if (g_IsInitPteBaseForSystem && g_PteBase != 0) {
    return 0;
  }

  const ULONG64 cr3 = __readcr3();
  PHYSICAL_ADDRESS dtb{};
  dtb.QuadPart = static_cast<LONGLONG>(((cr3 >> 12) & 0xFFFFFFFFFULL) << 12);
  auto* pml4 = static_cast<ULONG64*>(GetPml4Base(dtb));
  if (!pml4) {
    return 0x106;
  }

  for (ULONG64 index = 0; index < 0x200; ++index) {
    const ULONG64 item = pml4[index];
    if (((item >> 12) & 0xFFFFFFFFFULL) == ((cr3 >> 12) & 0xFFFFFFFFFULL)) {
      g_PteBase = (index << 39) - 0x1000000000000ULL;
      g_IsInitPteBaseForSystem = TRUE;
      return 0;
    }
  }
  return 1;
}

ULONG ProcessVirtualMemory(ULONG64 directory_table_base, PVOID address, PVOID buffer,
                           PULONG size_inout, BOOLEAN read) {
  if (!address || !buffer || !size_inout || *size_inout == 0) {
    return 22;
  }
  if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
    return 261;
  }

  ULONG64 cr3 = directory_table_base;
  if (!cr3) {
    cr3 = __readcr3();
  }

  PhysicalPageInfo xfer{};
  ULONG err = AllocatePhysicalPage(&xfer, 0x1000);
  if (err != 0) {
    return err;
  }

  ULONG size_left = *size_inout;
  const ULONG size_total = size_left;
  ULONG size_done = 0;
  ULONG offset_in_page = static_cast<ULONG>(reinterpret_cast<ULONG64>(address) & 0xFFF);
  ULONG64 page_address = reinterpret_cast<ULONG64>(address) & 0xFFFFFFFFFFFFF000ULL;
  auto* cur = static_cast<UCHAR*>(buffer);

  do {
    ULONG chunk = PAGE_SIZE - offset_in_page;
    if (chunk > size_left) {
      chunk = size_left;
    }

    ULONG64 phys = 0;
    err = GetPhysPageAddress(&xfer, cr3, reinterpret_cast<PVOID>(page_address), &phys);
    BOOLEAN ok = FALSE;
    if (err == 0 && phys != 0 &&
        ProcessPhysicalPage(&xfer, phys + offset_in_page, cur, chunk, read) == 0) {
      size_done += chunk;
      ok = TRUE;
    }

    if (!ok && read) {
      RtlZeroMemory(cur, chunk);
    }

    cur += chunk;
    page_address += offset_in_page + chunk;
    offset_in_page = 0;
    size_left -= chunk;
  } while (size_left != 0 && size_left < size_total);

  FreePhysicalPage(&xfer);

  if (size_done == 0) {
    return 216;
  }
  *size_inout = size_done;
  return 0;
}

}  // namespace phys_pte
}  // namespace xcat_krw
