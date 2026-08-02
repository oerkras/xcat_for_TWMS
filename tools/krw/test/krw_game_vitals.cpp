// Read Classic TWMS HP/MP via KRW only (no XCat). Resolves CharacterStat by:
//   --cs / --wm  explicit, or auto-scan private RW heaps for CS vitals pattern + WM chain.
#include "../client/xcat_krw_compat.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint64_t kOffWmMyUser = 0x28;
constexpr std::uint64_t kOffWmSceneState = 0x34;
constexpr std::uint64_t kOffWmCharacterData = 0xE0;
constexpr std::uint64_t kOffCdCharacterStat = 0x10;
constexpr std::uint64_t kOffCsName = 0x18;
constexpr std::uint64_t kOffCsLevel = 0x38;
constexpr std::uint64_t kOffCsJob = 0x3A;
constexpr std::uint64_t kOffCsHp = 0x44;
constexpr std::uint64_t kOffCsMhp = 0x46;
constexpr std::uint64_t kOffCsMp = 0x48;
constexpr std::uint64_t kOffCsMmp = 0x4A;
constexpr std::uint64_t kOffBarCharacterStat = 0x220;  // UIStatusBar

struct Vitals {
  std::uint64_t cs = 0;
  std::uint64_t wm = 0;
  std::uint64_t cd = 0;
  std::uint8_t level = 0;
  std::int16_t job = 0;
  std::int16_t hp = 0;
  std::int16_t mhp = 0;
  std::int16_t mp = 0;
  std::int16_t mmp = 0;
  char name[64]{};
  int score = 0;
};

bool EnableDebugPrivilege() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
    return false;
  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
    CloseHandle(tok);
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = luid;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  const BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  CloseHandle(tok);
  return ok && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

std::uint32_t FindPidByExe(const wchar_t* exe) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  std::uint32_t pid = 0;
  if (Process32FirstW(snap, &pe)) {
    do {
      if (_wcsicmp(pe.szExeFile, exe) == 0) {
        pid = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  return pid;
}

bool KrwRead(DWORD pid, UINT64 addr, void* buf, DWORD size, bool use_phys) {
  DWORD n = 0;
  return Read(pid, addr, reinterpret_cast<UINT64>(buf), size, n, use_phys ? TRUE : FALSE) &&
         n == size;
}

bool LooksHeap(std::uint64_t v) {
  return v >= 0x10000ULL && v < 0x00007FFFFFFFFFFFULL;
}

bool ReadIl2CppName(DWORD pid, std::uint64_t str_obj, char* out, size_t out_len, bool use_phys) {
  if (!LooksHeap(str_obj) || !out || out_len < 2) return false;
  std::int32_t len = 0;
  if (!KrwRead(pid, str_obj + 0x10, &len, sizeof(len), use_phys) || len <= 0 || len > 48) return false;
  // UTF-16 chars at +0x14
  std::vector<wchar_t> w(static_cast<size_t>(len) + 1);
  if (!KrwRead(pid, str_obj + 0x14, w.data(), static_cast<DWORD>(len * 2), use_phys)) return false;
  w[static_cast<size_t>(len)] = L'\0';
  WideCharToMultiByte(CP_UTF8, 0, w.data(), -1, out, static_cast<int>(out_len), nullptr, nullptr);
  return out[0] != 0;
}

bool NameLooksOk(const char* name) {
  if (!name || !name[0]) return false;
  const size_t n = std::strlen(name);
  if (n < 2 || n > 13) return false;  // Classic name length band
  int printable = 0;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c >= 0x20 && c != 0x7F) ++printable;
  }
  return printable == static_cast<int>(n);
}

bool VitalsPlausible(std::uint8_t level, std::int16_t job, std::int16_t hp, std::int16_t mhp,
                     std::int16_t mp, std::int16_t mmp) {
  if (level < 1 || level > 300) return false;
  if (mhp < 10 || mhp > 32000) return false;
  if (hp < 0 || hp > mhp) return false;
  if (mmp < 1 || mmp > 32000) return false;  // local player always has MP pool
  if (mp < 0 || mp > mmp) return false;
  if (job < 0 || job > 10000) return false;
  // Reject lv1 / low-level with absurd HP (common false positive).
  const int cap = static_cast<int>(level) * 800 + 400;
  if (mhp > cap) return false;
  if (job == 0 && level <= 15 && mhp > 800) return false;
  return true;
}

bool WmLooksAlive(DWORD pid, std::uint64_t wm, bool use_phys) {
  std::int32_t scene = -1;
  if (!KrwRead(pid, wm + kOffWmSceneState, &scene, sizeof(scene), use_phys)) return false;
  if (scene < 0 || scene > 5) return false;
  std::uint64_t my_user = 0;
  if (!KrwRead(pid, wm + kOffWmMyUser, &my_user, sizeof(my_user), use_phys) || !LooksHeap(my_user))
    return false;
  std::uint64_t cd = 0;
  if (!KrwRead(pid, wm + kOffWmCharacterData, &cd, sizeof(cd), use_phys) || !LooksHeap(cd))
    return false;
  return true;
}

bool ReadVitalsAtCs(DWORD pid, std::uint64_t cs, Vitals* out, bool use_phys) {
  if (!LooksHeap(cs) || (cs & 7) != 0) return false;
  std::uint64_t klass = 0;
  if (!KrwRead(pid, cs, &klass, sizeof(klass), use_phys) || !LooksHeap(klass)) return false;

  std::uint8_t level = 0;
  std::int16_t job = 0, hp = 0, mhp = 0, mp = 0, mmp = 0;
  if (!KrwRead(pid, cs + kOffCsLevel, &level, 1, use_phys)) return false;
  if (!KrwRead(pid, cs + kOffCsJob, &job, 2, use_phys)) return false;
  if (!KrwRead(pid, cs + kOffCsHp, &hp, 2, use_phys)) return false;
  if (!KrwRead(pid, cs + kOffCsMhp, &mhp, 2, use_phys)) return false;
  if (!KrwRead(pid, cs + kOffCsMp, &mp, 2, use_phys)) return false;
  if (!KrwRead(pid, cs + kOffCsMmp, &mmp, 2, use_phys)) return false;
  if (!VitalsPlausible(level, job, hp, mhp, mp, mmp)) return false;

  out->cs = cs;
  out->wm = 0;
  out->cd = 0;
  out->level = level;
  out->job = job;
  out->hp = hp;
  out->mhp = mhp;
  out->mp = mp;
  out->mmp = mmp;
  out->name[0] = 0;
  std::uint64_t name_obj = 0;
  if (KrwRead(pid, cs + kOffCsName, &name_obj, sizeof(name_obj), use_phys) && LooksHeap(name_obj)) {
    ReadIl2CppName(pid, name_obj, out->name, sizeof(out->name), use_phys);
  }
  if (!NameLooksOk(out->name)) return false;  // auto path: must have real char name

  out->score = 30;
  if (mmp > 0 && mp >= 0) out->score += 5;
  // Prefer lower mhp for given level (local beginner vs inflated garbage).
  out->score += 20 - (std::min)(20, mhp / (50 * level));
  return true;
}

bool ResolveFromWm(DWORD pid, std::uint64_t wm, Vitals* out, bool use_phys) {
  if (!WmLooksAlive(pid, wm, use_phys)) return false;
  std::uint64_t cd = 0, cs = 0;
  if (!KrwRead(pid, wm + kOffWmCharacterData, &cd, sizeof(cd), use_phys) || !LooksHeap(cd))
    return false;
  if (!KrwRead(pid, cd + kOffCdCharacterStat, &cs, sizeof(cs), use_phys) || !LooksHeap(cs))
    return false;
  if (!ReadVitalsAtCs(pid, cs, out, use_phys)) return false;
  out->wm = wm;
  out->cd = cd;
  out->score += 80;
  std::int32_t scene = 0;
  if (KrwRead(pid, wm + kOffWmSceneState, &scene, sizeof(scene), use_phys) && scene == 3)
    out->score += 20;  // MapScene
  return true;
}

void Consider(std::vector<Vitals>& top, const Vitals& v, size_t keep) {
  if (v.score < 0) return;
  for (const auto& e : top) {
    if (e.cs == v.cs) return;
  }
  top.push_back(v);
  std::sort(top.begin(), top.end(),
            [](const Vitals& a, const Vitals& b) { return a.score > b.score; });
  if (top.size() > keep) top.resize(keep);
}

void PrintVitals(const Vitals& v) {
  std::printf("cs=0x%llx", static_cast<unsigned long long>(v.cs));
  if (v.wm) std::printf(" wm=0x%llx cd=0x%llx", static_cast<unsigned long long>(v.wm),
                        static_cast<unsigned long long>(v.cd));
  std::printf(" name=%s lv=%u job=%d HP=%d/%d MP=%d/%d score=%d\n", v.name[0] ? v.name : "?",
              static_cast<unsigned>(v.level), static_cast<int>(v.job), static_cast<int>(v.hp),
              static_cast<int>(v.mhp), static_cast<int>(v.mp), static_cast<int>(v.mmp), v.score);
}

bool ScanHeapsForCs(DWORD pid, Vitals* best, bool use_phys) {
  HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
  if (!proc) {
    std::printf("OpenProcess(QUERY_INFORMATION) err=%lu - pass --wm/--cs\n", GetLastError());
    return false;
  }

  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  std::uint64_t addr = reinterpret_cast<std::uint64_t>(si.lpMinimumApplicationAddress);
  const std::uint64_t max_addr = reinterpret_cast<std::uint64_t>(si.lpMaximumApplicationAddress);

  constexpr DWORD kChunk = 0x40000;
  std::vector<std::uint8_t> buf(kChunk);
  int regions = 0, raw_hits = 0, named = 0;
  std::vector<Vitals> top;

  std::printf("Scanning private RW heaps via KRW (named CharacterStat only)...\n");

  while (addr < max_addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQueryEx(proc, reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) break;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
    const std::uint64_t size = static_cast<std::uint64_t>(mbi.RegionSize);
    const std::uint64_t next = base + size;

    const bool interesting = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                             (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE) &&
                             size >= 0x1000 && size <= 0x4000000ULL;
    if (interesting) {
      ++regions;
      for (std::uint64_t off = 0; off + 0x50 < size;) {
        const DWORD want =
            static_cast<DWORD>(size - off > kChunk ? kChunk : size - off);
        const std::uint64_t chunk_va = base + off;
        if (!KrwRead(pid, chunk_va, buf.data(), want, use_phys)) {
          off += want;
          continue;
        }
        const DWORD limit = want >= 0x230 ? want - 0x230 : (want >= 0x50 ? want - 0x50 : 0);
        for (DWORD i = 0; i < limit; i += 8) {
          const std::uint64_t cs = chunk_va + i;
          const auto* p = buf.data() + i;
          const std::uint64_t klass = *reinterpret_cast<const std::uint64_t*>(p);
          if (!LooksHeap(klass)) continue;
          const std::uint8_t level = p[kOffCsLevel];
          const std::int16_t mhp = *reinterpret_cast<const std::int16_t*>(p + kOffCsMhp);
          const std::int16_t hp = *reinterpret_cast<const std::int16_t*>(p + kOffCsHp);
          const std::int16_t mmp = *reinterpret_cast<const std::int16_t*>(p + kOffCsMmp);
          const std::int16_t mp = *reinterpret_cast<const std::int16_t*>(p + kOffCsMp);
          const std::int16_t job = *reinterpret_cast<const std::int16_t*>(p + kOffCsJob);
          if (!VitalsPlausible(level, job, hp, mhp, mp, mmp)) continue;
          ++raw_hits;

          Vitals v{};
          if (!ReadVitalsAtCs(pid, cs, &v, use_phys)) continue;
          ++named;

          // UIStatusBar → CS(+0x220): if this object is a bar pointing at same CS, boost.
          // Also: scan chunk for qword==cs at bar+0x220 => bar = hit - 0x220
          for (DWORD j = 0; j + 8 <= want; j += 8) {
            if (*reinterpret_cast<const std::uint64_t*>(buf.data() + j) != cs) continue;
            const std::uint64_t maybe_bar = chunk_va + j - kOffBarCharacterStat;
            if (LooksHeap(maybe_bar) && (maybe_bar & 7) == 0) {
              std::uint64_t bar_klass = 0;
              if (KrwRead(pid, maybe_bar, &bar_klass, 8, use_phys) && LooksHeap(bar_klass)) {
                v.score += 40;
                break;
              }
            }
          }

          for (DWORD j = 0; j + 8 <= want; j += 8) {
            if (*reinterpret_cast<const std::uint64_t*>(buf.data() + j) != cs) continue;
            const std::uint64_t cd = chunk_va + j - kOffCdCharacterStat;
            if (!LooksHeap(cd) || (cd & 7) != 0) continue;
            for (DWORD k = 0; k + 8 <= want; k += 8) {
              if (*reinterpret_cast<const std::uint64_t*>(buf.data() + k) != cd) continue;
              const std::uint64_t wm = chunk_va + k - kOffWmCharacterData;
              if (!LooksHeap(wm) || (wm & 7) != 0) continue;
              Vitals chained{};
              if (ResolveFromWm(pid, wm, &chained, use_phys) && chained.cs == cs) {
                v = chained;
                break;
              }
            }
            if (v.wm) break;
          }

          Consider(top, v, 8);
        }
        off += (limit ? limit : want);
      }
    }
    if (next <= addr) break;
    addr = next;
  }

  CloseHandle(proc);
  std::printf("scan regions=%d raw_hits=%d named=%d\n", regions, raw_hits, named);
  if (top.empty()) return false;
  std::printf("top candidates:\n");
  for (size_t i = 0; i < top.size(); ++i) {
    std::printf("  #%zu ", i + 1);
    PrintVitals(top[i]);
  }
  *best = top[0];
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  bool use_phys = false;
  bool watch = false;
  int interval_ms = 500;
  std::uint32_t pid = 0;
  std::uint64_t wm = 0;
  std::uint64_t cs = 0;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--phys") == 0) {
      use_phys = true;
    } else if (std::strcmp(argv[i], "--watch") == 0) {
      watch = true;
    } else if (std::strcmp(argv[i], "--ms") == 0 && i + 1 < argc) {
      interval_ms = static_cast<int>(std::strtoul(argv[++i], nullptr, 0));
      if (interval_ms < 50) interval_ms = 50;
    } else if (std::strcmp(argv[i], "--wm") == 0 && i + 1 < argc) {
      wm = std::strtoull(argv[++i], nullptr, 0);
    } else if (std::strcmp(argv[i], "--cs") == 0 && i + 1 < argc) {
      cs = std::strtoull(argv[++i], nullptr, 0);
    } else if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
      pid = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    } else if (argv[i][0] != '-') {
      pid = static_cast<std::uint32_t>(std::strtoul(argv[i], nullptr, 0));
    } else {
      std::printf(
          "Usage: krw_game_vitals.exe [--watch] [--ms N] [--phys] [--wm VA] [--cs VA] [--pid N]\n"
          "  No XCat. Auto-scan heaps for CharacterStat; prefer --wm if known.\n");
      return 1;
    }
  }

  EnableDebugPrivilege();
  if (pid == 0) pid = FindPidByExe(L"Maplestory_Classic.exe");
  if (pid == 0) {
    std::printf("Maplestory_Classic.exe not running\n");
    return 1;
  }
  std::printf("pid=%u\n", pid);

  if (!Init(0x7654321)) {
    std::printf("Init failed\n");
    return 2;
  }

  Vitals v{};
  bool ok = false;
  if (cs) {
    ok = ReadVitalsAtCs(pid, cs, &v, use_phys);
  } else if (wm) {
    ok = ResolveFromWm(pid, wm, &v, use_phys);
  } else {
    ok = ScanHeapsForCs(pid, &v, use_phys);
  }

  if (!ok) {
    std::printf("vitals resolve FAIL (in-map? try --wm from CE)\n");
    UnInit();
    return 3;
  }

  PrintVitals(v);

  if (watch) {
    std::printf("watching (Ctrl+C to stop)...\n");
    for (;;) {
      Vitals cur{};
      bool live = false;
      if (v.wm) live = ResolveFromWm(pid, v.wm, &cur, use_phys);
      if (!live) live = ReadVitalsAtCs(pid, v.cs, &cur, use_phys);
      if (live) {
        if (cur.hp != v.hp || cur.mp != v.mp || cur.mhp != v.mhp || cur.mmp != v.mmp) {
          PrintVitals(cur);
          v = cur;
        }
      } else {
        std::printf("read lost - rescanning...\n");
        if (ScanHeapsForCs(pid, &v, use_phys)) PrintVitals(v);
        else Sleep(1000);
      }
      Sleep(static_cast<DWORD>(interval_ms));
    }
  }

  UnInit();
  std::printf("vitals OK\n");
  return 0;
}
