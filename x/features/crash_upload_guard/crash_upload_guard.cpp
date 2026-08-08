// Classic TWMS — 给游戏自带 CrashReporter 的同步上传套上超时，避免它冻住主线程。
//
// 背景（2026-08-09 实机取证 hang_20260809_044528.txt）：
// 托管层抛异常后，Maplestory_Classic_Data/Plugins/x86_64/CrashReporter.dll 会**在 Unity
// 主线程上同步**把崩溃报告 POST 出去。它导入的是 WinINet 的
// HttpSendRequestExW + InternetWriteFile + HttpEndRequestW 这套分块上传，全程阻塞。
// 主泵栈实测停在 WININET!HttpSendRequestExW+0xcc → KERNELBASE!WaitForSingleObjectEx，
// 静默 12 s 以上 —— 用户看到的就是黑屏卡死。用户机上挂着本地代理，上传目标连不通时
// WinINet 默认几乎是无限等，于是必然复现。
//
// 处理方式：只给那个请求句柄补上超时，不改上传逻辑本身 —— 网络正常时崩溃报告照常发出，
// 网络异常时最多顿一下就返回失败。刻意**不**去禁用崩溃上报：那是游戏的正常行为，
// 我们要消掉的是「无限等待」，不是这个功能。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "crash_upload_guard.h"

#include <windows.h>
#include <wininet.h>

#include <atomic>
#include <cstring>
#include <cwchar>

#include "../../runtime/log.h"

namespace x::features::crash_upload_guard {
namespace {

constexpr wchar_t kTargetModule[] = L"CrashReporter.dll";
constexpr char kWinInet[] = "WININET.dll";
constexpr char kHookedFn[] = "HttpSendRequestExW";

// 每一档都按「够一次正常上传、又不至于让人以为卡死」来定。最坏情况是各档串行叠加，
// 仍远小于看门狗 12 s 的判死线，不会再被记成一次卡死。
constexpr DWORD kTimeoutMs = 2000;

using FnHttpSendRequestExW = BOOL(WINAPI*)(HINTERNET, LPINTERNET_BUFFERSW, LPINTERNET_BUFFERSW,
                                           DWORD, DWORD_PTR);
using FnInternetSetOptionW = BOOL(WINAPI*)(HINTERNET, DWORD, LPVOID, DWORD);

std::atomic<bool> gRunning{false};
std::atomic<bool> gStop{false};
std::atomic<bool> gInstalled{false};
std::atomic<bool> gInstalledByNotify{false};  // 供轮询线程补打日志（回调里不能落盘）
HANDLE gThread = nullptr;
FnHttpSendRequestExW gRealSend = nullptr;
FnInternetSetOptionW gSetOption = nullptr;
void* gLdrCookie = nullptr;

void ApplyTimeouts(HINTERNET h) {
    if (!h || !gSetOption) return;
    DWORD v = kTimeoutMs;
    static const DWORD kOpts[] = {
        INTERNET_OPTION_CONNECT_TIMEOUT,   INTERNET_OPTION_SEND_TIMEOUT,
        INTERNET_OPTION_RECEIVE_TIMEOUT,   INTERNET_OPTION_DATA_SEND_TIMEOUT,
        INTERNET_OPTION_DATA_RECEIVE_TIMEOUT,
    };
    for (DWORD opt : kOpts) {
        __try {
            gSetOption(h, opt, &v, sizeof(v));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

BOOL WINAPI HookHttpSendRequestExW(HINTERNET hRequest, LPINTERNET_BUFFERSW in,
                                   LPINTERNET_BUFFERSW out, DWORD flags, DWORD_PTR ctx) {
    ApplyTimeouts(hRequest);
    x::runtime::LogWThrottled(913, 60000, "CrashUpload",
                              "崩溃报告正在主线程上同步上传，已套 %lu ms 超时；"
                              "若此刻画面短暂僵住即为此处",
                              kTimeoutMs);
    if (!gRealSend) return FALSE;
    return gRealSend(hRequest, in, out, flags, ctx);
}

// 把 mod 的导入表里 dllName!funcName 那一格改指向 newFn。返回原函数。
void* PatchIat(HMODULE mod, const char* dllName, const char* funcName, void* newFn) {
    if (!mod || !dllName || !funcName || !newFn) return nullptr;
    auto base = reinterpret_cast<uint8_t*>(mod);
    void* prev = nullptr;
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress || !dir.Size) return nullptr;

        auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        for (; imp->Name; ++imp) {
            const char* name = reinterpret_cast<const char*>(base + imp->Name);
            if (_stricmp(name, dllName) != 0) continue;
            // OriginalFirstThunk 保存名字，FirstThunk 才是运行时要改的那一格。
            const DWORD nameThunkRva = imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                               : imp->FirstThunk;
            if (!nameThunkRva || !imp->FirstThunk) continue;
            auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + nameThunkRva);
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + imp->FirstThunk);
            for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
                if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG64) continue;
                auto* imp_by_name =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                if (strcmp(imp_by_name->Name, funcName) != 0) continue;

                DWORD old = 0;
                if (!VirtualProtect(&slots[i], sizeof(slots[i]), PAGE_READWRITE, &old)) {
                    return nullptr;
                }
                prev = reinterpret_cast<void*>(slots[i].u1.Function);
                slots[i].u1.Function = reinterpret_cast<ULONGLONG>(newFn);
                VirtualProtect(&slots[i], sizeof(slots[i]), old, &old);
                return prev;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

// 返回「不必再重试」。quiet=true 用于加载器回调：那时在加载器锁里，一律不落盘。
bool TryInstall(bool quiet) {
    HMODULE cr = GetModuleHandleW(kTargetModule);
    if (!cr) return false;

    HMODULE wi = GetModuleHandleA(kWinInet);
    if (!wi) return false;  // CrashReporter 在，WinINet 却没起来：等下一轮
    gSetOption =
        reinterpret_cast<FnInternetSetOptionW>(GetProcAddress(wi, "InternetSetOptionW"));
    if (!gSetOption) {
        if (!quiet) x::runtime::LogW("CrashUpload", "拿不到 InternetSetOptionW，超时护栏无法安装");
        return true;  // 别再重试了
    }

    void* prev = PatchIat(cr, kWinInet, kHookedFn, reinterpret_cast<void*>(&HookHttpSendRequestExW));
    if (!prev) {
        if (!quiet) {
            x::runtime::LogW("CrashUpload",
                             "%s 的 %s 导入格没找到（导入表变了？），超时护栏未安装",
                             "CrashReporter.dll", kHookedFn);
        }
        return true;
    }
    gRealSend = reinterpret_cast<FnHttpSendRequestExW>(prev);
    gInstalled.store(true, std::memory_order_release);
    if (!quiet) {
        x::runtime::LogI("CrashUpload",
                         "已给 CrashReporter 的 %s 套上 %lu ms 超时 —— 崩溃报告上传不会再无限"
                         "阻塞主线程（那正是 04:45 那次黑屏的成因）",
                         kHookedFn, kTimeoutMs);
    }
    return true;
}

// ——— 加载即挂钩 ———
// CrashReporter.dll 是**按需加载**的：2026-08-09 05:38 那次实测，客户端 05:38:15 起来，
// 它到 05:38:36 才被映射，而 05:38:37.07 主泵就已经堵死在 HttpSendRequestExW 上——
// 从加载到发请求只隔了约 200 ms。轮询（哪怕 100 ms）都可能输，所以改用加载器通知：
// 模块一映射完就地打钩子，不给它留窗口。
struct UnicodeStringLite {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

struct LdrDllNotificationData {
    ULONG Flags;
    const UnicodeStringLite* FullDllName;
    const UnicodeStringLite* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};

constexpr ULONG kLdrDllNotificationReasonLoaded = 1;

using FnLdrNotify = VOID(CALLBACK*)(ULONG, const LdrDllNotificationData*, PVOID);
using FnLdrRegister = LONG(NTAPI*)(ULONG, FnLdrNotify, PVOID, PVOID*);
using FnLdrUnregister = LONG(NTAPI*)(PVOID);

bool BaseNameIs(const UnicodeStringLite* s, const wchar_t* want) {
    if (!s || !s->Buffer || !s->Length) return false;
    const size_t n = s->Length / sizeof(wchar_t);
    const size_t w = wcslen(want);
    if (n != w) return false;
    return _wcsnicmp(s->Buffer, want, w) == 0;
}

VOID CALLBACK OnDllEvent(ULONG reason, const LdrDllNotificationData* data, PVOID) {
    if (reason != kLdrDllNotificationReasonLoaded || !data) return;
    if (gInstalled.load(std::memory_order_acquire)) return;
    if (!BaseNameIs(data->BaseDllName, kTargetModule)) return;
    // 回调在**加载器锁**里跑：只做 VirtualProtect + 写指针，绝不落盘、绝不 LoadLibrary。
    TryInstall(true);
    if (gInstalled.load(std::memory_order_acquire)) {
        gInstalledByNotify.store(true, std::memory_order_release);
    }
}

bool RegisterLoadNotify() {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return false;
    auto reg = reinterpret_cast<FnLdrRegister>(GetProcAddress(nt, "LdrRegisterDllNotification"));
    if (!reg) return false;
    return reg(0, &OnDllEvent, nullptr, &gLdrCookie) >= 0;
}

DWORD WINAPI WatchThread(LPVOID) {
    const bool notify = RegisterLoadNotify();

    // 通知只覆盖「注册之后才加载」的情况；若模块已经在了，这里立刻补一次。
    // 同时它也是通知注册失败时的退路，间隔取短一些，尽量少输一点。
    while (!gStop.load(std::memory_order_acquire)) {
        if (gInstalledByNotify.exchange(false, std::memory_order_acq_rel)) {
            x::runtime::LogI("CrashUpload", "加载器通知命中：%s 一映射完就已挂上超时钩子",
                             "CrashReporter.dll");
        }
        if (gInstalled.load(std::memory_order_acquire)) break;
        if (TryInstall(false)) break;
        Sleep(notify ? 500 : 100);
    }
    gRunning.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

void Start() {
    bool expected = false;
    if (!gRunning.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    gStop.store(false, std::memory_order_release);
    gThread = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr);
    if (!gThread) {
        gRunning.store(false, std::memory_order_release);
        x::runtime::LogW("CrashUpload", "看守线程创建失败，崩溃上传仍可能冻住主线程");
    }
}

void Stop() {
    gStop.store(true, std::memory_order_release);
    if (gLdrCookie) {
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            if (auto un = reinterpret_cast<FnLdrUnregister>(
                    GetProcAddress(nt, "LdrUnregisterDllNotification"))) {
                un(gLdrCookie);
            }
        }
        gLdrCookie = nullptr;
    }
    if (gThread) {
        CloseHandle(gThread);  // 只放句柄，不 join：DETACH 在加载器锁上
        gThread = nullptr;
    }
    // 刻意不还原 IAT：卸载时机与游戏线程无同步，改回去反而可能在别人正调用时打中。
}

}  // namespace x::features::crash_upload_guard
