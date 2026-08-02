// Watch Classic TWMS CharacterStat HP/MHP via XCatKrw compat (ETW-only).
// Offsets: CS+0x44 = int16 hp, CS+0x46 = int16 mhp.
// Chain: WorldManager → CharacterData(+0xE0) → CharacterStat(+0x10).

#include "../client/xcat_krw_compat.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace {

constexpr std::uint64_t kOffWmCharacterData = 0xE0;
constexpr std::uint64_t kOffCdCharacterStat = 0x10;
constexpr std::uint64_t kOffCsHp = 0x44;
constexpr std::uint64_t kOffCsMhp = 0x46;
constexpr std::uint64_t kOffCsLevel = 0x38;

void Usage() {
  std::printf(
      "Usage:\n"
      "  krw_hp_watch.exe --auto [--phys] [--ms N]\n"
      "  krw_hp_watch.exe <pid> --wm <WorldManagerVA> [...]\n"
      "  krw_hp_watch.exe <pid> <CharacterStatVA> [...]\n"
      "  ETW-only Init(0x7654321); no Device/IOCTL\n");
}

std::uint64_t ParseU64(const char* s) {
  if (!s || !*s) return 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 0);
  if (end == s) return 0;
  return static_cast<std::uint64_t>(v);
}

std::uint32_t FindGamePid() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  std::uint32_t pid = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, L"Maplestory_Classic.exe") == 0) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return pid;
}

std::string ExeDir() {
  char path[MAX_PATH]{};
  const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  std::string s(path, path + n);
  const auto slash = s.find_last_of("\\/");
  if (slash == std::string::npos) return {};
  return s.substr(0, slash + 1);
}

struct WmPick {
  std::uint64_t wm = 0;
  bool from_in_map = false;
  int last_scene = -1;
  int last_in_map = -1;
  const char* source = "none";
  std::uint32_t live_pid = 0;
};

// Prefer xcat-published live pointer (requires injected payload in-map).
WmPick LatestWmFromLiveFile(std::uint32_t expect_pid) {
  WmPick out{};
  const std::string path = ExeDir() + "..\\XCat_data\\state\\wm_live.txt";
  std::ifstream in(path);
  if (!in) return out;
  std::string line;
  std::uint32_t pid = 0;
  std::uint64_t wm = 0;
  int in_map = 0;
  while (std::getline(in, line)) {
    if (line.rfind("pid=", 0) == 0) pid = static_cast<std::uint32_t>(ParseU64(line.c_str() + 4));
    else if (line.rfind("wm=", 0) == 0) wm = ParseU64(line.c_str() + 3);
    else if (line.rfind("inMap=", 0) == 0) in_map = static_cast<int>(ParseU64(line.c_str() + 6));
  }
  if (!wm || in_map != 1) return out;
  if (expect_pid && pid && pid != expect_pid) {
    std::printf("wm_live.txt pid=%u != game pid=%u (stale file)\n", pid, expect_pid);
    return out;
  }
  out.wm = wm;
  out.from_in_map = true;
  out.last_in_map = 1;
  out.live_pid = pid;
  out.source = "wm_live.txt";
  return out;
}

// Prefer last wm= that sits on a line with inMap=1 (in-map). Fall back to any last wm=.
WmPick LatestWmFromLog() {
  WmPick out{};
  out.source = "x.jsonl";
  const std::string base = ExeDir() + "..\\XCat_data\\logs\\";
  const char* names[] = {"x.jsonl", "x.jsonl.1"};
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  if (GetFileAttributesExA((base + "x.jsonl").c_str(), GetFileExInfoStandard, &fad)) {
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a{}, b{};
    a.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    a.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime;
    b.HighPart = now.dwHighDateTime;
    const ULONGLONG age_s = (b.QuadPart - a.QuadPart) / 10000000ULL;
    if (age_s > 60) {
      std::printf("warn: x.jsonl age=%llus (payload may be dead; prefer reinject)\n",
                  static_cast<unsigned long long>(age_s));
    }
  }
  for (const char* name : names) {
    std::ifstream in(base + name, std::ios::binary);
    if (!in) continue;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::uint64_t last_any = 0;
    std::uint64_t last_in_map = 0;
    int scene = -1;
    int in_map = -1;
    for (size_t pos = 0;;) {
      const size_t p = content.find("wm=", pos);
      if (p == std::string::npos) break;
      size_t line = content.rfind('\n', p);
      line = (line == std::string::npos) ? 0 : line + 1;
      const size_t line_end = content.find('\n', p);
      const size_t line_len =
          (line_end == std::string::npos ? content.size() : line_end) - line;
      const std::string row = content.substr(line, line_len);

      auto parse_tag = [&](const char* tag) -> int {
        const size_t t = row.find(tag);
        if (t == std::string::npos) return -1;
        char* end = nullptr;
        const long v = std::strtol(row.c_str() + t + std::strlen(tag), &end, 10);
        if (end == row.c_str() + t + std::strlen(tag)) return -1;
        return static_cast<int>(v);
      };
      scene = parse_tag("scene=");
      in_map = parse_tag("inMap=");

      pos = p + 3;
      char* end = nullptr;
      const unsigned long long v = std::strtoull(content.c_str() + pos, &end, 16);
      if (end == content.c_str() + pos || v == 0) continue;
      last_any = static_cast<std::uint64_t>(v);
      if (in_map == 1) last_in_map = last_any;
    }
    out.last_scene = scene;
    out.last_in_map = in_map;
    if (last_in_map) {
      out.wm = last_in_map;
      out.from_in_map = true;
      return out;
    }
    if (last_any) {
      out.wm = last_any;
      out.from_in_map = false;
      return out;
    }
  }
  return out;
}

WmPick ResolveAutoWm(std::uint32_t pid) {
  WmPick live = LatestWmFromLiveFile(pid);
  if (live.wm) return live;
  return LatestWmFromLog();
}

bool CompatRead(DWORD pid, UINT64 addr, void* buf, DWORD size, bool use_phys) {
  DWORD n = 0;
  return Read(pid, addr, reinterpret_cast<UINT64>(buf), size, n, use_phys ? TRUE : FALSE) &&
         n == size;
}

bool ResolveCs(std::uint32_t pid, std::uint64_t wm, bool use_phys, std::uint64_t* out_cs) {
  std::int32_t scene = -1;
  if (CompatRead(pid, wm + 0x34, &scene, sizeof(scene), use_phys)) {
    std::printf("wm+0x34 scene=%d (3=in-map typically)\n", static_cast<int>(scene));
  } else {
    std::printf("wm+0x34 scene read FAIL - wm may be stale vs this pid\n");
  }

  std::uint64_t cd = 0;
  if (!CompatRead(pid, wm + kOffWmCharacterData, &cd, sizeof(cd), use_phys)) {
    std::printf("Resolve: read CD @ wm+0xE0 FAIL\n");
    return false;
  }
  if (cd < 0x10000) {
    std::printf("Resolve: CD null/low (0x%llx) — not in play or bad wm\n",
                static_cast<unsigned long long>(cd));
    return false;
  }
  std::uint64_t cs = 0;
  if (!CompatRead(pid, cd + kOffCdCharacterStat, &cs, sizeof(cs), use_phys)) {
    std::printf("Resolve: read CS @ cd+0x10 FAIL (cd=0x%llx)\n",
                static_cast<unsigned long long>(cd));
    return false;
  }
  if (cs < 0x10000) {
    std::printf("Resolve: CS null/low (0x%llx)\n", static_cast<unsigned long long>(cs));
    return false;
  }
  std::uint8_t lv = 0;
  std::int16_t mhp = 0;
  if (!CompatRead(pid, cs + kOffCsLevel, &lv, 1, use_phys)) {
    std::printf("Resolve: read lv @ cs+0x38 FAIL (cs=0x%llx)\n",
                static_cast<unsigned long long>(cs));
    return false;
  }
  if (lv < 1) {
    std::printf("Resolve: lv=%u invalid (cs=0x%llx)\n", static_cast<unsigned>(lv),
                static_cast<unsigned long long>(cs));
    return false;
  }
  if (!CompatRead(pid, cs + kOffCsMhp, &mhp, 2, use_phys)) {
    std::printf("Resolve: read mhp @ cs+0x46 FAIL\n");
    return false;
  }
  if (mhp <= 0) {
    std::printf("Resolve: mhp=%d invalid\n", static_cast<int>(mhp));
    return false;
  }
  std::printf("resolved wm=0x%llx cd=0x%llx cs=0x%llx lv=%u mhp=%d\n",
              static_cast<unsigned long long>(wm), static_cast<unsigned long long>(cd),
              static_cast<unsigned long long>(cs), static_cast<unsigned>(lv), static_cast<int>(mhp));
  *out_cs = cs;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool use_phys = false;
  bool auto_mode = false;
  bool use_wm = false;
  int interval_ms = 200;
  std::uint32_t pid = 0;
  std::uint64_t cs_arg = 0;
  std::uint64_t wm_arg = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--auto") == 0) {
      auto_mode = true;
    } else if (std::strcmp(argv[i], "--phys") == 0) {
      use_phys = true;
    } else if (std::strcmp(argv[i], "--ms") == 0 && i + 1 < argc) {
      interval_ms = static_cast<int>(ParseU64(argv[++i]));
      if (interval_ms < 50) interval_ms = 50;
    } else if (std::strcmp(argv[i], "--wm") == 0 && i + 1 < argc) {
      use_wm = true;
      wm_arg = ParseU64(argv[++i]);
    } else if (pid == 0 && argv[i][0] != '-') {
      pid = static_cast<std::uint32_t>(ParseU64(argv[i]));
    } else if (!use_wm && cs_arg == 0 && argv[i][0] != '-') {
      cs_arg = ParseU64(argv[i]);
    } else {
      Usage();
      return 1;
    }
  }

  if (auto_mode) {
    pid = FindGamePid();
    const WmPick pick = ResolveAutoWm(pid);
    wm_arg = pick.wm;
    use_wm = true;
    if (pid == 0) {
      std::printf("Maplestory_Classic.exe not running\n");
      return 1;
    }
    if (wm_arg == 0) {
      std::printf("no wm= (need wm_live.txt from injected xcat, or x.jsonl)\n");
      return 1;
    }
    std::printf("auto pid=%u wm=0x%llx src=%s (prefer_inMap1=%d log_last scene=%d inMap=%d)\n",
                pid, static_cast<unsigned long long>(wm_arg), pick.source,
                pick.from_in_map ? 1 : 0, pick.last_scene, pick.last_in_map);
    if (!pick.from_in_map) {
      std::printf("note: no inMap=1 wm; last line may be lobby/transition - CD often null\n");
    }
  }

  if (pid == 0 || (!use_wm && cs_arg == 0) || (use_wm && wm_arg == 0 && !auto_mode)) {
    Usage();
    return 1;
  }

  if (!Init(0x7654321)) {
    std::printf("Init failed (ETW covert required; no IOCTL fallback)\n");
    return 2;
  }

  std::uint64_t cs = cs_arg;
  if (use_wm) {
    if (!ResolveCs(pid, wm_arg, use_phys, &cs)) {
      std::printf("Resolve CS failed\n");
      UnInit();
      return 3;
    }
  }

  std::printf("pid=%u cs=0x%llx path=%s interval=%dms (Ctrl+C to stop)\n", pid,
              static_cast<unsigned long long>(cs), use_phys ? "phys" : "cr3", interval_ms);

  std::int16_t last_hp = -1;
  std::int16_t last_mhp = -1;
  for (;;) {
    if (auto_mode) {
      const auto live = FindGamePid();
      if (live == 0) {
        std::printf("game exited\n");
        UnInit();
        return 4;
      }
      if (live != pid) {
        pid = live;
        std::printf("pid changed -> %u\n", pid);
      }
    }

    std::int16_t hp = 0;
    std::int16_t mhp = 0;
    if (!CompatRead(pid, cs + kOffCsHp, &hp, sizeof(hp), use_phys) ||
        !CompatRead(pid, cs + kOffCsMhp, &mhp, sizeof(mhp), use_phys) || mhp <= 0) {
      std::printf("Read HP failed — retry resolve\n");
      if (auto_mode || use_wm) {
        const std::uint64_t wm = auto_mode ? ResolveAutoWm(pid).wm : wm_arg;
        std::uint64_t neu = 0;
        if (wm && ResolveCs(pid, wm, use_phys, &neu)) {
          cs = neu;
          wm_arg = wm;
        }
      }
      Sleep(static_cast<DWORD>(interval_ms));
      continue;
    }

    if (hp != last_hp || mhp != last_mhp) {
      const int pct = static_cast<int>((static_cast<std::int64_t>(hp) * 100) / mhp);
      std::printf("HP %d/%d (%d%%)\n", static_cast<int>(hp), static_cast<int>(mhp), pct);
      last_hp = hp;
      last_mhp = mhp;
    }
    Sleep(static_cast<DWORD>(interval_ms));
  }
}
