// security_attack_port — SecurityClient 攻包计数窗只读（数据面）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "security_attack_port.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::ports::security_attack {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

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
    "d87491da2374f815e43cf3ce5d21f637171b2bba55112b8d27d2ec8d2f84264";  // Dictionary<ushort,int>
constexpr char kHashSkillDict[] =
    "e2536ba7e6bd90ceb4e19ab119d02560d65bcf3200d03c48d3f150c3ccad735";  // Dictionary<int,int>
constexpr char kHashDetectTime[] =
    "c5c58d4820beefd3153ff06483d828a765e6261b45db32658bd1cc34090b1e4";
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

constexpr DWORD kRebindMs = 5000;

using FnClassStaticData = void* (*)(void* klass);

HMODULE gGA = nullptr;
FnClassStaticData gClassStaticData = nullptr;
void* gKlass = nullptr;
void* gStatic = nullptr;
DWORD gLastBindMs = 0;
std::atomic<bool> gReady{false};

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
                "", "cf990184167a3debe30b85ee608efab18ffc750676a5f79617009d0f56bec8d");
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
                sum += val;
                ++keys;
                ++seen;
                if (val > peak) peak = val;
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

}  // namespace

void Init() {
    gReady.store(false);
    gLastBindMs = 0;
    gKlass = nullptr;
    gStatic = nullptr;
    runtime::LogI("SecAttack",
                  "port init (SecurityClient attack window TERM_MS=%d CHECK_COUNT=%d type=%d)",
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
        if (out) *out = snap;
        return false;
    }
    snap.staticReady = true;

    void* pktDict = nullptr;
    void* skillDict = nullptr;
    int detectTime = 0;
    __try {
        pktDict = ReadPtr(gStatic, gOffPktDict);
        skillDict = ReadPtr(gStatic, gOffSkillDict);
        detectTime = ReadI32Local(gStatic, gOffDetectTime);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        runtime::LogW("SecAttack", "probe SEH reading static_fields");
        if (out) *out = snap;
        return false;
    }
    snap.detectTime = detectTime;
    // detectTime 是游戏侧 tCur（与 SendAttackPacketCountCheck 同钟），不是 GetTickCount。
    snap.windowAgeMs = -1;

    const DictScan pkt = SumDictIntValues(pktDict);
    const DictScan skill = SumDictIntValues(skillDict);
    snap.pktDictOk = pkt.headerOk;
    snap.skillDictOk = skill.headerOk;
    snap.pktSum = pkt.sum;
    snap.pktKeys = pkt.keys;
    snap.skillSum = skill.sum;
    snap.skillKeys = skill.keys;

    // type20 按单键计数；pct 用 peak（两表取大），sum 仍保留总量观测。
    const int peakKey = pkt.peak > skill.peak ? pkt.peak : skill.peak;
    snap.pctOfCheck = (kCheckCount > 0) ? (peakKey * 100) / kCheckCount : 0;
    // 静态表在且至少一侧字典头可读（含确认空表）即 ok。
    snap.ok = snap.pktDictOk || snap.skillDictOk;

    runtime::LogI(
        "SecAttack",
        "probe ok=%d static=1 pktSum=%d pktKeys=%d skillSum=%d skillKeys=%d "
        "peakKey=%d detectTime=%d pct=%d/%d(%d%%) hacksType=%d "
        "pktDict=%p skillDict=%p "
        "pkt{hdr=%d sumOk=%d cnt=%d free=%d ver=%d elen=%d} "
        "skill{hdr=%d sumOk=%d cnt=%d free=%d ver=%d elen=%d}",
        snap.ok ? 1 : 0, snap.pktSum, snap.pktKeys, snap.skillSum, snap.skillKeys, peakKey,
        snap.detectTime, peakKey, kCheckCount, snap.pctOfCheck, kClientHacksAttackPacketCountCheck,
        pktDict, skillDict, pkt.headerOk ? 1 : 0, pkt.summed ? 1 : 0, pkt.count, pkt.freeCount,
        pkt.version, pkt.entriesLen, skill.headerOk ? 1 : 0, skill.summed ? 1 : 0, skill.count,
        skill.freeCount, skill.version, skill.entriesLen);

    if (out) *out = snap;
    return snap.ok;
}

}  // namespace x::features::ports::security_attack
