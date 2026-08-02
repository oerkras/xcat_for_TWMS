#pragma once

#include <ntifs.h>

namespace xcat_krw {

// Copy between caller buffer (kernel VA in current process) and target process VA
// by walking the target CR3 and using MmMapIoSpace on physical pages.
// Must run at PASSIVE_LEVEL / APC_LEVEL (IOCTL path).
NTSTATUS PhysCopyVirtual(PEPROCESS target, PVOID remote_va, PVOID buffer, SIZE_T size, BOOLEAN write,
                         PSIZE_T transferred);

}  // namespace xcat_krw
