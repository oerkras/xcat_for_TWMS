#pragma once
// foothold_port — Classic TWMS MapData foothold / ladder-rope 只读枚举
//
// 真源（TW dump ≡ CMS 偏移）：
//   WM._currentMapData@0x88
//   MapData._footholdMap@0xE0  Dictionary<uint,StaticFoothold>
//   MapData.LadderRopes@0x108  List<LadderOrRope>
// 禁止 INLINE HOOK；不做寻路，只采几何。
// Snapshot ~100KB：Collect 写入堆缓存；勿在 worker 栈上声明 Snapshot。

#include <cstdint>

namespace x::features::ports::foothold {

constexpr int kMaxFootholds = 2048;
constexpr int kMaxLadders = 256;

struct FootholdLite {
    uint32_t id = 0;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    uint32_t prev = 0, next = 0;
    int32_t zMass = 0;
    int32_t page = 0;
    int32_t layer = 0;
    bool forbidFall = false;
};

struct LadderLite {
    int32_t id = 0;
    int32_t x = 0, y1 = 0, y2 = 0;
    int32_t page = 0;
    bool isLadder = false;
    bool isUpperFh = false;
};

struct Snapshot {
    bool ok = false;
    int32_t mapId = 0;
    int footholdN = 0;
    int ladderN = 0;
    uint32_t curFhId = 0;
    int idMismatch = 0;  // dict key ≠ StaticFoothold.ID
    FootholdLite footholds[kMaxFootholds]{};
    LadderLite ladders[kMaxLadders]{};
};

// 轻量摘要（可放栈上）
struct SnapshotMeta {
    bool ok = false;
    int32_t mapId = 0;
    int footholdN = 0;
    int ladderN = 0;
    uint32_t curFhId = 0;
    int idMismatch = 0;
};

bool EnsureBound();

// 枚举写入堆缓存；meta 可空。成功条件：有 fh 或有绳子。
bool CollectToCache(SnapshotMeta* meta = nullptr);

// 轻量：只读缓存摘要（可栈）。
bool GetCachedMeta(SnapshotMeta* out);

// 实时读玩家 CurFootHold.ID（不改缓存）。
uint32_t PeekCurFhId();

// 按 ID 查 MapData._footholdMap → StaticFoothold*（瞬移落地种植 CurFh 用）。
void* ResolveFhObject(uint32_t id);

// 从堆缓存拷出完整几何（调用方勿放栈；寻路用堆/静态缓冲）。
bool GetCached(Snapshot& out);

// 详情只写 logs/foothold.log；runtime LogI 仅一行摘要。
void DumpCachedLog();

}  // namespace x::features::ports::foothold
