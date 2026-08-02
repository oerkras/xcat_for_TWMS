#pragma once
// mob_pool_port — Classic TWMS 活怪只读快照 + 刷怪槽 M
//
// 真源：Dumps/runtime/out/dump.cs（2026-08-03 remount）
//   MobPool(d8a4e9e1…) / Mob(a6c2b431…) / MapData(bb2af058…＝WM+0x88 字段类型)
//   MapData.LifeList@+0x38 · MapLifeData.Type@+0x20 (1=Mob)
//   WorldManager._currentMapData@+0x88
// 禁止 INLINE HOOK；不调用游戏写接口。

#include <cstdint>

namespace x::features::ports::mob {

constexpr int kMaxLiteMobs = 128;

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

struct MobLite {
    void* ptr = nullptr;
    int32_t id = 0;
    int32_t templateId = 0;
    int32_t hpPct = 0;
    int32_t deadType = 0;
    int32_t ctrl = 0;  // MobCtrlType; >0 = ours
    float x = 0.f;
    float y = 0.f;
    bool ready = false;
};

// Short name for a MobCtrlType value, for logs.
const char* CtrlName(int32_t ctrl);

struct Snapshot {
    bool ok = false;
    bool truncated = false;
    int count = 0;        // 入榜活怪 n
    int rawDict = 0;      // 字典/FindAll 原始候选
    int spawnSlots = -1;  // 刷怪槽 M：LifeList(Mob) 优先；失败用本图峰值
    int mapId = 0;        // MapData.Id；0=未知
    int lifeMob = -1;     // LifeList 里 Type==Mob 条数（-1=未读到）
    int lifeAll = -1;     // LifeList 总条数
    uint64_t tickMs = 0;
    MobLite mobs[kMaxLiteMobs]{};
};

// 解析 MobPool / APIs（可重复调用；失败返回 false）
bool EnsureBound();

// 同步采集一帧快照（SEH 护体；可在 worker 线程调）
bool Collect(Snapshot& out);

// 锁怪热路径：直接读 mob 指针字段（不等 mobscan 缓存）。
// 活着填 out 返回 true；尸体/空血/野指针返回 false。expectId!=0 时校验 id。
bool TryFillLive(void* mob, int32_t expectId, MobLite& out);

// 最近一次成功快照（双缓冲只读）
bool GetCached(Snapshot& out);
int GetCachedAliveCount();
int GetCachedSpawnSlots();  // -1=未知

// 仅刷怪槽：优先 MapData.LifeList(Mob)；失败回退本图峰值（需先 Collect 过）
int CountMapMobLifeSlots();
int GetSpawnPeak();

}  // namespace x::features::ports::mob
