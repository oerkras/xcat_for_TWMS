#pragma once

// Compat ABI when statically linked into smoke tools (not only as a DLL).
#ifndef XCAT_KRW_COMPAT_STATIC
#ifdef XCAT_KRW_COMPAT_EXPORTS
#define XCAT_KRW_COMPAT_API __declspec(dllexport)
#else
#define XCAT_KRW_COMPAT_API __declspec(dllimport)
#endif
#else
#define XCAT_KRW_COMPAT_API
#endif

#include <Windows.h>

// RW_Driver Client_Dll-compatible exports (no VMP). ETW-only; no IOCTL fallback.

extern "C" {

XCAT_KRW_COMPAT_API BOOLEAN Init(DWORD Key);
XCAT_KRW_COMPAT_API VOID UnInit();
XCAT_KRW_COMPAT_API BOOLEAN Read(DWORD Pid, UINT64 Address, UINT64 Buffer,
                                 DWORD NumberOfBytesToRead, DWORD& NumberOfBytesReaded,
                                 BOOLEAN RealCr3);
XCAT_KRW_COMPAT_API BOOLEAN Write(DWORD Pid, UINT64 Address, UINT64 Buffer,
                                  DWORD NumberOfBytesToWrite, DWORD& NumberOfBytesWritten,
                                  BOOLEAN RealCr3);
XCAT_KRW_COMPAT_API BOOLEAN Inject(DWORD Pid, PCHAR DllPath);

}
