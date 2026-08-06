#pragma once
// foothold_path — Classic TWMS foothold 图 + BFS 首步（跨层接近用）
// 边：Prev/Next 步行；绳子 ClimbUp/Down；!forbidFall → FallDown（下跳穿台）。
// Snapshot / Graph 均在堆上；禁止 worker 栈声明大缓冲。

#include <cstdint>

namespace x::features::ports::foothold_path {

enum class EdgeKind : uint8_t {
    Walk = 0,
    ClimbUp = 1,     // 朝更小 Y（MS 坐标系向上）
    ClimbDown = 2,
    FallDown = 3,    // ↓+Jump 穿台 / 落到下方 FH
};

struct FirstAction {
    bool ok = false;
    EdgeKind kind = EdgeKind::Walk;
    uint32_t fromFh = 0;
    uint32_t toFh = 0;
    int32_t ropeId = 0;
    int32_t wx = 0;  // 走位 / 绳子 X / 下跳落点 X
    int32_t wy = 0;
    int hops = 0;
};

bool EnsureGraph();
bool FindNearestFh(float x, float y, uint32_t* outId, float* outDist = nullptr);

// 贴站立平台：优先同层(|fy-y|≤45)且覆盖 x → 同层 Y 带 → 宽松覆盖 → 最近点。
// FH=Prev/Next 线段链；X 钳在「整条 Walk 链」安全带（链条端点内缩）。
// avoidWalkJunction：战斗默认 true（再避开 Walk 段缝）；贴门路径传 false。
// preferFlat：优先 |y1-y2|≤kFlatYTol 的平台段（全局搜；赶路贴门请用 SnapStandForPortal）。
bool SnapStandAt(float x, float y, float* outX, float* outY, uint32_t* outFhId = nullptr,
                 bool preferFlat = false, bool avoidWalkJunction = true);

// 诊断：报「该点附近到底有没有台、为什么不可用」。Snap 贴到几百 px 外的远台时用它定位过滤器。
// inBand = |段在该 X 处的 Y - y| ≤ 45 的段数（不过滤）；其余为其中各档。
struct StandCensus {
    int nodes = 0;
    int inBand = 0;
    int usable = 0;  // 过滤全过（可被 Snap 选中）
    int wall = 0;    // SpanX < 16 判墙
    int narrow = 0;  // 整条 Walk 链跨度不足，判站不住
    int bestSpan = 0;      // inBand 中最长段的 SpanX
    int bestChainLo = 0;   // 该段所在链的安全带（lo≥hi 即链过短）
    int bestChainHi = 0;
};
bool CensusStandAt(float x, float y, StandCensus* out);

// 诊断：报「该 x 这一列从上到下有哪些台」。落点失手时判「引擎为何把人抬到别的台上」用：
// 只取 X 区间真覆盖 x 的段（不钳 X、不放宽），按 Y 升序（屏幕上方在前）。
// 返回写入条数；图未就绪返回 -1。outTotal 给窗口内总条数（可 > maxOut）。
struct ColumnHit {
    uint32_t fh = 0;
    int y = 0;      // 该段在该 x 处的 Y
    int span = 0;   // SpanX
    int slope = 0;  // |y1-y2|
    bool wall = false;
    bool narrow = false;
};
int ProbeColumn(float x, float y, int yWindow, ColumnHit* out, int maxOut, int* outTotal = nullptr);

// 赶路贴门：先定覆盖 (x,y) 的 Walk 链，再在链内选与触发框 X 相交的最平段。
// 仅悬崖内缩（avoidWalkJunction=false）；失败回退 SnapStandAt(preferFlat, junction=false)。
// rectValid=false 时用 x±20 作兴趣带。
bool SnapStandForPortal(float x, float y, float rectL, float rectT, float rectR, float rectB,
                        bool rectValid, float* outX, float* outY, uint32_t* outFhId = nullptr);

// 钉死在指定 FH 线段上：X = 本段 ∩ 链条安全带。
// avoidWalkJunction 默认 true（战斗）；赶路贴门复钉请传 false（接合处可站）。
bool SnapOnFh(uint32_t fhId, float x, float* outX, float* outY, bool avoidWalkJunction = true);

// x 是否落在该 FH 安全站立带内（与 SnapOnFh 同一套链条规则）。
bool IsXSafeOnFh(uint32_t fhId, float x, bool avoidWalkJunction = true);

// FH / 点附近站立台的 zMass（连通域键）。图未就绪或未命中台返回 false。
bool ZMassOfFh(uint32_t fhId, int32_t* outZMass);
bool ZMassAt(float x, float y, int32_t* outZMass, uint32_t* outFhId = nullptr);

// 仅 Prev/Next 步行边连通（不含绳/下跳）。拟人走路可达性；同台则 true。
// 图未就绪 / 未知 FH → false（调用方决定是否退回纯 Y 带）。
bool SameWalkComponent(uint32_t fhA, uint32_t fhB);

bool PlanFirst(uint32_t fromFh, uint32_t toFh, FirstAction* out);

struct GraphMeta {
    bool ok = false;
    int32_t mapId = 0;
    int nodes = 0;
    int walkEdges = 0;
    int climbEdges = 0;
    int fallEdges = 0;
    int ropeLinked = 0;
};
bool GetGraphMeta(GraphMeta* out);

}  // namespace x::features::ports::foothold_path
