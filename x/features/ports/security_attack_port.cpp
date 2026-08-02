// security_attack_port — SecurityClient 攻包计数窗只读（数据面）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "security_attack_port.h"

#include "../../runtime/il2cpp_bind.h"
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
// （hash d9ef28f1… + static Dict@0/8 + I32@0x10；旧 ba499947… 已作废）

constexpr size_t kOffPktDict = 0x0;
constexpr size_t kOffSkillDict = 0x8;
constexpr size_t kOffDetectTime = 0x10;

// Dictionary`2 IL2CPP（与 drop_pool 一致；勿用 0x2C——那是 _version）
// buckets@0x10 / entries@0x18 / count@0x20 / freeList@0x24 / freeCount@0x28 / version@0x2C
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kOffDictFreeCount = 0x28;
constexpr size_t kOffDictVersion = 0x2C;  // 仅诊断
constexpr size_t kEntryStrideTight = 0x10;  // hash+next+key+value(@12) — ushort/int、int/int
constexpr size_t kEntryStrideAlign = 0x18;  // value@16

constexpr DWORD kRebindMs = 5000;

using FnClassStaticData = void* (*)(void* klass);

HMODULE gGA = nullptr;
FnClassStaticData gClassStaticData = nullptr;
void* gKlass = nullptr;
void* gStatic = nullptr;
DWORD gLastBindMs = 0;
std::atomic<bool> gReady{false};

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
        len = ReadI32Local(entries, 0x18);
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
                uint8_t* e =
                    reinterpret_cast<uint8_t*>(entries) + 0x20 + static_cast<size_t>(i) * stride;
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

    if (tryStride(kEntryStrideTight, 12)) return out;
    if (tryStride(kEntryStrideAlign, 16)) return out;
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
        pktDict = ReadPtr(gStatic, kOffPktDict);
        skillDict = ReadPtr(gStatic, kOffSkillDict);
        detectTime = ReadI32Local(gStatic, kOffDetectTime);
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
