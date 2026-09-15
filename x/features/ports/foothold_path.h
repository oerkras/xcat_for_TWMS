#pragma once
// foothold_path — Classic TWMS foothold 图 + BFS 首步（跨层接近用）
// 边：Prev/Next 步行；绳子 ClimbUp/Down；!forbidFall → FallDown（下跳穿台）。
// Snapshot / Graph 均在堆上；禁止 worker 栈声明大缓冲。

#include <cstdint>

namespace x::features::ports::foothold_path {

enum class EdgeKind : uint8_t {
    Walk = 0,
    ClimbUp = 1,     // 朝更大 AbsPos.Y（更高）
    ClimbDown = 2,   // 朝更小 AbsPos.Y（更低）
    FallDown = 3,    // 穿台 ↓+Jump；悬崖走出台沿（不趴下）
    JumpAcross = 4,  // 极限跳：从本段崖沿助跑起跳，越过空隙 / 跳上不相连的高台（wx=起跳点，aimX=落点）
    RopeJump = 5,    // 绳上侧跳：爬到 wy 再朝 aimX 跳——落到绳侧的台（ropeId2=0）或抓住另一根绳（ropeId2≠0）
    JumpUp = 6,      // 原地竖直跳上头顶的台：台面单向可穿，站在下台 X 重叠处起跳、下落时落到上台（wx=aimX=起跳 X）
};

// 构图 FallDown 边的官方一跳上限（像素）。所有图同一条引擎规则，不是某图出生坑 Y。
constexpr int kFallMaxDyPx = 720;

// 跳跃物理（BIN 实测：起跳 vy0≈525 px/s、g≈1714 px/s² → 顶点≈80px、同高滞空≈0.61s；
// 台上助跑起跳空中横速≈走速 125，从绳上跳出实测 ≈162）。极限跳只按 92% 距离规划，
// 速度/跳跃 buff 变化留余量：同高空隙可跳 ≈56px，落低 100px 的可跳 ≈74px，跳上 60px 高台 ≈39px。
constexpr float kJumpV0PxPerSec = 525.f;
constexpr float kJumpGravityPxPerSec2 = 1714.f;
constexpr float kJumpAirVxPxPerSec = 125.f;
constexpr float kRopeJumpAirVxPxPerSec = 150.f;
constexpr float kJumpReachFactor = 0.92f;
// 从绳上侧跳出去是「小跳」，不是台上的满跳：upload 2026-09-09 16:00:36（0CABDEGD map30000）离绳
// y=9 后 ≈150ms 在 y=34、vy=+38、vx=162 → vy0≈295、顶点≈25px；BIN 13:55:26 另一次 y0=-154 → 顶点≈17px。
// 取 vy0=270（顶点≈21px、0.16s 到顶）；横速实测 162，可行域按 150~175 的带算（规划名义 150）。
// 绳只在**下落段**抓得住（同日三次成功抓绳全在顶点后），绳到绳的落点 Y 要按下落段算。
constexpr float kRopeHopV0PxPerSec = 270.f;
constexpr float kRopeHopVxLoPxPerSec = 150.f;
constexpr float kRopeHopVxHiPxPerSec = 175.f;

// 绳节点：图里每根绳一个节点（id = kRopeNodeIdBase + 绳序号），代表「人挂在这根绳上」。
// 有些绳（WZ `uf=0`）顶上没有台，只能从别的绳接力跳过去、再从它跳到下一根 / 侧台：
// 没有绳节点这类绳在图里既到不了也走不出（upload 2026-09-09 map101040000：28 根绳 15 根 uf=0）。
constexpr uint32_t kRopeNodeIdBase = 0x40000000u;
inline bool IsRopeNodeId(uint32_t id) { return (id & 0xC0000000u) == kRopeNodeIdBase; }

struct FirstAction {
    bool ok = false;
    EdgeKind kind = EdgeKind::Walk;
    uint32_t fromFh = 0;
    uint32_t toFh = 0;
    int32_t ropeId = 0;
    int32_t wx = 0;  // 走位 / 绳子 X / 下跳落点 X / 极限跳起跳 X
    int32_t wy = 0;  // 绳底(ClimbUp) / 绳顶(ClimbDown) / 起跳台面 Y / RopeJump 的起跳 Y
    int32_t aimX = 0;      // JumpAcross / RopeJump：落点 X（RopeJump 抓绳时 = 目标绳 X）
    int32_t ropeId2 = 0;   // RopeJump：目标绳 id（0 = 落台）
    int32_t ropeYBot = 0;  // 本动作用到的绳（ropeId）的底 / 顶（AbsPos：更大 Y = 更高）
    int32_t ropeYTop = 0;
    int hops = 0;
};

bool EnsureGraph();  // 懒建。换图只靠 CollectToCache；拟人/落台/lie_safe 第一次 Snap 才构图。
bool FindNearestFh(float x, float y, uint32_t* outId, float* outDist = nullptr);

// 本图欧氏最近可站点（段上最近点，不是中点）。跳过墙；先宽链、没有再允许窄台。
// 给 lie_safe / 补给落台用：不走 SnapStandAt 的同高 band（会贴到千米外幽灵台）。
bool FindNearestStand(float x, float y, float* outX, float* outY, uint32_t* outFhId = nullptr,
                      float* outDist = nullptr, bool avoidWalkJunction = true,
                      bool cliffInset = true);

// 贴站立平台：优先同层(|fy-y|≤45)且覆盖 x → 同层 Y 带 → 宽松覆盖 → 最近点。
// FH=Prev/Next 线段链。
// avoidWalkJunction：战斗默认 true（再避开 Walk 段缝）。
// cliffInset：战斗默认 true（链条端点内缩 36）。**F6 / 超级赶路必须 false**。
// preferFlat：优先 |y1-y2|≤kFlatYTol 的平台段（全局搜；赶路贴门请用 SnapStandForPortal）。
bool SnapStandAt(float x, float y, float* outX, float* outY, uint32_t* outFhId = nullptr,
                 bool preferFlat = false, bool avoidWalkJunction = true, bool cliffInset = true);

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

// 赶路贴门：在发门带 [portal.x±16] 里找可站点（X=带内离门心最近，Y=该处台面）。
// 用本段原始 X（2px 边），**不用**战斗悬崖内缩 36（门口常在悬崖边）。
// 门心底下可以是缝（BIN 18:27 top00：x=65 miss，x=72 可站且 |dx|≤16）。
// 门口 SpanX<16 的近水平短台算可站（BIN 107000000 east00：12 段短台被当墙 → 假空集悬停）。
// 竖墙 / 错层短台（|fy-portal.y|>24）仍拒。禁止回退 SnapStandAt 的 band/any（远岸 Y 污染）。
// 失败 = 发门带空集 → 调用方保持 portal.y，走悬停。
bool SnapStandForPortal(float x, float y, float rectL, float rectT, float rectR, float rectB,
                        bool rectValid, float* outX, float* outY, uint32_t* outFhId = nullptr);

// 钉死在指定 FH 线段上。
// avoidWalkJunction / cliffInset 默认 true（战斗）。F6 / 赶路传 false。
bool SnapOnFh(uint32_t fhId, float x, float* outX, float* outY, bool avoidWalkJunction = true,
              bool cliffInset = true);

// x 是否落在该 FH 安全站立带内（与 SnapOnFh 同一套链条规则）。
bool IsXSafeOnFh(uint32_t fhId, float x, bool avoidWalkJunction = true, bool cliffInset = true);

// (x,y) 旁 ±14px 内有没有绳段经过（上下各放 24px）：在这儿按 ↓ 会上绳而不是穿台，穿台点要避开。
bool RopeNearDropPoint(float x, float y);

// 站在 (x,y) 按 ↓ 穿台会落到哪块台：X 真盖住 x、台面在脚下 ≥8px 的最高一块（非墙、非绳节点）。没有 → 0。
uint32_t FirstFhBelow(float x, float y);

// FH / 点附近站立台的 zMass（连通域键）。图未就绪或未命中台返回 false。
bool ZMassOfFh(uint32_t fhId, int32_t* outZMass);
bool ZMassAt(float x, float y, int32_t* outZMass, uint32_t* outFhId = nullptr);

// 仅 Prev/Next 步行边连通（不含绳/下跳）。拟人走路可达性；同台则 true。
// 图未就绪 / 未知 FH → false（调用方决定是否退回纯 Y 带）。
bool SameWalkComponent(uint32_t fhA, uint32_t fhB);

// 步行连通域编号（构图时标好）。0 = 未知 / 图未就绪。同号 = SameWalkComponent。
int WalkCompOf(uint32_t fh);

// 从 fromFh 出发：Walk 不限跳、FallDown ≤ maxFallHops（不含绳；边按无向计跳）。
// outMask[i]=1 表示构图节点 i 可达。返回节点数；图未就绪 / 未知 FH → 0。
// outReachCnt 可选：mask 里标到的节点数（不是图总节点）。
int MarkFallWalkReachable(uint32_t fromFh, int maxFallHops, uint8_t* outMask, int maxMask,
                          int* outReachCnt = nullptr);
bool MaskHasFh(const uint8_t* mask, int n, uint32_t fh);

// 首步：按拟人耗时（走 / 上绳 / 下绳侧跳 / 下跳 / 极限跳各自的秒数 + 失手风险）找最快路，
// 与 MarkAllPathTime 的选怪 ETA 同一模型。同 Walk 连通域且几乎同高 → hops=0 直接朝目标 X 走。
// fromX / toX：当前站的 X 与目标 X（省略 = 用段中点）；给了才能算准「走到绳下 / 下跳点」和
// 「落地后再走到目标」这两段——站在高台上要去正下方时，下跳 1.5s 对爬绳 6.5s，不给 X 会算错边。
// ignoreSkips=true：不理 AddHopSkip 的边（被跳过的边是唯一路时兜底再试，别把对面整层判 no_path）。
constexpr int32_t kPlanNoX = -0x7fffffff - 1;
bool PlanFirst(uint32_t fromFh, uint32_t toFh, FirstAction* out, bool ignoreSkips = false,
               int32_t fromX = kPlanNoX, int32_t toX = kPlanNoX);

// 耗时模型自校准。EstimateEdgeMs：某条边（from→to，kind，wx 对边；entryX 站的 X）按当前模型的估计，
// 找不到边 → -1。NoteEdgeObserved：一跳干净完成后回报实测耗时，按边类型更新「实测/模型」EMA 并落盘
//（bin\state\navmem\eta_scale.txt），下次规划与选怪 ETA 都用校准后的值。
int EstimateEdgeMs(uint32_t fromFh, uint32_t toFh, EdgeKind kind, int32_t wx, int32_t entryX);
void NoteEdgeObserved(EdgeKind kind, int modelMs, int observedMs);
int HopSkipCount();

// 寻路失败后跳过这条边 8s，让 Dijkstra 改走绳/其它落点（BIN 03:26 86→81 fall 连 timeout）。
// forMs=0 → 默认 8s；长周期绕圈（cycle_break）给 60s，否则一圈 5~8s 跳过刚过期又选回来。
void AddHopSkip(uint32_t fromFh, uint32_t toFh, EdgeKind kind, uint32_t forMs = 0);
void ClearHopSkips();
// 实机证明物理上过不去的边（跳 3 次够不着的悬空绳等）：整张图有效，换图重建才清，
// `ClearHopSkips` 与 `ignoreSkips` 兜底都不放行。upload 2026-09-09 11:43：绳底高 150px 的绳
// 每 25s 被重新选进路线，人在下面跳一轮又一轮。
void MarkEdgeDead(uint32_t fromFh, uint32_t toFh, EdgeKind kind);
int DeadEdgeCount();

// 该 FH 线段原始 X 范围（不做悬崖内缩）。悬崖走下用。
bool FhXRange(uint32_t fhId, int* xmin, int* xmax);

// 该 FH 线段几何 + 构图属性（诊断 / 判墙用）。
struct FhGeomInfo {
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool vertical = false;  // x1≈x2：立面 / 墙
    bool wall = false;      // SpanX<16（含窄小台）
    int walkDeg = 0;        // Walk 边数；0 = 孤立
    int walkComp = 0;
};
bool FhGeom(uint32_t fhId, FhGeomInfo* out);

// 该 FH 在 x 处的台面 Y（x 钳进段范围）。不做安全带处理。
bool FhYAt(uint32_t fhId, float x, float* outY);

// 走路 ESP：沿 dir 看 aheadPx。用当前 FH 链覆盖 + 墙段 + 落点，判断要不要跳。
enum class WalkAhead : uint8_t { Floor = 0, Jump = 1, Pit = 2 };
WalkAhead ProbeWalkAhead(float px, float py, int dir, uint32_t curFh, int aheadPx = 42);

// 爬绳落点：把绳 X 钳到 fromFh 上（台沿极限），并判断要不要跳抓（悬空 / 绳在台外）。
bool ClimbGrabHint(uint32_t fromFh, int32_t ropeX, int32_t ropeYBot, int32_t* outWalkX,
                   bool* outNeedJump);

// 人已经挂在绳/梯上时反查这根绳：X 在 xTol 内、Y 在绳段内（上下各放 24px）。
// upFh / dnFh 为构图时挂的顶台 / 底台（没挂上则 0）。
struct RopeInfo {
    int32_t x = 0;
    int32_t yTop = 0;  // AbsPos：更大 Y = 更高
    int32_t yBot = 0;
    uint32_t upFh = 0;
    uint32_t dnFh = 0;
    bool isLadder = false;
    bool upperFh = true;   // WZ uf：顶上接着台（false = 到顶也上不去，只能侧跳）
    uint32_t nodeId = 0;   // 图里的绳节点 id（kRopeNodeIdBase + 序号；0 = 图里没有）
};
bool FindRopeAt(float x, float y, int xTol, RopeInfo* out);
// 全图离 (x,y) 最近、顶底都挂了台、长度 ≥60 的绳（低血挂绳休息用）。
bool FindNearestRope(float x, float y, RopeInfo* out);
// 一次加权最短路：outCost[i] 对应构图节点 i，-1 不可达。
// Walk=1 / Climb=4 / FallDown=11。返回节点数；图未就绪 / 未知 fromFh → 0。
int MarkAllPathCost(uint32_t fromFh, int16_t* outCost, int maxN);
// 查 MarkAllPathCost 结果。不可达 / 未知 FH → false。
bool PathCostOfFh(uint32_t fh, const int16_t* cost, int n, int* outCost);

// 拟人走路耗时（ms）版最短路：按几何算走路距离 / 爬绳高度 / 下跳，不是抽象权重。
// outMs[i] = 从 fromFh（人在 fromX）走到节点 i 的估时，-1 不可达；outArriveX[i] = 到达时所在 X
//（最后一段沿台走向怪的距离由调用方再加）。斜坡图一段段小台会把「跳数」虚高，
// 而近在头顶、要绕 30 跳的怪却被欧氏距离选中（upload 2026-09-09 11:43 `hop~30`）。
int MarkAllPathTime(uint32_t fromFh, float fromX, int32_t* outMs, int32_t* outArriveX, int maxN);
bool PathTimeOfFh(uint32_t fh, const int32_t* ms, const int32_t* arriveX, int n, int* outMs,
                  int* outArriveX);

struct GraphMeta {
    bool ok = false;
    int32_t mapId = 0;
    int nodes = 0;
    int walkEdges = 0;
    int climbEdges = 0;
    int fallEdges = 0;
    int ropeLinked = 0;
    int jumpEdges = 0;  // JumpAcross + RopeJump
    int ropeNodes = 0;  // 绳节点数（nodes 不含它们）
};
bool GetGraphMeta(GraphMeta* out);

}  // namespace x::features::ports::foothold_path
