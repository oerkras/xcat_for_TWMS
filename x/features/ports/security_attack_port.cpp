// security_attack_port — SecurityClient 攻包计数窗（只读观测）。
// 不写字典、不 Hook GA .text；热更时若发现旧 ret 补丁则还原。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "security_attack_port.h"

#include "world_port.h"
#include "mob_gather_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x::features::ports::security_attack {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;
namespace world = x::features::ports::world;

// BIN：bin/XCat_data/logs/sec_attack.log（AppendDbgLog 会话轮转 + 512KiB）。
// 观测心跳只写这份；x.jsonl 仍 5s 节流。
void BinLine(const char* fmt, ...) {
    char body[900]{};
    va_list ap;
    va_start(ap, fmt);
    const int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn <= 0) return;

    const char* bin = x::runtime::GetBinDir();
    if (!bin || !bin[0]) return;
    char dir[MAX_PATH]{};
    snprintf(dir, sizeof(dir), "%slogs", bin);
    CreateDirectoryA(dir, nullptr);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1024]{};
    const int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute,
                           st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    (void)x::runtime::AppendDbgLogA(dir, "sec_attack.log", line, static_cast<DWORD>(n));
}

int32_t ReadI32Local(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// TW TypeDef 15147 — SecurityClient 攻包窗；resolve：il2cpp_shape::ResolveSecAttackKlass
// （hash e439d992… + static Dict@0/8 + I32@0x10；旧 ba499947… 已作废）

// Remount 2026-08-06：TDI 15147 字段 ACS 重哈希；偏移仍 0/8/0x10；klass cf990184… 未漂。
constexpr char kHashPktDict[] =
    "cd1587bb2d14b1cf8f74351df8611ee9b4465cc77182c331dea286c6aae0840";  // Dictionary<ushort,int>
constexpr char kHashSkillDict[] =
    "f3ce5950e5ba685be43ec79f66bbdb0120f85d6217d4600c4f12875fbbc8e06";  // Dictionary<int,int>
constexpr char kHashDetectTime[] =
    "f731cfbda82df190c1057570d40cc5371dc85644aaedda2fd46a0673d7ff96c";
constexpr size_t kFbPktDict = 0x0;
constexpr size_t kFbSkillDict = 0x8;
constexpr size_t kFbDetectTime = 0x10;
size_t gOffPktDict = kFbPktDict;
size_t gOffSkillDict = kFbSkillDict;
size_t gOffDetectTime = kFbDetectTime;
bool gFieldOffTried = false;

// Dictionary`2 / Entry / Array → il2cpp_container SSOT（勿把 freeCount 读成 0x2C=_version）
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictFreeCount (x::runtime::il2cpp_container::OffDictFreeCount())
#define kOffDictVersion (x::runtime::il2cpp_container::OffDictVersion())
#define kEntryStrideTight (x::runtime::il2cpp_container::DictEntryStrideIntIntTight())
#define kEntryStrideAlign (x::runtime::il2cpp_container::DictEntryStrideIntIntAlign())
#define kOffArrLen (x::runtime::il2cpp_container::OffArrayMaxLength())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())
#define kValOffTight (x::runtime::il2cpp_container::OffDictEntryValueIntTight())
#define kValOffAlign (x::runtime::il2cpp_container::OffDictEntryValueIntAlign())
#define kOffDictEntryKey (x::runtime::il2cpp_container::OffDictEntryKey())

constexpr DWORD kRebindMs = 5000;

using FnClassStaticData = void* (*)(void* klass);

HMODULE gGA = nullptr;
FnClassStaticData gClassStaticData = nullptr;
void* gKlass = nullptr;
void* gStatic = nullptr;
DWORD gLastBindMs = 0;
std::atomic<bool> gReady{false};

// type20 只读观测：worker 周期采字典。
constexpr DWORD kSampleMs = 200;
constexpr DWORD kBinProbeMs = 1000;    // BIN 心跳；峰值上涨 / 清窗立刻写
std::atomic<bool> gProbeStop{false};
std::atomic<HANDLE> gProbeWorker{nullptr};
std::atomic<int> gWindowHighPeak{0};
std::atomic<int> gWindowHighPktSum{0};
std::atomic<int> gWindowHighSkillSum{0};
std::atomic<DWORD> gSkipRaiseUntil{0};
std::atomic<uint8_t> gSessResetBin{0};
int gSampleDetectTime = 0;
bool gSampleDetectInit = false;
DWORD gLastSampleMs = 0;
DWORD gLastBinProbeMs = 0;
int gLastBinnedHigh = -1;

void SampleTick(DWORD now);
void RaiseWindowHighs(const WindowSnapshot& snap);
void UndoStaleTextHook();

bool PlausibleStaticOff(size_t off) { return off < 0x40; }

bool FieldOffHit(void* klass, const char* hash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !hash || !x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        field = nullptr;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    // 静态槽偶发返回非 0..0x3F 的编码；名已命中则用 dump fb（布局未漂）
    if (PlausibleStaticOff(off)) {
        *out = off;
    } else {
        *out = fb;
    }
    return true;
}

void EnsureStaticFieldOff() {
    constexpr int kExpect = 3;
    static int sLastHits = -1;
    if (gFieldOffTried && sLastHits >= kExpect) return;
    if (!gKlass) {
        gKlass = x::runtime::il2cpp_shape::ResolveSecAttackKlass();
        if (!gKlass) {
            gKlass = x::runtime::il2cpp::FindClass(
                "", "d856fdab82ec7edbec7bafdff562821eb524fe938c019a176509b60ced547a9");
        }
    }
    if (!gKlass) return;
    int hits = 0;
    if (FieldOffHit(gKlass, kHashPktDict, kFbPktDict, &gOffPktDict)) ++hits;
    if (FieldOffHit(gKlass, kHashSkillDict, kFbSkillDict, &gOffSkillDict)) ++hits;
    if (FieldOffHit(gKlass, kHashDetectTime, kFbDetectTime, &gOffDetectTime)) ++hits;
    gFieldOffTried = true;
    if (hits != sLastHits) {
        sLastHits = hits;
        runtime::LogI("SecAttack",
                      "static fields path=%s hits=%d/3 pkt=0x%zX skill=0x%zX detect=0x%zX",
                      hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits,
                      gOffPktDict, gOffSkillDict, gOffDetectTime);
    }
}

struct DictScan {
    bool headerOk = false;  // 读到合法 Dictionary 头（含空表）
    bool summed = false;    // 扫出至少 1 个存活 entry，或确认空表
    int sum = 0;
    int keys = 0;
    int peak = 0;  // 单键最大值（type20 按键判定）
    int peakId = 0;  // 峰值对应的 dict key（opcode 或 skillId）
    int count = 0;
    int freeCount = 0;
    int version = 0;
    int entriesLen = 0;
};

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    if (gClassStaticData) {
        __try {
            void* p = gClassStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

void ScanEmpty(DictScan* out) {
    if (!out) return;
    out->headerOk = true;
    out->summed = true;
    out->sum = 0;
    out->keys = 0;
    out->peak = 0;
    out->peakId = 0;
}

// 合计 Dictionary 中 hash>=0 的 int value（ushort/int 与 int/int 同 stride 紧凑布局）。
// 以 entries[] 扫描为主（对齐 skill_port）；freeCount 仅作空表提示，不因 version 误读提前退出。
DictScan SumDictIntValues(void* dict) {
    DictScan out{};
    if (!LooksLikeHeapPtr(dict)) return out;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);

    void* entries = nullptr;
    int count = 0;
    int freeCount = 0;
    int version = 0;
    __try {
        entries = ReadPtr(dict, kOffDictEntries);
        count = ReadI32Local(dict, kOffDictCount);
        freeCount = ReadI32Local(dict, kOffDictFreeCount);
        version = ReadI32Local(dict, kOffDictVersion);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return out;
    }
    out.count = count;
    out.freeCount = freeCount;
    out.version = version;

    if (count < 0 || count > 65536 || freeCount < 0 || freeCount > 65536) return out;

    // 未扩容 / 空表：entries 可为 null。
    if (!entries) {
        if (count == 0) ScanEmpty(&out);
        return out;
    }
    if (!LooksLikeHeapPtr(entries)) return out;

    int len = 0;
    __try {
        len = ReadI32Local(entries, kOffArrLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return out;
    }
    out.entriesLen = len;
    out.headerOk = true;

    const int liveHint = count - freeCount;
    // count==0 或 free 槽吃光：确认空表（仍可能有容量 len>0）。
    if (count == 0 || liveHint == 0) {
        ScanEmpty(&out);
        return out;
    }
    // 容量异常：头可读，但不假装已 sum。
    if (len <= 0 || len > 65536) return out;

    auto tryStride = [&](size_t stride, size_t valOff) -> bool {
        int sum = 0;
        int keys = 0;
        int peak = 0;
        int peakId = 0;
        int seen = 0;
        __try {
            for (int i = 0; i < len && i < 8192; ++i) {
                uint8_t* e = x::runtime::il2cpp_container::DictEntryAt(entries, i, stride);
                if (!e) continue;
                const int hash = *reinterpret_cast<int*>(e + 0);
                if (hash < 0) continue;
                const int val = *reinterpret_cast<int*>(e + valOff);
                // 计数应为非负；单键上限放宽到 CHECK_COUNT*4 防脏读
                if (val < 0 || val > kCheckCount * 4) continue;
                const int key = *reinterpret_cast<int*>(e + kOffDictEntryKey);
                sum += val;
                ++keys;
                ++seen;
                if (val > peak) {
                    peak = val;
                    peakId = key;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        (void)seen;
        // 槽位全是 tombstone / 未占用 → 空表
        if (keys == 0) {
            ScanEmpty(&out);
            return true;
        }
        out.sum = sum;
        out.keys = keys;
        out.peak = peak;
        out.peakId = peakId;
        out.summed = true;
        return true;
    };

    if (tryStride(kEntryStrideTight, kValOffTight)) return out;
    if (tryStride(kEntryStrideAlign, kValOffAlign)) return out;
    // liveHint 可疑（例如曾误读 version）时仍可能扫成功；到这里才算布局未命中
    return out;
}

bool Bind() {
    const DWORD now = GetTickCount();
    if (gReady.load() && gStatic && now - gLastBindMs < kRebindMs) return true;
    gLastBindMs = now;
    if (!x::runtime::il2cpp::Ensure()) {
        gReady.store(false);
        return false;
    }
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gClassStaticData = e.classStaticData;
    if (!gKlass) {
        gKlass = x::runtime::il2cpp_shape::ResolveSecAttackKlass();
        if (!gKlass) {
            runtime::LogWThrottled(17, 15000, "SecAttack", "ResolveSecAttackKlass miss");
        }
    }
    // Do NOT RuntimeClassInit here — Bind/ProbeWindow run from feature workers
    // (e.g. multi_skill). class_init allocates → GC "Collecting from unknown thread".
    // Statics are readable once the game has touched SecurityClient.
    gStatic = KlassStaticFields(gKlass);
    const bool ok = gGA && gKlass && gStatic;
    if (ok) EnsureStaticFieldOff();
    gReady.store(ok);
    return ok;
}


DWORD WINAPI ProbeWorker(LPVOID) {
    BinLine("WORKER_START sample=%ums bin=%ums path=logs/sec_attack.log",
            (unsigned)kSampleMs, (unsigned)kBinProbeMs);
    runtime::LogI("SecAttack", "probe worker start sample=%ums bin=%ums",
                  (unsigned)kSampleMs, (unsigned)kBinProbeMs);
    UndoStaleTextHook();
    for (int i = 0; i < 400 && !gProbeStop.load() &&
                    !GetModuleHandleW(L"GameAssembly.dll");
         ++i)
        Sleep(50);
    while (!gProbeStop.load()) {
        SampleTick(GetTickCount());
        Sleep(50u);
    }
    BinLine("WORKER_STOP");
    runtime::LogI("SecAttack", "probe worker stop");
    return 0;
}

bool FillSnapshot(WindowSnapshot* out) {
    WindowSnapshot snap{};
    if (!Bind() || !gStatic) {
        if (out) *out = snap;
        return false;
    }
    snap.staticReady = true;
    void* pktDict = ReadPtr(gStatic, gOffPktDict);
    void* skillDict = ReadPtr(gStatic, gOffSkillDict);
    snap.detectTime = ReadI32Local(gStatic, gOffDetectTime);
    snap.windowAgeMs = -1;

    const DictScan pkt = SumDictIntValues(pktDict);
    const DictScan skill = SumDictIntValues(skillDict);
    snap.pktDictOk = pkt.headerOk;
    snap.skillDictOk = skill.headerOk;
    snap.pktSum = pkt.sum;
    snap.pktKeys = pkt.keys;
    snap.skillSum = skill.sum;
    snap.skillKeys = skill.keys;
    snap.pktPeak = pkt.peak;
    snap.skillPeak = skill.peak;
    snap.pktPeakId = pkt.peakId & 0xFFFF;
    snap.skillPeakId = skill.peakId;
    snap.peakKey = pkt.peak > skill.peak ? pkt.peak : skill.peak;
    snap.pctOfCheck = (kCheckCount > 0) ? (snap.peakKey * 100) / kCheckCount : 0;
    snap.ok = snap.pktDictOk || snap.skillDictOk;
    const DWORD now = GetTickCount();
    const bool skipRaise = now < gSkipRaiseUntil.load(std::memory_order_relaxed);
    if (skipRaise) {
        snap.windowPeak = gWindowHighPeak.load(std::memory_order_relaxed);
        snap.windowPktSum = gWindowHighPktSum.load(std::memory_order_relaxed);
        snap.windowSkillSum = gWindowHighSkillSum.load(std::memory_order_relaxed);
    } else if (gSampleDetectInit && snap.detectTime == gSampleDetectTime) {
        RaiseWindowHighs(snap);
        snap.windowPeak = gWindowHighPeak.load(std::memory_order_relaxed);
        snap.windowPktSum = gWindowHighPktSum.load(std::memory_order_relaxed);
        snap.windowSkillSum = gWindowHighSkillSum.load(std::memory_order_relaxed);
    } else {
        snap.windowPeak = snap.peakKey;
        snap.windowPktSum = snap.pktSum;
        snap.windowSkillSum = snap.skillSum;
    }
    if (out) *out = snap;
    return snap.ok;
}

void RaiseWindowHighs(const WindowSnapshot& snap) {
    int cur = gWindowHighPeak.load(std::memory_order_relaxed);
    while (snap.peakKey > cur &&
           !gWindowHighPeak.compare_exchange_weak(cur, snap.peakKey, std::memory_order_relaxed)) {
    }
    cur = gWindowHighPktSum.load(std::memory_order_relaxed);
    while (snap.pktSum > cur &&
           !gWindowHighPktSum.compare_exchange_weak(cur, snap.pktSum, std::memory_order_relaxed)) {
    }
    cur = gWindowHighSkillSum.load(std::memory_order_relaxed);
    while (snap.skillSum > cur &&
           !gWindowHighSkillSum.compare_exchange_weak(cur, snap.skillSum, std::memory_order_relaxed)) {
    }
}

void ResetWindowHighs(const WindowSnapshot& snap) {
    gWindowHighPeak.store(snap.peakKey, std::memory_order_relaxed);
    gWindowHighPktSum.store(snap.pktSum, std::memory_order_relaxed);
    gWindowHighSkillSum.store(snap.skillSum, std::memory_order_relaxed);
}

void BinProbe(const WindowSnapshot& snap, int highPeak, int highPkt, int highSkill) {
    unsigned fires = 0, firesNeed = 0;
    x::features::ports::mob_gather::QueryHangupFiresRaw(&fires, &firesNeed);
    BinLine("PROBE ok=%d peakKey=%d/%d(%d%%) pktPeak=%d op=%d skillPeak=%d sid=%d "
            "pktSum=%d pktKeys=%d skillSum=%d skillKeys=%d highPeak=%d highPkt=%d highSkill=%d "
            "fires=%u/%u detectTime=%d",
            snap.ok ? 1 : 0, snap.peakKey, kCheckCount, snap.pctOfCheck, snap.pktPeak,
            snap.pktPeakId, snap.skillPeak, snap.skillPeakId, snap.pktSum, snap.pktKeys,
            snap.skillSum, snap.skillKeys, highPeak, highPkt, highSkill, fires, firesNeed,
            snap.detectTime);
}

void SampleTick(DWORD now) {
    if (gLastSampleMs && now - gLastSampleMs < kSampleMs) return;
    gLastSampleMs = now;
    if (!world::IsPlayReady()) return;

    WindowSnapshot snap{};
    if (!FillSnapshot(&snap) && !snap.staticReady) return;

    const bool skipRaise = now < gSkipRaiseUntil.load(std::memory_order_relaxed);
    bool rolled = false;
    if (gSessResetBin.exchange(0)) {
        rolled = true;
        gLastBinnedHigh = -1;
        gLastBinProbeMs = 0;
    }
    if (!gSampleDetectInit) {
        gSampleDetectInit = true;
        gSampleDetectTime = snap.detectTime;
        if (!skipRaise) ResetWindowHighs(snap);
    } else if (snap.detectTime != gSampleDetectTime) {
        BinLine("WINRESET detectTime %d→%d closedPeak=%d closedPkt=%d closedSkill=%d",
                gSampleDetectTime, snap.detectTime,
                gWindowHighPeak.load(std::memory_order_relaxed),
                gWindowHighPktSum.load(std::memory_order_relaxed),
                gWindowHighSkillSum.load(std::memory_order_relaxed));
        gSampleDetectTime = snap.detectTime;
        ResetWindowHighs(snap);
        gLastBinnedHigh = -1;
        gLastBinProbeMs = 0;
        rolled = true;
    } else if (!skipRaise) {
        RaiseWindowHighs(snap);
    }

    const int high = gWindowHighPeak.load(std::memory_order_relaxed);
    const int highPkt = gWindowHighPktSum.load(std::memory_order_relaxed);
    const int highSkill = gWindowHighSkillSum.load(std::memory_order_relaxed);
    const bool grew = high > gLastBinnedHigh;
    const bool due = !gLastBinProbeMs || now - gLastBinProbeMs >= kBinProbeMs;
    if (!grew && !due && !rolled) return;
    gLastBinnedHigh = high;
    gLastBinProbeMs = now;
    BinProbe(snap, high, highPkt, highSkill);
}


// 热更清残：上一版若把 Collect*/Send 入口写成 xor eax,eax; ret，按原序言还原。不再安装。
constexpr uint32_t kRvaCollectAttackPacket = 0x3C8C450;
constexpr uint32_t kRvaCollectAttackSkill = 0x3C8CAB0;
constexpr uint32_t kRvaSendAttackPacketCountCheck = 0x3C8CE60;
constexpr size_t kTextPatchN = 8;
constexpr uint8_t kRetPatch[] = {0x33, 0xC0, 0xC3, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr uint8_t kExpectPkt[] = {0x56, 0x57, 0x48, 0x81, 0xEC, 0xB8, 0x00, 0x00};
constexpr uint8_t kExpectSkill[] = {0x56, 0x57, 0x48, 0x81, 0xEC, 0x98, 0x00, 0x00};
constexpr uint8_t kExpectSend[] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41};

struct TextSite {
    uint32_t rva;
    const uint8_t* expect;
};

constexpr TextSite kTextSites[] = {
    {kRvaCollectAttackPacket, kExpectPkt},
    {kRvaCollectAttackSkill, kExpectSkill},
    {kRvaSendAttackPacketCountCheck, kExpectSend},
};

bool BytesEq(const void* p, const uint8_t* expect, size_t n) {
    __try {
        return memcmp(p, expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProtectWrite(void* addr, size_t n, const uint8_t* src) {
    if (!addr || !src || n == 0) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        memcpy(addr, src, n);
        FlushInstructionCache(GetCurrentProcess(), addr, n);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(addr, n, old, &old);
    return ok;
}

void UndoStaleTextHook() {
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga) return;
    auto* base = reinterpret_cast<uint8_t*>(ga);
    int n = 0;
    for (const TextSite& s : kTextSites) {
        uint8_t* p = base + s.rva;
        if (!BytesEq(p, kRetPatch, kTextPatchN)) continue;
        if (ProtectWrite(p, kTextPatchN, s.expect)) ++n;
    }
    if (n > 0) {
        BinLine("TEXTUNDO n=%d pkt=0x%X skill=0x%X send=0x%X", n, kRvaCollectAttackPacket,
                kRvaCollectAttackSkill, kRvaSendAttackPacketCountCheck);
        runtime::LogI("SecAttack", "undid stale text hook n=%d", n);
    }
}

}  // namespace

void Init() {
    gReady.store(false);
    gLastBindMs = 0;
    gKlass = nullptr;
    gStatic = nullptr;
    runtime::LogI("SecAttack",
                  "port init (SecurityClient attack window TERM_MS=%d CHECK_COUNT=%d type=%d observe-only)",
                  kTermMs, kCheckCount, kClientHacksAttackPacketCountCheck);
    BinLine("INIT TERM_MS=%d CHECK_COUNT=%d type=%d observe=1 path=logs/sec_attack.log",
            kTermMs, kCheckCount, kClientHacksAttackPacketCountCheck);
}

void Shutdown() {
    gReady.store(false);
    gKlass = nullptr;
    gStatic = nullptr;
    gClassStaticData = nullptr;
    gGA = nullptr;
}

bool EnsureBound() { return Bind(); }

bool Ready() { return gReady.load() && gStatic; }

bool ProbeWindow(WindowSnapshot* out) {
    WindowSnapshot snap{};
    if (!EnsureBound() || !gStatic) {
        runtime::LogW("SecAttack", "probe fail bind klass=%p static=%p", gKlass, gStatic);
        BinLine("PROBE fail bind klass=%p static=%p", gKlass, gStatic);
        if (out) *out = snap;
        return false;
    }
    const bool ok = FillSnapshot(&snap);
    if (!ok && !snap.staticReady) {
        runtime::LogW("SecAttack", "probe fail bind klass=%p static=%p", gKlass, gStatic);
        BinLine("PROBE fail bind klass=%p static=%p", gKlass, gStatic);
        if (out) *out = snap;
        return false;
    }

    runtime::LogI(
        "SecAttack",
        "probe ok=%d static=1 pktSum=%d pktKeys=%d skillSum=%d skillKeys=%d "
        "peakKey=%d detectTime=%d pct=%d/%d(%d%%) hacksType=%d "
        "pkt{hdr=%d} skill{hdr=%d}",
        snap.ok ? 1 : 0, snap.pktSum, snap.pktKeys, snap.skillSum, snap.skillKeys, snap.peakKey,
        snap.detectTime, snap.peakKey, kCheckCount, snap.pctOfCheck,
        kClientHacksAttackPacketCountCheck, snap.pktDictOk ? 1 : 0, snap.skillDictOk ? 1 : 0);
    BinProbe(snap, snap.windowPeak, snap.windowPktSum, snap.windowSkillSum);

    if (out) *out = snap;
    return snap.ok;
}

bool PeekWindow(WindowSnapshot* out) { return FillSnapshot(out); }

int WindowHighPeak() { return gWindowHighPeak.load(std::memory_order_relaxed); }
int WindowHighPktSum() { return gWindowHighPktSum.load(std::memory_order_relaxed); }
int WindowHighSkillSum() { return gWindowHighSkillSum.load(std::memory_order_relaxed); }

void NoteHangupSession(const char* why) {
    const int closedPeak = gWindowHighPeak.load(std::memory_order_relaxed);
    const int closedPkt = gWindowHighPktSum.load(std::memory_order_relaxed);
    const int closedSkill = gWindowHighSkillSum.load(std::memory_order_relaxed);

    gWindowHighPeak.store(0, std::memory_order_relaxed);
    gWindowHighPktSum.store(0, std::memory_order_relaxed);
    gWindowHighSkillSum.store(0, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    gSkipRaiseUntil.store(now + 1000u, std::memory_order_release);
    gSessResetBin.store(1, std::memory_order_release);

    BinLine("SESSRESET why=%s closedPeak=%d closedPkt=%d closedSkill=%d",
            why && why[0] ? why : "?", closedPeak, closedPkt, closedSkill);
    runtime::LogI("SecAttack",
                  "hangup session reset why=%s closedPeak=%d closedPkt=%d closedSkill=%d",
                  why && why[0] ? why : "?", closedPeak, closedPkt, closedSkill);
}

void StartWorker() {
    if (gProbeWorker.load()) return;
    gProbeStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, ProbeWorker, nullptr, 0, nullptr);
    if (th) gProbeWorker.store(th);
}

void StopWorker() {
    gProbeStop.store(true);
    HANDLE th = gProbeWorker.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

}  // namespace x::features::ports::security_attack
