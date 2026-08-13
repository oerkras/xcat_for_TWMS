// TWMS Classic — GA .text reversible integrity probe (+ optional MemoryCrc extinguish).
// Default OFF. Does not enable business E9.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "ga_text_probe.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"

#include <Windows.h>
#include <Psapi.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x::features::ga_text_probe {
namespace {

constexpr DWORD kDefaultDirtyMs = 10 * 60 * 1000;  // 10 min DRBG window
constexpr DWORD kGaWaitMs = 120 * 1000;
constexpr DWORD kHeartbeatMs = 60 * 1000;
constexpr size_t kPatchBytes = 4;
constexpr uint8_t kPatchPattern[kPatchBytes] = {0x90, 0x90, 0x90, 0x90};  // NOP pad

// grap-core MemoryCrc（ImageBase 0x400000 → 运行时 module+RVA）
// 样本 MD5 CF0439C3474AD5C8A9B1BFBEAB29C65E（Dumps/runtime/_grap_dig/grap-core64.dll · 2026-08-03 复核未变）
constexpr uint32_t kRvaMemoryCrcRpmScan = 0x102D610;  // remounted 2026-08-03: prologue 41 57 41 56
constexpr uint32_t kRvaVtableMemoryCrc = 0x1ECCB00;  // remounted 2026-08-03: 表基；[+0x30]=RpmScan VA
constexpr uint8_t kRpmScanPrologueExpect[] = {0x41, 0x57, 0x41, 0x56};  // push r15,r14…
// xor eax,eax ; ret
constexpr uint8_t kEarlyRet[] = {0x33, 0xC0, 0xC3};

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<bool> gEnabled{false};
std::atomic<bool> gWantCrc{false};
std::atomic<bool> gRunning{false};
DWORD gDirtyMs = kDefaultDirtyMs;

HANDLE gLog = INVALID_HANDLE_VALUE;

void CloseLog() {
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const char* dir = x::runtime::GetBinDir();
    gLog = x::runtime::OpenRotatingDbgLogA(dir, "ga_text_probe.log");
}

void LogLine(const char* fmt, ...) {
    OpenLog();
    char buf[768]{};
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u ", st.wYear, st.wMonth,
                     st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), fmt, ap);
    va_end(ap);
    const size_t len = strnlen(buf, sizeof(buf));
    if (gLog != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(gLog, buf, static_cast<DWORD>(len), &wr, nullptr);
        WriteFile(gLog, "\r\n", 2, &wr, nullptr);
        FlushFileBuffers(gLog);
    }
    x::runtime::LogI("GaTextProbe", "%s", buf + n);
}

void WriteStatus(const char* phase, const char* detail) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%sstate\\ga_text_probe_status.txt", x::runtime::GetBinDir());
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char body[1024]{};
    snprintf(body, sizeof(body),
             "phase=%s\n"
             "detail=%s\n"
             "utc_local=%04u-%02u-%02uT%02u:%02u:%02u\n"
             "dirty_ms=%lu\n"
             "crc_extinguish=%d\n"
             "pid=%lu\n",
             phase ? phase : "?", detail ? detail : "", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, static_cast<unsigned long>(gDirtyMs),
             gWantCrc.load() ? 1 : 0, GetCurrentProcessId());
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, body, static_cast<DWORD>(strlen(body)), &wr, nullptr);
    CloseHandle(h);
}

bool FlagFileOn(const char* leaf) {
    char path[MAX_PATH]{};
    snprintf(path, sizeof(path), "%sstate\\%s", x::runtime::GetBinDir(), leaf);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8]{};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, nullptr);
    CloseHandle(h);
    return rd > 0 && buf[0] == '1';
}

bool EnvOn(const char* name) {
    char buf[16]{};
    return GetEnvironmentVariableA(name, buf, sizeof(buf)) > 0 && buf[0] == '1';
}

DWORD EnvMs(const char* name, DWORD def) {
    char buf[32]{};
    if (GetEnvironmentVariableA(name, buf, sizeof(buf)) == 0) return def;
    const unsigned long v = strtoul(buf, nullptr, 10);
    if (v < 5000) return def;  // refuse tiny windows
    if (v > 60ul * 60ul * 1000ul) return 60ul * 60ul * 1000ul;
    return static_cast<DWORD>(v);
}

struct TextRange {
    uint8_t* base = nullptr;
    size_t size = 0;
    uint32_t rva = 0;
};

bool FindTextSection(HMODULE mod, TextRange* out) {
    if (!mod || !out) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(mod);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<uint8_t*>(mod) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        memcpy(name, sec[i].Name, 8);
        if (strcmp(name, ".text") != 0) continue;
        out->base = reinterpret_cast<uint8_t*>(mod) + sec[i].VirtualAddress;
        out->size = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
        out->rva = sec[i].VirtualAddress;
        return out->size >= 64;
    }
    return false;
}

// Prefer trailing INT3/NOP/zero padding — never execute business code.
bool PickPaddingSite(const TextRange& text, uint8_t** site, uint32_t* rva) {
    if (!site || !rva || text.size < 64) return false;
    constexpr size_t kNeed = 16;
    for (size_t end = text.size; end >= kNeed; --end) {
        const size_t start = end - kNeed;
        bool ok = true;
        for (size_t i = 0; i < kNeed; ++i) {
            const uint8_t b = text.base[start + i];
            if (b != 0xCC && b != 0x00 && b != 0x90) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        // Use middle of the pad run so we stay inside .text
        *site = text.base + start + 4;
        *rva = text.rva + static_cast<uint32_t>(start + 4);
        return true;
    }
    return false;
}

bool ProtectWrite(void* addr, size_t n, const uint8_t* src, uint8_t* backup) {
    if (!addr || !src || !backup || n == 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(backup, addr, n);
    memcpy(addr, src, n);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    DWORD tmp = 0;
    VirtualProtect(addr, n, old, &tmp);
    return true;
}

bool ProtectRestore(void* addr, size_t n, const uint8_t* backup) {
    if (!addr || !backup || n == 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, backup, n);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    DWORD tmp = 0;
    VirtualProtect(addr, n, old, &tmp);
    return true;
}

HMODULE FindGrapCoreModule() {
    // Runtime name is often grap-core64.aes; interface grap64.dll is different.
    HMODULE mods[512]{};
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count && i < 512; ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        // Prefer path containing grap-core
        const wchar_t* leaf = wcsrchr(path, L'\\');
        leaf = leaf ? leaf + 1 : path;
        if (_wcsicmp(leaf, L"grap-core64.aes") == 0) return mods[i];
    }
    for (DWORD i = 0; i < count && i < 512; ++i) {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH)) continue;
        if (wcsstr(path, L"grap-core") != nullptr) return mods[i];
    }
    return nullptr;
}

bool ExtinguishMemoryCrc(HMODULE core) {
    if (!core) return false;
    auto* base = reinterpret_cast<uint8_t*>(core);
    uint8_t* fn = base + kRvaMemoryCrcRpmScan;
    uint8_t* vt = base + kRvaVtableMemoryCrc;

    if (memcmp(fn, kRpmScanPrologueExpect, sizeof(kRpmScanPrologueExpect)) != 0) {
        LogLine("CRC_ABORT prologue mismatch at RVA 0x%X bytes=%02X%02X%02X%02X",
                kRvaMemoryCrcRpmScan, fn[0], fn[1], fn[2], fn[3]);
        return false;
    }
    uint64_t slot30 = 0;
    memcpy(&slot30, vt + 0x30, sizeof(slot30));
    const uint64_t expectVa = reinterpret_cast<uint64_t>(base) + kRvaMemoryCrcRpmScan;
    // Preferred ImageBase style absolute may be rebased — accept RVA match via (slot - base)
    const uint64_t slotRva = slot30 >= reinterpret_cast<uint64_t>(base)
                                 ? slot30 - reinterpret_cast<uint64_t>(base)
                                 : 0;
    if (slot30 != expectVa && slotRva != kRvaMemoryCrcRpmScan &&
        slot30 != (0x400000ull + kRvaMemoryCrcRpmScan)) {
        LogLine("CRC_ABORT vtable+0x30 unexpected slot=%llX expect_fn=%p",
                static_cast<unsigned long long>(slot30), fn);
        return false;
    }

    uint8_t backup[sizeof(kEarlyRet)]{};
    if (!ProtectWrite(fn, sizeof(kEarlyRet), kEarlyRet, backup)) {
        LogLine("CRC_ABORT VirtualProtect RpmScan failed err=%lu", GetLastError());
        return false;
    }
    LogLine("CRC_OK RpmScan early-ret applied RVA=0x%X before=%02X%02X%02X%02X after=33C0C3",
            kRvaMemoryCrcRpmScan, backup[0], backup[1], backup[2],
            backup[3] ? backup[3] : 0);
    // Persist backup next to status for manual restore tooling
    char bakPath[MAX_PATH]{};
    snprintf(bakPath, sizeof(bakPath), "%sstate\\ga_text_probe_crc_backup.bin",
             x::runtime::GetBinDir());
    HANDLE hb = CreateFileA(bakPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hb != INVALID_HANDLE_VALUE) {
        DWORD wr = 0;
        WriteFile(hb, backup, sizeof(backup), &wr, nullptr);
        CloseHandle(hb);
    }
    return true;
}

bool WaitMsInterruptible(DWORD totalMs) {
    const DWORD step = 500;
    DWORD left = totalMs;
    DWORD lastBeat = GetTickCount();
    while (left > 0 && !gStop.load()) {
        const DWORD slice = left > step ? step : left;
        Sleep(slice);
        left -= slice;
        const DWORD now = GetTickCount();
        if (now - lastBeat >= kHeartbeatMs) {
            lastBeat = now;
            LogLine("HEARTBEAT dirty_left_ms~%lu", static_cast<unsigned long>(left));
            WriteStatus("DIRTY", "heartbeat");
        }
    }
    return !gStop.load();
}

DWORD WINAPI Worker(LPVOID) {
    gRunning.store(true);
    WriteStatus("BOOT", "worker_start");
    LogLine("BOOT enabled=%d crc=%d dirty_ms=%lu", gEnabled.load() ? 1 : 0,
            gWantCrc.load() ? 1 : 0, static_cast<unsigned long>(gDirtyMs));

    if (!gEnabled.load()) {
        WriteStatus("IDLE", "disabled");
        gRunning.store(false);
        return 0;
    }

    const DWORD t0 = GetTickCount();
    while (!gStop.load() && (GetTickCount() - t0) < kGaWaitMs) {
        if (x::runtime::il2cpp::Ensure()) break;
        Sleep(500);
    }
    if (!x::runtime::il2cpp::Ensure()) {
        LogLine("FAIL GameAssembly not ready within %lu ms", static_cast<unsigned long>(kGaWaitMs));
        WriteStatus("FAIL", "no_gameassembly");
        gRunning.store(false);
        return 0;
    }

    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    TextRange text{};
    if (!FindTextSection(ga, &text)) {
        LogLine("FAIL .text section not found");
        WriteStatus("FAIL", "no_text");
        gRunning.store(false);
        return 0;
    }
    LogLine("GA base=%p .text rva=0x%X size=0x%zX", ga, text.rva, text.size);

    if (gWantCrc.load()) {
        HMODULE core = FindGrapCoreModule();
        if (!core) {
            LogLine("CRC_FAIL grap-core module not found in process");
            WriteStatus("FAIL", "no_grap_core");
            gRunning.store(false);
            return 0;
        }
        LogLine("CRC module=%p", core);
        if (!ExtinguishMemoryCrc(core)) {
            WriteStatus("FAIL", "crc_extinguish");
            gRunning.store(false);
            return 0;
        }
        WriteStatus("CRC_APPLIED", "rpmscan_early_ret");
    }

    uint8_t* site = nullptr;
    uint32_t siteRva = 0;
    if (!PickPaddingSite(text, &site, &siteRva)) {
        LogLine("FAIL no INT3/NOP/zero padding in .text tail");
        WriteStatus("FAIL", "no_padding");
        gRunning.store(false);
        return 0;
    }

    uint8_t before[kPatchBytes]{};
    if (!ProtectWrite(site, kPatchBytes, kPatchPattern, before)) {
        LogLine("FAIL VirtualProtect write site=%p err=%lu", site, GetLastError());
        WriteStatus("FAIL", "protect_write");
        gRunning.store(false);
        return 0;
    }
    LogLine("DIRTY site=%p rva=0x%X before=%02X%02X%02X%02X after=90909090 window_ms=%lu", site,
            siteRva, before[0], before[1], before[2], before[3],
            static_cast<unsigned long>(gDirtyMs));
    WriteStatus("DIRTY", "text_patched_waiting_scan_window");

    const bool finished = WaitMsInterruptible(gDirtyMs);

    if (!ProtectRestore(site, kPatchBytes, before)) {
        LogLine("FAIL restore site=%p err=%lu", site, GetLastError());
        WriteStatus("FAIL", "restore");
        gRunning.store(false);
        return 0;
    }
    LogLine("RESTORE site=%p rva=0x%X bytes=%02X%02X%02X%02X finished_window=%d", site, siteRva,
            before[0], before[1], before[2], before[3], finished ? 1 : 0);

    if (finished) {
        WriteStatus("PASS", "survived_dirty_window_and_restored");
        LogLine("PASS survived dirty window — process still alive after restore");
    } else {
        WriteStatus("ABORT", "stop_requested_before_window_end");
        LogLine("ABORT stop before window end (still restored)");
    }

    gRunning.store(false);
    return 0;
}

}  // namespace

void Init() {
    const bool want = EnvOn("GA_TEXT_PROBE") || FlagFileOn("ga_text_probe.enable");
    if (want && !EnvOn("XCAT_ALLOW_TEXT_PATCH")) {
        SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", "1");
    }
    const bool allowText = EnvOn("XCAT_ALLOW_TEXT_PATCH");
    gEnabled.store(want && allowText);
    gWantCrc.store(EnvOn("GA_TEXT_PROBE_CRC") || FlagFileOn("ga_text_probe_crc.enable"));
    gDirtyMs = EnvMs("GA_TEXT_PROBE_MS", kDefaultDirtyMs);
    if (gEnabled.load()) {
        OpenLog();
        LogLine("Init enabled=1 crc=%d dirty_ms=%lu", gWantCrc.load() ? 1 : 0,
                static_cast<unsigned long>(gDirtyMs));
        x::runtime::LogI("GaTextProbe", "ENABLED dirty_ms=%lu crc=%d",
                         static_cast<unsigned long>(gDirtyMs), gWantCrc.load() ? 1 : 0);
    } else {
        x::runtime::LogI("GaTextProbe", "disabled (set GA_TEXT_PROBE=1)");
    }
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (!gEnabled.load()) return;
    if (gWorker.load()) return;
    gStop.store(false);
    HANDLE h = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!h) {
        x::runtime::LogE("GaTextProbe", "CreateThread failed err=%lu", GetLastError());
        return;
    }
    gWorker.store(h);
}

void StopWorker() {
    gStop.store(true);
    HANDLE h = gWorker.exchange(nullptr);
    // Never join under loader lock; signal only (same as other features).
    if (h) CloseHandle(h);
    CloseLog();
}

bool IsEnabled() { return gEnabled.load(); }
bool IsRunning() { return gRunning.load(); }

}  // namespace x::features::ga_text_probe
