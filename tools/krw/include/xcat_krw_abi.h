#pragma once

// Shared ABI for tools/krw (user + kernel). Keep POD / C-compatible.

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_KERNEL_MODE)
#include <ntdef.h>
#else
#include <Windows.h>
typedef signed long NTSTATUS;
#endif

#define XCAT_KRW_DEVICE_NAME L"\\Device\\XCatKrw"
#define XCAT_KRW_SYMLINK_NAME L"\\DosDevices\\XCatKrw"
#define XCAT_KRW_WIN32_NAME L"\\\\.\\XCatKrw"

// Historical IOCTL ABI (driver no longer creates a Device; stealth path is ETW-only).
// Kept for reference / optional lab tooling that still compiles against these codes.
#define XCAT_KRW_DEVICE_TYPE 0x8000u

#define XCAT_KRW_CTL_CODE(i) \
  CTL_CODE(XCAT_KRW_DEVICE_TYPE, 0x800u + (i), METHOD_BUFFERED, FILE_ANY_ACCESS)

enum XcatKrwIoctl {
  XcatKrwIoctl_Handshake = XCAT_KRW_CTL_CODE(1),
  XcatKrwIoctl_Read = XCAT_KRW_CTL_CODE(2),
  XcatKrwIoctl_Write = XCAT_KRW_CTL_CODE(3),
  XcatKrwIoctl_QueryInfo = XCAT_KRW_CTL_CODE(4),
  XcatKrwIoctl_Inject = XCAT_KRW_CTL_CODE(5),
};

enum XcatKrwStatus {
  XcatKrwStatus_Ok = 0,
  XcatKrwStatus_BadKey = 1,
  XcatKrwStatus_BadArgs = 2,
  XcatKrwStatus_LookupFailed = 3,
  XcatKrwStatus_CopyFailed = 4,
  XcatKrwStatus_NotReady = 5,
  XcatKrwStatus_AccessDenied = 6,
};

enum XcatKrwRwFlags {
  XcatKrwRwFlag_None = 0,
  XcatKrwRwFlag_Phys = 1u << 0,  // CR3 walk + MmMapIoSpace
};

#pragma pack(push, 8)

typedef struct XcatKrwHandshakeIn {
  UINT64 session_key;
  UINT32 client_abi;
  UINT32 reserved;
} XcatKrwHandshakeIn;

typedef struct XcatKrwHandshakeOut {
  UINT32 status;
  UINT32 driver_abi;
  UINT64 server_token;
} XcatKrwHandshakeOut;

typedef struct XcatKrwRwIn {
  UINT64 session_key;
  UINT32 pid;
  UINT32 size;
  UINT64 remote_va;
  UINT32 flags;  // XcatKrwRwFlags
  UINT32 reserved;
} XcatKrwRwIn;

typedef struct XcatKrwRwOut {
  UINT32 status;
  UINT32 bytes_transferred;
} XcatKrwRwOut;

typedef struct XcatKrwQueryInfoOut {
  UINT32 status;
  UINT32 driver_abi;
  UINT32 features;  // XCAT_KRW_FEATURE_*
  UINT32 reserved;
} XcatKrwQueryInfoOut;

typedef struct XcatKrwInjectIn {
  UINT64 session_key;
  UINT32 pid;
  UINT32 path_bytes;  // bytes of UTF-16 path including L'\0'
  // WCHAR path[] follows
} XcatKrwInjectIn;

typedef struct XcatKrwInjectOut {
  UINT32 status;
  UINT32 ntstatus;
} XcatKrwInjectOut;

#pragma pack(pop)

#define XCAT_KRW_ABI_VERSION 3u
#define XCAT_KRW_FEATURE_MMCOPY (1u << 0)
#define XCAT_KRW_FEATURE_PHYS (1u << 1)
#define XCAT_KRW_FEATURE_ETW_IPC (1u << 2)
#define XCAT_KRW_FEATURE_INJECT (1u << 3)

#ifdef __cplusplus
}
#endif
