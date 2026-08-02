#pragma once

// Generation-keeping open for the hand-rolled diagnostic logs (movepath_elems.log,
// keypad_bin.log, kick.log, send.log, 以及历史 fly.log 等). These sit outside xcat::log
// because they are raw high-rate traces, not structured events, and they must not be swept
// into the uploader.
//
// Each of them used to get this wrong in its own way: some keepers used a single generation
// so two client launches destroyed the first capture; kick.log opened CREATE_ALWAYS with no
// backup at all, which is why the 08-19 drop had no evidence left; send.log appended forever,
// growing without bound and mixing every session into one file. One policy for all of them now.

#include <Windows.h>

#include <cwctype>
#include <mutex>
#include <set>
#include <string>

namespace x::runtime {

// A run is one client launch. Disk is cheap next to re-reproducing a drop, so keep plenty.
inline constexpr int kDbgLogGenerations = 24;

namespace detail {

inline std::mutex& DbgLogMutex() {
    static std::mutex m;
    return m;
}

inline std::set<std::wstring>& DbgLogRotated() {
    static std::set<std::wstring> s;
    return s;
}

inline std::wstring DbgLogKey(const std::wstring& full) {
    std::wstring k = full;
    for (wchar_t& c : k) c = static_cast<wchar_t>(towupper(c));
    return k;
}

// True the first time this process opens the path. combat.log is opened by both simple_combat
// and attack_input_port; without this the second opener would rotate away the file the first
// one is still holding, splitting the run across two generations.
inline bool ClaimFirstOpen(const std::wstring& full) {
    std::lock_guard<std::mutex> lk(DbgLogMutex());
    return DbgLogRotated().insert(DbgLogKey(full)).second;
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

inline HANDLE OpenRotatingDbgLog(const std::wstring& dir, const wchar_t* leaf,
                                 int generations = kDbgLogGenerations) {
    if (dir.empty() || !leaf) return INVALID_HANDLE_VALUE;
    const std::wstring full = dir + L"\\" + leaf;

    if (generations <= 0) {  // beacons and marker files: plain truncate, no history wanted
        return CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    if (detail::ClaimFirstOpen(full) && RotateDbgLogGenerations(full, generations)) {
        // Start the generation empty, then drop the handle: everyone opens the same way below.
        const HANDLE fresh =
            CreateFileW(full.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fresh != INVALID_HANDLE_VALUE) CloseHandle(fresh);
    }

    // Always append, and always share write. Sharing only read is what silently starved the
    // second writer of combat.log: attack_input_port could not get a handle at all once
    // simple_combat held one. With FILE_APPEND_DATA each WriteFile lands at the end, so two
    // handles on one file interleave lines instead of overwriting each other.
    return CreateFileW(full.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// ANSI convenience for the features that build their paths with snprintf.
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
