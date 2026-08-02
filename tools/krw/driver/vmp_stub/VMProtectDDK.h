#pragma once
// No-op stubs replacing VMProtectDDK for XCatKrw (Scope B: no VMP).

#ifndef VM_BEGIN
#define VM_BEGIN() ((void)0)
#endif
#ifndef VM_END
#define VM_END() ((void)0)
#endif
#ifndef VM_STRA
#define VM_STRA(s) (s)
#endif
#ifndef VMProtectBegin
#define VMProtectBegin() ((void)0)
#endif
#ifndef VMProtectEnd
#define VMProtectEnd() ((void)0)
#endif
#ifndef VMProtectBeginUltra
#define VMProtectBeginUltra() ((void)0)
#endif
#ifndef VMProtectBeginMutation
#define VMProtectBeginMutation() ((void)0)
#endif
#ifndef VMProtectBeginVirtualization
#define VMProtectBeginVirtualization() ((void)0)
#endif
#ifndef VMProtectIsProtected
#define VMProtectIsProtected() (0)
#endif
#ifndef VMProtectIsDebuggerPresent
#define VMProtectIsDebuggerPresent(x) (0)
#endif
#ifndef VMProtectIsVirtualMachinePresent
#define VMProtectIsVirtualMachinePresent() (0)
#endif
#ifndef VMProtectDecryptStringA
#define VMProtectDecryptStringA(s) (s)
#endif
#ifndef VMProtectDecryptStringW
#define VMProtectDecryptStringW(s) (s)
#endif
