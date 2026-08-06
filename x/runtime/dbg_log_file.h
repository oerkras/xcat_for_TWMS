#pragma once

// Generation-keeping open for the hand-rolled diagnostic logs (combat.log, kick.log,
// foothold.log, send.log, …). These sit outside xcat::log because they are raw high-rate
// traces, not structured events, and they must not be swept into the uploader.
//
// Policy:
//   - First open in a process: rotate base -> base.1 -> … -> base.N (session boundary).
//   - Within a session: AppendDbgLog size-rotates when the live file reaches kDbgLogMaxBytes.
//   - One cached HANDLE per absolute path (mutex), so dual writers of combat.log
//     (simple_combat + attack_input_port) share one file object and rotate safely together.

#include <Windows.h>

#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>

namespace x::runtime {

inline constexpr int kDbgLogGenerations = 24;
// Align with structured payload rotation / upload chunk; session-unbounded append was the
// commit-pressure footgun (combat.log grew for the whole hang session).
inline constexpr ULONGLONG kDbgLogMaxBytes = 512ull * 1024ull;

namespace detail {

inline std::mutex& DbgLogMutex() {
    static std::mutex m;
    return m;
}

inline std::wstring DbgLogKey(const std::wstring& full) {
    std::wstring k = full;
    for (wchar_t& c : k) c = static_cast<wchar_t>(towupper(c));
    return k;
}

struct DbgLogSlot {
    HANDLE h = INVALID_HANDLE_VALUE;
    ULONGLONG bytes = 0;
    bool sessionRotated = false;
};

inline std::unordered_map<std::wstring, DbgLogSlot>& DbgLogSlots() {
    static std::unordered_map<std::wstring, DbgLogSlot> s;
    return s;
}

}  // namespace detail

// base -> base.1 -> ... -> base.N, oldest dropped. Returns false when the live file could not
// be moved aside (an editor or the uploader holding it), so the caller can append instead of
// truncating — losing the rotation is survivable, losing the capture is not.
inline bool RotateDbgLogGenerations(const std::wstring& full, int generations) {
    if (full.empty() || generations <= 0) return false;

    DeleteFileW((full + L"." + std::to_wstring(generations)).c_str());
    for (int i = generations; i > 1; --i) {
        MoveFileExW((full + L"." + std::to_wstring(i - 1)).c_str(),
                    (full + L"." + std::to_wstring(i)).c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) return true;  // nothing yet
    if (MoveFileExW(full.c_str(), (full + L".1").c_str(), MOVEFILE_REPLACE_EXISTING)) return true;

    // Held open elsewhere. Copy what we can so .1 is still a full backup, then report failure
    // so the caller appends rather than truncating the original out from under the reader.
    CopyFileW(full.c_str(), (full + L".1").c_str(), FALSE);
    return false;
}

namespace detail {

inline void CloseSlotUnlocked(DbgLogSlot& slot) {
    if (slot.h != INVALID_HANDLE_VALUE) {
        CloseHandle(slot.h);
        slot.h = INVALID_HANDLE_VALUE;
    }
    slot.bytes = 0;
}

inline bool OpenAppendUnlocked(DbgLogSlot& slot, const std::wstring& full) {
    CloseSlotUnlocked(slot);
    slot.h = CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (slot.h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(slot.h, &sz) && sz.QuadPart > 0)
        slot.bytes = static_cast<ULONGLONG>(sz.QuadPart);
    else
        slot.bytes = 0;
    return true;
}

inline bool EnsureFreshUnlocked(DbgLogSlot& slot, const std::wstring& full, int generations) {
    CloseSlotUnlocked(slot);
    const bool moved = RotateDbgLogGenerations(full, generations);
    if (moved) {
        const HANDLE fresh =
            CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fresh != INVALID_HANDLE_VALUE) CloseHandle(fresh);
    }
    return OpenAppendUnlocked(slot, full);
}

}  // namespace detail

// Preferred write path: session-first rotation + mid-session size rotation, single HANDLE.
inline bool AppendDbgLog(const std::wstring& full, const void* data, DWORD n,
                         int generations = kDbgLogGenerations) {
    if (full.empty() || !data || n == 0) return false;
    std::lock_guard<std::mutex> lk(detail::DbgLogMutex());
    detail::DbgLogSlot& slot = detail::DbgLogSlots()[detail::DbgLogKey(full)];

    if (!slot.sessionRotated) {
        slot.sessionRotated = true;
        if (!detail::EnsureFreshUnlocked(slot, full, generations)) return false;
    } else if (slot.h == INVALID_HANDLE_VALUE) {
        if (!detail::OpenAppendUnlocked(slot, full)) return false;
    }

    if (slot.bytes >= kDbgLogMaxBytes ||
        (kDbgLogMaxBytes - slot.bytes) < static_cast<ULONGLONG>(n)) {
        if (!detail::EnsureFreshUnlocked(slot, full, generations)) return false;
    }

    DWORD w = 0;
    if (!WriteFile(slot.h, data, n, &w, nullptr)) return false;
    slot.bytes += w;
    return w == n;
}

inline bool AppendDbgLogA(const char* dir, const char* leaf, const void* data, DWORD n,
                          int generations = kDbgLogGenerations) {
    if (!dir || !leaf) return false;
    const int dn = MultiByteToWideChar(CP_ACP, 0, dir, -1, nullptr, 0);
    const int ln = MultiByteToWideChar(CP_ACP, 0, leaf, -1, nullptr, 0);
    if (dn <= 0 || ln <= 0) return false;
    std::wstring wdir(static_cast<size_t>(dn) - 1, L'\0');
    std::wstring wleaf(static_cast<size_t>(ln) - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir.data(), dn);
    MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf.data(), ln);
    return AppendDbgLog(wdir + L"\\" + wleaf, data, n, generations);
}

inline void FlushDbgLog(const std::wstring& full) {
    if (full.empty()) return;
    std::lock_guard<std::mutex> lk(detail::DbgLogMutex());
    auto it = detail::DbgLogSlots().find(detail::DbgLogKey(full));
    if (it == detail::DbgLogSlots().end()) return;
    if (it->second.h != INVALID_HANDLE_VALUE) FlushFileBuffers(it->second.h);
}

inline void CloseDbgLog(const std::wstring& full) {
    if (full.empty()) return;
    std::lock_guard<std::mutex> lk(detail::DbgLogMutex());
    auto it = detail::DbgLogSlots().find(detail::DbgLogKey(full));
    if (it == detail::DbgLogSlots().end()) return;
    detail::CloseSlotUnlocked(it->second);
    // Keep sessionRotated so a later Append in the same process does not wipe .1 mid-run
    // unless size-rotation triggers EnsureFresh.
}

// Legacy open for call sites that still WriteFile on a kept HANDLE. Mid-session size
// rotation requires AppendDbgLog — this path only does first-open generation rotate.
inline HANDLE OpenRotatingDbgLog(const std::wstring& dir, const wchar_t* leaf,
                                 int generations = kDbgLogGenerations) {
    if (dir.empty() || !leaf) return INVALID_HANDLE_VALUE;
    const std::wstring full = dir + L"\\" + leaf;

    if (generations <= 0) {
        return CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    {
        std::lock_guard<std::mutex> lk(detail::DbgLogMutex());
        detail::DbgLogSlot& slot = detail::DbgLogSlots()[detail::DbgLogKey(full)];
        // If AppendDbgLog already owns this path, do not open a second long-lived handle.
        if (slot.sessionRotated && slot.h != INVALID_HANDLE_VALUE) {
            HANDLE dup = INVALID_HANDLE_VALUE;
            if (DuplicateHandle(GetCurrentProcess(), slot.h, GetCurrentProcess(), &dup, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                return dup;
            return INVALID_HANDLE_VALUE;
        }
        if (!slot.sessionRotated) {
            slot.sessionRotated = true;
            if (!detail::EnsureFreshUnlocked(slot, full, generations)) return INVALID_HANDLE_VALUE;
            HANDLE dup = INVALID_HANDLE_VALUE;
            if (DuplicateHandle(GetCurrentProcess(), slot.h, GetCurrentProcess(), &dup, 0, FALSE,
                                DUPLICATE_SAME_ACCESS))
                return dup;
            return INVALID_HANDLE_VALUE;
        }
    }

    return CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

inline HANDLE OpenRotatingDbgLogA(const char* dir, const char* leaf,
                                  int generations = kDbgLogGenerations) {
    if (!dir || !leaf) return INVALID_HANDLE_VALUE;
    const int dn = MultiByteToWideChar(CP_ACP, 0, dir, -1, nullptr, 0);
    const int ln = MultiByteToWideChar(CP_ACP, 0, leaf, -1, nullptr, 0);
    if (dn <= 0 || ln <= 0) return INVALID_HANDLE_VALUE;
    std::wstring wdir(static_cast<size_t>(dn) - 1, L'\0');
    std::wstring wleaf(static_cast<size_t>(ln) - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir.data(), dn);
    MultiByteToWideChar(CP_ACP, 0, leaf, -1, wleaf.data(), ln);
    return OpenRotatingDbgLog(wdir, wleaf.c_str(), generations);
}

}  // namespace x::runtime
