#pragma once
// mob_pool_port — Classic TWMS 活怪只读快照 + 刷怪槽 M
//
// 真源：Dumps/runtime/out/dump.cs（2026-08-13 remount）
//   MobPool / Mob(d8d3c811…) / MapData（WM+0x88 字段类型）
//   MapData.LifeList@+0x38 · MapLifeData.Type@+0x20 (1=Mob)
//   MapLifeData.X@+0x24 Y@+0x28（ctor 把 WZ.y 存成 −WZ.y = AbsPos；更大 Y = 更高）
//   Rx0@+0x38 Rx1@+0x3C（巡逻横区间，与 X 同号）
//   WorldManager._currentMapData@+0x88
// 禁止 INLINE HOOK；不调用游戏写接口。
//
// 绝对血：
//   Mob 本体只有 HpPercentage@+0x238（08-13：旧 0x240 已变成 bool）。
//   包/UI 侧有绝对血（ShowMobHpTag → UIHpTag cur@+0xD4 max@+0xD8），但包路径为 E8 直调；
//   不做 MI/trampoline 观察。ResolveAbsHp：可选 FindAll UIHpTag → 进程缓存 → hp%×表。

#include <cstdint>

namespace x::features::ports::mob {

constexpr int kMaxLiteMobs = 128;
constexpr int kMaxSpawnPoints = 256;

// LifeList 里 Type==Mob 的刷怪槽几何（AbsPos）。
struct SpawnPoint {
    float x = 0.f;
    float y = 0.f;  // AbsPos：更大 Y = 更高
    int32_t rx0 = 0;
    int32_t rx1 = 0;
};

// Mob.MobCtrlState@0xE8, typed MobCtrlType: negative = someone else drives this mob, positive =
// we do. The controller is the client that runs the mob's AI and reports its movement, so this is
// the field that decides whether our attack on it is coming from the right client.
enum : int32_t {
    kMobCtrlPassive = -1,
    kMobCtrlPassive0 = -2,
    kMobCtrlPassive1 = -3,
    kMobCtrlActiveInt = 1,
    kMobCtrlActiveReq = 2,
    kMobCtrlActivePerm0 = 3,
    kMobCtrlActivePerm1 = 4,
};

enum class AbsHpSrc : uint8_t {
    None = 0,
    UiHpTag = 1,       // 当场读到 UIHpTag
    UiHpTagCache = 2,  // 先前 UI FindAll 写入的进程缓存
    PctEstimate = 3,   // hpPct × mob_stats.maxHP
};

struct MobLite {
    void* ptr = nullptr;
    int32_t id = 0;
    int32_t templateId = 0;
    int32_t hpPct = 0;
    int32_t lastHitted = 0;  // Mob._lastHitted@0x208；AddDamageInfo 当帧写（早于 hpPct）
    int32_t deadType = 0;
    int32_t ctrl = 0;  // MobCtrlType; >0 = ours
    float x = 0.f;
    float y = 0.f;
    bool ready = false;
    // FindHit 要 inView；选怪/旋翼不再因 false 剔出 n（BIN：下层满血怪 v=0 → 假空图落地）。
    bool inView = false;
    // ResolveAbsHp 填充（TryFillLive / Collect 可选）；src=None 表示未解析。
    int64_t absHp = 0;
    int64_t absMaxHp = 0;
    AbsHpSrc absSrc = AbsHpSrc::None;
};

// 离线表 dataservice/mob_stats.tsv：templateId → maxHP。未知模板返回 0。
int64_t LookupTemplateMaxHp(int32_t templateId);

// UIHpTag（预制名）：包/UI 路径缓存的绝对血（非 Mob 本体）。
// IDA：ShowMobHpTag 写 mobId@+0xC8 · cur@+0xD4 · max@+0xD8（CMS 同语义，TW 偏移 +8）。
// 仅血条弹出/曾弹出时有效；FindAll 扫不到时 ok=false。
struct UiHpTagSnap {
    bool ok = false;
    int scanned = 0;  // FindAll 返回个数
    int valid = 0;    // mobId!=0 && maxHp>0
    int32_t mobId = 0;
    int32_t cachedHp = 0;
    int32_t cachedMaxHp = 0;
    void* tagPtr = nullptr;
};
// preferMobId!=0 时优先匹配该 id；否则取第一份有效 tag。成功时顺便刷新进程缓存。
bool TryReadUiHpTag(int32_t preferMobId, UiHpTagSnap& out);

struct AbsHp {
    bool ok = false;
    AbsHpSrc src = AbsHpSrc::None;
    int64_t cur = 0;
    int64_t max = 0;
};

// 按 mobId 取绝对血：UIHpTag → 进程缓存 → hp%×表 maxHP。
// refreshUi=true 时先 FindAll 扫 UIHpTag（较贵，热路径宜节流）。
bool ResolveAbsHp(int32_t mobId, int32_t templateId, int32_t hpPct, AbsHp& out,
                  bool refreshUi = false);

// 丢弃某 mob / 全表的绝对血缓存（死怪、换图）。
void InvalidateAbsHpCache(int32_t mobId);
void ClearAbsHpCache();

// Mob._damageInfo@+0x1D8（CMS List<DamageInfo>）。只读；用于标定 Damage@+0x24。
constexpr int kMaxDamageInfoProbe = 8;
struct DamageInfoLite {
    int32_t skillId = 0;
    int32_t hitAction = 0;
    int32_t damage = 0;      // CMS +0x24
    int32_t attackIdx = 0;
    int32_t moveType = 0;
    uint32_t charId = 0;
    float delayed = 0.f;
    // 原始 int 扫描（防布局漂移）：offs 0x14/18/1C/24/2C/34
    int32_t raw[6]{};
};
struct DamageInfoSnap {
    bool ok = false;
    int listSize = 0;
    int count = 0;           // 实际填入条数（末尾最多 kMaxDamageInfoProbe）
    int64_t sumDamage = 0;   // 全列表 Damage@+0x24 求和（截断前）
    int32_t lastDamage = 0;  // 末条 Damage
    DamageInfoLite items[kMaxDamageInfoProbe]{};
};
bool TryReadDamageInfoList(void* mob, DamageInfoSnap& out);

// Short name for a MobCtrlType value, for logs.
const char* CtrlName(int32_t ctrl);
const char* AbsHpSrcName(AbsHpSrc src);

// 抢怪软优先：>0 我方控、0 中性、<0 他人驱动。同分再比距离；全员 Passive 时行为与旧版一致。
inline int CtrlPreferRank(int32_t ctrl) {
    if (ctrl > 0) return 2;
    if (ctrl == 0) return 1;
    return 0;
}

// FillLite 拒样：n=0 但 raw>0 时拆拒绝门（尸体/未就绪等）。
struct FillRejectSample {
    int id = 0;
    int tpl = 0;
    int hpPct = 0;
    int deadType = 0;
    float x = 0.f;
    float y = 0.f;
    uint8_t ready = 0;
    uint8_t inView = 0;
    uint8_t suspended = 0;
    // R=notReady D=deadType H=hp S=suspended P=dirtyPos O=other(klass/id/special)
    char why = '?';
};
constexpr int kMaxFillRejectSamples = 4;

struct Snapshot {
    bool ok = false;
    bool truncated = false;
    int count = 0;        // 入榜活怪 n
    int rawDict = 0;      // 字典/FindAll 原始候选
    int spawnSlots = -1;  // 刷怪槽 M：LifeList(Mob) 优先；失败用本图峰值
    int mapId = 0;        // MapData.Id；0=未知
    int lifeMob = -1;     // LifeList 里 Type==Mob 条数（-1=未读到）
    int lifeAll = -1;     // LifeList 总条数
    int spawnPointN = 0;  // 填入 spawnPoints 的槽数（可能 < lifeMob）
    SpawnPoint spawnPoints[kMaxSpawnPoints]{};
    uint64_t tickMs = 0;
    MobLite mobs[kMaxLiteMobs]{};
    // 入榜但 FindHit 尚不可用的活怪数（inView=0）；不挡 n，供 BIN / 选怪软优先对照。
    int nInView0 = 0;
    // raw - n 归因（每帧 Collect 清零重计）
    int rejNotReady = 0;
    int rejDeadType = 0;
    int rejHp = 0;
    int rejSuspended = 0;
    int rejDirty = 0;
    int rejOther = 0;
    int rejSampleN = 0;
    FillRejectSample rejSamples[kMaxFillRejectSamples]{};
};

// 解析 MobPool / APIs（可重复调用；失败返回 false）
bool EnsureBound();

// 同步采集一帧快照（SEH 护体；可在 worker 线程调）
// fillSpawnSlots=false：跳过 LifeList 扫 M，沿用上一帧 spawnSlots（热路径减负）。
bool Collect(Snapshot& out, bool fillSpawnSlots = true);

// 锁怪热路径：直接读 mob 指针字段（不等 mobscan 缓存）。
// 活着填 out 返回 true；尸体/空血/野指针返回 false。expectId!=0 时校验 id。
// 绝对血：默认用缓存+%估计（不 FindAll）；需要新鲜 UI 时自行 ResolveAbsHp(..., refreshUi=true)。
bool TryFillLive(void* mob, int32_t expectId, MobLite& out);
// CDF / 作动器：不 EnsureBound。尸体、空血、未就绪、id 对不上（池槽复用）→ false。
bool StillSameLiveMob(void* mob, int32_t expectId, const char** why = nullptr);

// 最近一次成功快照（双缓冲只读）
bool GetCached(Snapshot& out);
int GetCachedAliveCount();
int GetCachedSpawnSlots();  // -1=未知
// 缓存年龄（ms）；无缓存返回 ~0xFFFFFFFF。
uint32_t GetCachedAgeMs();

// 战斗 worker 安全刷新：仅当 MobPool Singleton 已由 mob_scan 热身后，只读字典 + Publish。
// 不 FindAll、不 RuntimeClassInit、不扫 LifeList。失败返回 false（继续用缓存 + RequestImmediateScan）。
bool TryRefreshCacheLite(Snapshot& out);

// 仅刷怪槽：优先 MapData.LifeList(Mob)；失败回退本图峰值（需先 Collect 过）
int CountMapMobLifeSlots();
int GetSpawnPeak();

// Ap 是否距某条 Mob 槽 (X,Y) < nearPx。不管 Rx 巡逻带（走出台仍在 Rx 内，不能当「还在出生点」）。
// spawnPointN==0（LifeList 未读到）→ false（放行，避免误杀全图）。
bool NearMobLifeSlot(float x, float y, const Snapshot& snap, float nearPx,
                     float* outDist = nullptr);

}  // namespace x::features::ports::mob
