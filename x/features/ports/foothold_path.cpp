// Classic TWMS — foothold adjacency + weighted first hop.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foothold_path.h"

#include "foothold_port.h"
#include "nav_memory.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace x::features::ports::foothold_path {
namespace {

// Walk 2 + 绳 2×n + 下跳 ≤5 + 极限跳 ≤4/端 + JumpUp ≤3 + 绳侧跳（每根绳的每个目标一条）；满了新边静默丢。
// 24 时体检里挂着多根绳的长地台常满员（fh34 / fh193 deg=24），排在最后的 JumpUp / RopeJump 被丢；堆上的
// Graph 放 32 只多 ~0.3MB。构图日志 degFull= 记满员节点数。
constexpr int kMaxDeg = 32;
// 接缝 Walk 边：两块台相接（≤2px）且接口同高（≤4px）但没有 prev/next（多为异 zMass/异组）。
// 引擎里所有 foothold 都实心（组/层只是 WZ 的归组与渲染层），人走到段尾会立刻落到相接的台上继续走；
// b71cfd「异 zMass 短台穿落」实为 fill+Doing 把人写到 542 后被打回 410 的瞬移回弹，不是台不实心。
// 以 ropeIdx=-2 标记，ChainSafeXRange 不把它当链（安全钳口径不变），规划/导航/ProbeWalkAhead 照 Walk 用。
constexpr int kSeamEdgeRopeIdx = -2;
constexpr int kSeamTouchPx = 2;
constexpr int kSeamDyPx = 4;
constexpr int kRopeXTol = 48;
constexpr int kRopeCoverXTol = 4;  // 段 X 范围真盖住绳子（含段沿几像素）
constexpr int kRopeYTol = 60;
constexpr int kVerticalSpanPx = 2;   // x1≈x2 视为竖直段（立面 / 墙）
constexpr int kWallStepMaxPx = 48;   // 竖直段高 ≤ 此为台阶（可跳上）；更高为墙（链断）
// 悬空梯/绳：站台在绳底下方多少以内算一跳够得着。
// 实测（upload 2026-09-09 map101040000）：绳底高 45px 一跳就抓住；高 101~109px（fh61→绳 470）与
// 150px（fh66→绳 1606）各竖直跳 3 次全落回。起跳顶点≈80px（vy0≈525、g≈1714），抓绳只在顶点后
// 下落段发生（同日三次成功抓绳都在 +77~78px 顶点处），判定基本按脚下 Y。
// 54 张图体检：绳底离下方台 74~80px 的绳成批出现（110040000 x=-1476 / 552、101020000 x=-736），
// 是地图刻意按一跳高度摆的；72 把它们全判成够不着，整层楼就成了孤岛。放到 78（顶点 −2）；
// 真够不着的会在竖直跳 3 次后标死边并落盘，只多付一次学费。
constexpr int kRopeJumpGrabDy = 78;
constexpr int kCoverYTol = 80;
constexpr int kFallMinDropPx = 8;  // 下跳目标至少低这么多（更近的是接缝；AbsPos：更大 Y = 更高）
constexpr int kFallMinSpanX = 16; // 过窄视为墙，不做下跳起点/落点
constexpr int kFallXTol = 12;
constexpr int kCliffStepOutPx = 10;  // 走崖下跳边的采样点：段端往外这么多
// 站立落点：FH 是 Prev/Next 连成的线段链，不是孤立板。
// 内缩只作用在**整条 Walk 链的真正端点**（悬崖）；段与段接合处不内缩。
// **仅战斗** fill+Doing。F6 / 超级赶路禁止（门口/台沿要能站，BIN 19:27 east00）。
constexpr int kEndInsetCliff = 36;
constexpr int kEndInsetFly = 2;  // F6 / 赶路：只躲开段端 1px
// BIN d1a58e / 0.1.69：战斗落在段缝（fh5 右端=fh6 左端）→ RelPos.V=nan → Walk 滑链滑出图。
// 仅战斗 Snap 开启：有 Walk 邻台的端点再缩一点。
// 1d2b0b：8px 不够——斜坡落点结算后横滑 16~17px 跨缝到邻段（doing_miss，服端位置违规源头），
// 内缩必须盖过滑移量。极短段由 lo>hi 中点回退兜底。
// a7dc3e：20px 仍漏——残余 miss 里 5/8 是跨一条缝、滑移 21~30px，曾抬到 32。
// 79d048 幽深峽谷Ⅲ：fh63 左端=783 接斜坡 fh62，lo=783+32=815 正好是毒点——Doing 挂不上
// 台，CollisionDetect 掉到下层 fh60@(786,-1463)。实机 827(+44) 稳，故抬到 48。
constexpr int kJunctionInset = 48;
// 钳到安全带刀刃时再往内收一点，避免 standOff 把落点钉死在 lo/hi（815 类 miss）。
constexpr int kEdgeBiasPx = 8;
constexpr int kEdgeBiasMinSpan = 24;

struct Edge {
    uint16_t to = 0;
    EdgeKind kind = EdgeKind::Walk;
    int16_t ropeIdx = -1;  // ladders[] index; -1 = walk/fall
    int32_t wx = 0;
    int32_t wy = 0;
    int32_t aimX = 0;       // JumpAcross / RopeJump 落点 X
    int16_t ropeIdx2 = -1;  // RopeJump 抓另一根绳时的目标绳
};

struct Graph {
    int32_t mapId = 0;
    int n = 0;
    uint32_t ids[foothold::kMaxFootholds]{};
    uint8_t deg[foothold::kMaxFootholds]{};
    Edge adj[foothold::kMaxFootholds][kMaxDeg]{};
    int32_t x1[foothold::kMaxFootholds]{};
    int32_t y1[foothold::kMaxFootholds]{};
    int32_t x2[foothold::kMaxFootholds]{};
    int32_t y2[foothold::kMaxFootholds]{};
    uint8_t forbidFall[foothold::kMaxFootholds]{};
    int32_t zMass[foothold::kMaxFootholds]{};
    int walkEdges = 0;
    int climbEdges = 0;
    int fallEdges = 0;
    int ropeLinked = 0;
    int jumpEdges = 0;
    uint16_t walkComp[foothold::kMaxFootholds]{};
    uint8_t revFallDeg[foothold::kMaxFootholds]{};
    uint16_t revFall[foothold::kMaxFootholds][kMaxDeg]{};
    // 绳节点：nodeRope[idx] = 绳序号（-1 = 真台）；ropeNode[ri] = 节点下标（-1 = 没建）。
    // 几何上是 x1==x2 的竖段（IsWallFh 为真，站立 / 落点 / 探列一律跳过），只参与绳边。
    int16_t nodeRope[foothold::kMaxFootholds]{};
    int16_t ropeNode[foothold::kMaxLadders]{};
    int ropeNodes = 0;
    bool ok = false;
};

std::mutex gMu;
Graph* gGraph = nullptr;
foothold::LadderLite gLadders[foothold::kMaxLadders]{};
int gLadderN = 0;

Graph* EnsureGraphObj() {
    if (!gGraph) gGraph = new Graph{};
    return gGraph;
}

int IndexOf(const Graph& g, uint32_t id) {
    if (id == 0) return -1;
    for (int i = 0; i < g.n; ++i) {
        if (g.ids[i] == id) return i;
    }
    return -1;
}

void AddEdge(Graph& g, int from, int to, EdgeKind kind, int ropeIdx, int wx, int wy, int aimX = 0,
             int ropeIdx2 = -1) {
    if (from < 0 || to < 0 || from >= g.n || to >= g.n) return;
    if (g.deg[from] >= kMaxDeg) return;
    // de-dup same (to, kind)
    for (int i = 0; i < g.deg[from]; ++i) {
        if (g.adj[from][i].to == static_cast<uint16_t>(to) && g.adj[from][i].kind == kind)
            return;
    }
    Edge& e = g.adj[from][g.deg[from]++];
    e.to = static_cast<uint16_t>(to);
    e.kind = kind;
    e.ropeIdx = static_cast<int16_t>(ropeIdx);
    e.wx = wx;
    e.wy = wy;
    e.aimX = aimX;
    e.ropeIdx2 = static_cast<int16_t>(ropeIdx2);
    if (kind == EdgeKind::Walk)
        ++g.walkEdges;
    else if (kind == EdgeKind::JumpAcross || kind == EdgeKind::RopeJump || kind == EdgeKind::JumpUp)
        ++g.jumpEdges;
    else if (kind == EdgeKind::FallDown) {
        ++g.fallEdges;
        if (g.revFallDeg[to] < kMaxDeg) {
            bool dup = false;
            for (int i = 0; i < g.revFallDeg[to]; ++i) {
                if (g.revFall[to][i] == static_cast<uint16_t>(from)) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                g.revFall[to][g.revFallDeg[to]] = static_cast<uint16_t>(from);
                ++g.revFallDeg[to];
            }
        }
    } else
        ++g.climbEdges;
}

int SpanX(const Graph& g, int idx) {
    return std::abs(g.x2[idx] - g.x1[idx]);
}

bool IsRopeNode(const Graph& g, int idx) {
    return idx >= 0 && idx < g.n && g.nodeRope[idx] >= 0;
}

int EdgeWeight(EdgeKind kind) {
    if (kind == EdgeKind::Walk) return 1;
    if (kind == EdgeKind::ClimbUp || kind == EdgeKind::ClimbDown) return 4;
    if (kind == EdgeKind::JumpAcross) return 3;  // 一跳过去比爬绳快，但有摔空风险
    if (kind == EdgeKind::JumpUp) return 3;      // 原地跳上头顶的台：比绕去爬绳快得多
    if (kind == EdgeKind::RopeJump) return 8;    // 爬到位再侧跳：比普通爬绳多一步，且只在没别的路时用
    return 11;  // FallDown：悬崖抄近路贵过爬绳（BIN 全程不爬、只 fall 空跳）
}

// ───────── 跳跃物理 ─────────
// 从起跳点落回相对高度 h（AbsPos，h>0 = 落点更高）所需时间；h 超过顶点 → -1。
float JumpLandTime(float h) {
    const float disc = kJumpV0PxPerSec * kJumpV0PxPerSec - 2.f * kJumpGravityPxPerSec2 * h;
    if (disc < 0.f) return -1.f;
    return (kJumpV0PxPerSec + std::sqrt(disc)) / kJumpGravityPxPerSec2;
}
// 起跳后 t 秒的相对高度。
float JumpHeightAt(float t) {
    return kJumpV0PxPerSec * t - 0.5f * kJumpGravityPxPerSec2 * t * t;
}
constexpr int kJumpApexPx = 80;

// 绳上小跳版（vy0=270，顶点≈21px、0.16s 到顶）。
float RopeHopLandTime(float h) {
    const float disc = kRopeHopV0PxPerSec * kRopeHopV0PxPerSec - 2.f * kJumpGravityPxPerSec2 * h;
    if (disc < 0.f) return -1.f;
    return (kRopeHopV0PxPerSec + std::sqrt(disc)) / kJumpGravityPxPerSec2;
}
float RopeHopHeightAt(float t) {
    return kRopeHopV0PxPerSec * t - 0.5f * kJumpGravityPxPerSec2 * t * t;
}
constexpr float kRopeHopApexSec = kRopeHopV0PxPerSec / kJumpGravityPxPerSec2;

constexpr int kHopSkipCap = 16;
constexpr DWORD kHopSkipMs = 8000;
struct HopSkip {
    uint32_t from = 0;
    uint32_t to = 0;
    uint8_t kind = 0;
    DWORD until = 0;
};
HopSkip gHopSkip[kHopSkipCap]{};
int gHopSkipN = 0;

void PurgeHopSkipsUnlocked(DWORD now) {
    int w = 0;
    for (int i = 0; i < gHopSkipN; ++i) {
        if (gHopSkip[i].from && gHopSkip[i].until > now) gHopSkip[w++] = gHopSkip[i];
    }
    gHopSkipN = w;
}

// 实机学到的「物理上过不去」的边：跳了够不着的绳、穿不下去的台。整张图有效，换图重建才清；
// `ignoreSkips` 兜底规划也不用它——真是唯一路就让上层 no_path/ban 目标，别再原地空跳一轮。
constexpr int kDeadEdgeCap = 32;
struct DeadEdge {
    uint32_t from = 0;
    uint32_t to = 0;
    uint8_t kind = 0;
};
DeadEdge gDeadEdge[kDeadEdgeCap]{};
int gDeadEdgeN = 0;

bool DeadEdgeUnlocked(uint32_t fromId, uint32_t toId, EdgeKind kind) {
    const uint8_t k = static_cast<uint8_t>(kind);
    for (int i = 0; i < gDeadEdgeN; ++i) {
        if (gDeadEdge[i].from == fromId && gDeadEdge[i].to == toId && gDeadEdge[i].kind == k)
            return true;
    }
    return false;
}

// 耗时模型自校准：按边类型记「实测 / 模型」比例的 EMA，回灌 EdgeEtaMsUnlocked。
// 模型常数（上绳 2.2s 开销、下跳 0.9s……）是拍的；每完成一跳 human_nav 都有真实耗时，
// 用它把常数校到这台机器 / 这个角色的实际节奏。比例夹在 [0.5, 2.5]，单样本夹 [0.4, 3.0]，
// 前 3 个样本权重大（快速起步），之后 α=0.15。全局不分图（跨图落盘 eta_scale.txt）。
constexpr int kEtaKindN = 7;  // EdgeKind 0..6
float gEtaScale[nav_memory::kEtaKinds] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
int gEtaSamples[nav_memory::kEtaKinds]{};
bool gEtaLoaded = false;
DWORD gEtaSavedMs = 0;
constexpr DWORD kEtaSaveGapMs = 60000;

void EnsureEtaScaleLoaded() {
    if (gEtaLoaded) return;
    gEtaLoaded = true;
    if (nav_memory::LoadEtaScale(gEtaScale, gEtaSamples, kEtaKindN)) {
        x::runtime::LogI("Foothold",
                         "eta scale loaded walk=%.2f up=%.2f down=%.2f fall=%.2f across=%.2f ropejump=%.2f "
                         "(n=%d/%d/%d/%d/%d/%d)",
                         gEtaScale[0], gEtaScale[1], gEtaScale[2], gEtaScale[3], gEtaScale[4], gEtaScale[5],
                         gEtaSamples[0], gEtaSamples[1], gEtaSamples[2], gEtaSamples[3], gEtaSamples[4],
                         gEtaSamples[5]);
    }
}

float EtaScaleOf(EdgeKind kind) {
    const int k = static_cast<int>(kind);
    if (k < 0 || k >= kEtaKindN) return 1.f;
    return gEtaScale[k];
}

bool HopSkippedUnlocked(uint32_t fromId, uint32_t toId, EdgeKind kind, DWORD now) {
    if (!fromId || !toId) return false;
    if (DeadEdgeUnlocked(fromId, toId, kind)) return true;
    PurgeHopSkipsUnlocked(now);
    const uint8_t k = static_cast<uint8_t>(kind);
    for (int i = 0; i < gHopSkipN; ++i) {
        if (gHopSkip[i].from == fromId && gHopSkip[i].to == toId && gHopSkip[i].kind == k)
            return true;
    }
    return false;
}

bool IsWallFh(const Graph& g, int idx) { return SpanX(g, idx) < kFallMinSpanX; }
// 真竖墙（x1≈x2）。IsWallFh 把 <16px 的短水平段也算「墙」（不当站台 / 落点），但作为 Walk 链的接缝
// 它是能走过去的：判「段端有没有邻台接着」时只能排除真竖墙（101030404 fh109 15px 接 108/110，
// 按 IsWallFh 排掉就把 fh110 左端当成崖，走崖边永远走不掉）。
bool IsVerticalWall(const Graph& g, int idx) {
    return idx >= 0 && idx < g.n && std::abs(g.x1[idx] - g.x2[idx]) <= 2;
}

// 沿 Walk（Prev/Next）展开整条连续台面，得到链条 X 范围；仅链条端点做悬崖内缩。
// 段间接合处不内缩——那是链表节点缝，不是掉落边。
bool ChainSafeXRange(const Graph& g, int seed, int* outLo, int* outHi,
                     int endInset = kEndInsetCliff) {
    if (!outLo || !outHi || seed < 0 || seed >= g.n || IsWallFh(g, seed)) return false;

    bool seen[foothold::kMaxFootholds]{};
    int stack[foothold::kMaxFootholds];
    int sn = 0;
    stack[sn++] = seed;
    seen[seed] = true;

    int chainMin = 0x7fffffff;
    int chainMax = -0x7fffffff;
    int visited = 0;

    while (sn > 0) {
        const int u = stack[--sn];
        ++visited;
        const int xmin = (std::min)(g.x1[u], g.x2[u]);
        const int xmax = (std::max)(g.x1[u], g.x2[u]);
        if (xmin < chainMin) chainMin = xmin;
        if (xmax > chainMax) chainMax = xmax;
        for (int e = 0; e < g.deg[u]; ++e) {
            if (g.adj[u][e].kind != EdgeKind::Walk) continue;
            // 接缝边（异组相接台）不并进链：链 X 范围用于落点/站位安全钳，异组短台并进来会把内缩算错
            //（b71cfd 教训）；走不走得过去由规划/导航按 Walk 边自行处理。
            if (g.adj[u][e].ropeIdx == kSeamEdgeRopeIdx) continue;
            const int v = static_cast<int>(g.adj[u][e].to);
            if (v < 0 || v >= g.n || IsWallFh(g, v) || seen[v]) continue;
            seen[v] = true;
            stack[sn++] = v;
        }
    }
    if (visited <= 0 || chainMax < chainMin) return false;

    // 链条左右端 = 真正可能掉下去的地方；中间节点缝不缩。
    if (endInset < 0) endInset = 0;
    int lo = chainMin + endInset;
    int hi = chainMax - endInset;
    if (lo > hi) {
        const int mid = (chainMin + chainMax) / 2;
        *outLo = mid;
        *outHi = mid;
        return true;
    }
    *outLo = lo;
    *outHi = hi;
    return true;
}

// 本段某端 X 是否与 Walk 邻台共享（段缝 / 交接节点 / 相接的异组台）。
bool HasWalkNeighborAtX(const Graph& g, int idx, int edgeX) {
    for (int e = 0; e < g.deg[idx]; ++e) {
        if (g.adj[idx][e].kind != EdgeKind::Walk) continue;
        const int v = static_cast<int>(g.adj[idx][e].to);
        if (v < 0 || v >= g.n || IsVerticalWall(g, v)) continue;
        if (std::abs(g.x1[v] - edgeX) > kSeamTouchPx && std::abs(g.x2[v] - edgeX) > kSeamTouchPx) continue;
        if (!IsWallFh(g, v)) return true;
        // <16px 的短水平段：接着别的台才算接缝（101030404 fh109 接 108/110）；自己就是尽头的小短桩
        // 不算——它太短做不了跳/走崖起点，崖沿要落在本段这一端上。
        for (int e2 = 0; e2 < g.deg[v]; ++e2) {
            if (g.adj[v][e2].kind != EdgeKind::Walk) continue;
            const int w = static_cast<int>(g.adj[v][e2].to);
            if (w == idx || w < 0 || w >= g.n || IsVerticalWall(g, w)) continue;
            return true;
        }
    }
    return false;
}

// 段端 x 处有没有竖墙**高出台面**（顶 ≥ yEnd+12、底 ≤ yEnd+4）：有就不是崖，走不出去也跳不出去
// （离线 sim 50001 fh75 左端 x=1626：下面是崖面 fh74，但另一层的墙 fh2 从 150 立到 210，人顶着墙
// 走 9s 超时 ×3）。任何 zMass 的墙都算——引擎判碰撞不分层。
bool WallRisesAt(const Graph& g, int x, int yEnd, int skip = -1) {
    for (int k = 0; k < g.n; ++k) {
        if (k == skip || IsRopeNode(g, k) || !IsWallFh(g, k)) continue;
        if (std::abs(g.x1[k] - g.x2[k]) > 2) continue;  // 只认竖直段；15px 的短水平段是窄台不是墙
        if (std::abs(g.x1[k] - x) > 2) continue;
        const int top = (std::max)(g.y1[k], g.y2[k]);
        const int bot = (std::min)(g.y1[k], g.y2[k]);
        if (top >= yEnd + 12 && bot <= yEnd + 4) return true;
    }
    return false;
}

// 指定 FH 线段上可站的 X = 本段 ∩ 链条安全带。
// avoidWalkJunction：战斗用，再避开 Walk 段缝；贴门/赶路传 false（接合处可站）。
bool SafeStandXRange(const Graph& g, int idx, int* outLo, int* outHi, bool avoidWalkJunction,
                     int endInset = kEndInsetCliff) {
    if (!outLo || !outHi || idx < 0 || idx >= g.n || IsWallFh(g, idx)) return false;
    const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
    const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
    if (xmax - xmin < kFallMinSpanX) return false;

    int chainLo = 0, chainHi = 0;
    if (!ChainSafeXRange(g, idx, &chainLo, &chainHi, endInset)) return false;

    int lo = (std::max)(xmin, chainLo);
    int hi = (std::min)(xmax, chainHi);
    if (avoidWalkJunction) {
        // 段缝内缩：有 Walk 邻台的端点勿当落点（悬崖端已由 ChainSafeXRange 缩过）。
        if (HasWalkNeighborAtX(g, idx, xmin)) lo = (std::max)(lo, xmin + kJunctionInset);
        if (HasWalkNeighborAtX(g, idx, xmax)) hi = (std::min)(hi, xmax - kJunctionInset);
    }
    if (lo > hi) {
        // 本段几乎整段落在链条端点内缩带外（极短端节）：退到本段中点。
        const int mid = (xmin + xmax) / 2;
        *outLo = mid;
        *outHi = mid;
        return true;
    }
    *outLo = lo;
    *outHi = hi;
    return true;
}

int ClampToSafeStandX(const Graph& g, int idx, int ix, bool avoidWalkJunction,
                      int endInset = kEndInsetCliff) {
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi, avoidWalkJunction, endInset)) {
        const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
        const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
        return (xmin + xmax) / 2;
    }
    int cx = (std::max)(lo, (std::min)(hi, ix));
    // 宽段才偏置：贴 lo/hi 时往段内收。仅战斗；F6 / 赶路禁止再往里推。
    if (endInset >= kEndInsetCliff && hi - lo >= kEdgeBiasMinSpan) {
        if (cx == lo) cx = lo + kEdgeBiasPx;
        else if (cx == hi) cx = hi - kEdgeBiasPx;
    }
    return cx;
}

// 同 z 链被端点内缩压成单点 = 台面过短，fill+Doing 易滑落（勿当战斗落点）。
bool ChainTooNarrowToStand(const Graph& g, int idx, int endInset = kEndInsetCliff) {
    int lo = 0, hi = 0;
    if (!ChainSafeXRange(g, idx, &lo, &hi, endInset)) return true;
    return lo >= hi;
}

int FhYAtX(int x1, int y1, int x2, int y2, int x) {
    if (x1 == x2) return (y1 + y2) / 2;
    const double t = static_cast<double>(x - x1) / static_cast<double>(x2 - x1);
    return static_cast<int>(y1 + t * (y2 - y1));
}

bool FhNearPoint(const Graph& g, int idx, int x, int y, int xTol, int yTol) {
    const int xa = g.x1[idx], xb = g.x2[idx];
    const int xmin = (std::min)(xa, xb) - xTol;
    const int xmax = (std::max)(xa, xb) + xTol;
    if (x < xmin || x > xmax) return false;
    const int fy = FhYAtX(xa, g.y1[idx], xb, g.y2[idx], x);
    return std::abs(fy - y) <= yTol;
}

// 真盖住 x 的台优先于「靠 xTol 才够着」的台：绳顶 x=837 时 fh122(702..841) 与 fh98(841..1005) 同高，
// 引擎让人站在 fh122 上，构图若挂成 fh98，爬到顶落到 122 就成 landed_elsewhere → 掉回去再爬（离线 sim
// 101030102 死循环）。同 dy 下先比覆盖，再比 X 距离。
bool CoversXExact(const Graph& g, int i, int x) {
    return x >= (std::min)(g.x1[i], g.x2[i]) && x <= (std::max)(g.x1[i], g.x2[i]);
}

int BestFhNear(const Graph& g, int x, int y, int xTol, int yTol, bool skipWall = false) {
    int best = -1;
    int bestDy = 0x7fffffff;
    bool bestCover = false;
    for (int i = 0; i < g.n; ++i) {
        // 只排真竖墙：<16px 的短水平段是能站的地板碎片（弧形地面常由 9~10px 小段拼成）。按 IsWallFh 排掉，
        // 绳底就挂到 48px 外的邻段（107000401 绳 x=-756 挂成 fh616）→ 导航当台外绳助跑、在绳下蹭到超时。
        if (skipWall && (IsVerticalWall(g, i) || IsRopeNode(g, i))) continue;
        if (!FhNearPoint(g, i, x, y, xTol, yTol)) continue;
        const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], x);
        const int dy = std::abs(fy - y);
        const bool cover = CoversXExact(g, i, x);
        if (dy < bestDy || (dy == bestDy && cover && !bestCover)) {
            bestDy = dy;
            best = i;
            bestCover = cover;
        }
    }
    return best;
}

// 绳底下方（含齐平）最近可站 FH。fy 不得明显高于绳底，避免误接到上层台。
int BestFhAtOrBelow(const Graph& g, int x, int yBot, int xTol, int maxBelow) {
    int best = -1;
    int bestDy = 0x7fffffff;
    bool bestCover = false;
    for (int i = 0; i < g.n; ++i) {
        if (IsVerticalWall(g, i) || IsRopeNode(g, i)) continue;  // 短地板碎片能站，只排真竖墙（见 BestFhNear）
        const int xmin = (std::min)(g.x1[i], g.x2[i]) - xTol;
        const int xmax = (std::max)(g.x1[i], g.x2[i]) + xTol;
        if (x < xmin || x > xmax) continue;
        const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], x);
        if (fy > yBot + 12) continue;
        int dy = yBot - fy;
        if (dy < 0) dy = -dy;
        if (dy > maxBelow) continue;
        const bool cover = CoversXExact(g, i, x);
        if (dy < bestDy || (dy == bestDy && cover && !bestCover)) {
            bestDy = dy;
            best = i;
            bestCover = cover;
        }
    }
    return best;
}

// 从 x 正下方（±kFallXTol）找**物理上最近**的可站台：dy ∈ [kFallMinDropPx, kFallMaxDyPx]。没有 → -1。
int NearestFhBelow(const Graph& g, int self, int x, int yFrom) {
    int best = -1;
    int bestDy = 0x7fffffff;
    bool bestCover = false;
    for (int j = 0; j < g.n; ++j) {
        if (j == self) continue;
        // <16px 的水平碎片也接得住人（107000402 fh473 穿台点正下方 13px 的 fh79：按墙跳过就连到 500px 下的 fh513，
        // 人每次都落在 fh79 上再规划）；只排真竖墙 / 绳节点。
        if (IsVerticalWall(g, j) || IsRopeNode(g, j)) continue;
        const int jmin = (std::min)(g.x1[j], g.x2[j]) - kFallXTol;
        const int jmax = (std::max)(g.x1[j], g.x2[j]) + kFallXTol;
        if (x < jmin || x > jmax) continue;
        const int yTo = FhYAtX(g.x1[j], g.y1[j], g.x2[j], g.y2[j], x);
        const int dy = yFrom - yTo;  // AbsPos：>0 = 目标更低
        if (dy <= 2 || dy > kFallMaxDyPx) continue;
        const bool cover = CoversXExact(g, j, x);
        if (dy < bestDy || (dy == bestDy && cover && !bestCover)) {
            bestDy = dy;
            best = j;
            bestCover = cover;
        }
    }
    // 物理上人只会落到**正下方最近**的那块台；旧口径（最少低 45）跳过近台去找更低的台，边就指向了一块
    // 根本落不到的台（离线 sim 103000002 楼梯：fh11 下 21px 是 fh10，边却连到 63px 下的 fh8）。
    // 8px 内是接缝不连；8px 以上的矮台阶照样是下跳（叠台楼梯只能这样一层层下）。
    if (best >= 0 && bestDy < kFallMinDropPx) return -1;
    return best;
}

// 穿台点旁边有没有绳：站在绳顶 ±20px 内按 ↓ 会**上绳**而不是穿台（BIN 2026-09-09 map101040000 x=668 绳顶台
// fh197：穿台点 664 → ↓ 一按就 resume_on_rope → 爬到顶 → 下台 → 再穿 → 3 轮 loop_break，每处 2.5s）。
// 绳段上下各放 24px：绳顶接台 / 绳穿过台面都算。
constexpr int kDropRopeAvoidPx = 14;
bool RopeNearForDrop(int x, int y) {
    for (int ri = 0; ri < gLadderN; ++ri) {
        const auto& lr = gLadders[ri];
        if (std::abs(lr.x - x) > kDropRopeAvoidPx) continue;
        const int yTop = (std::max)(lr.y1, lr.y2);
        const int yBot = (std::min)(lr.y1, lr.y2);
        if (y >= yBot - 24 && y <= yTop + 24) return true;
    }
    return false;
}

// 下跳边两类：
//  · 穿台（可穿的台，段内 1/4、1/2、3/4 三点，↓+Alt 穿下去）；
//  · 走崖（**任何**台的两端：端点没有 Walk 邻台接着 → 是崖沿/高墙 → 往外走 10px 掉到正下方的台）。
// forbidFall（不可穿的实地）以前整段没有下跳边，从高台下不来：BIN 2026-09-09 map30000
// fh7(y=-215) → 门 fh85(y=687) no_path 8s 熔断。走崖 wx 落在段外，导航直接按走崖处理。
void AddFallDownEdges(Graph& g) {
    for (int i = 0; i < g.n; ++i) {
        if (IsWallFh(g, i)) continue;
        const int xa = g.x1[i], xb = g.x2[i];
        const int xmin = (std::min)(xa, xb);
        const int xmax = (std::max)(xa, xb);

        if (!g.forbidFall[i]) {
            const int xs[3] = {xa + (xb - xa) / 4, (xa + xb) / 2, xa + 3 * (xb - xa) / 4};
            for (int s = 0; s < 3; ++s) {
                int x = xs[s];
                const int yFrom = FhYAtX(xa, g.y1[i], xb, g.y2[i], x);
                const int best = NearestFhBelow(g, i, x, yFrom);
                if (best < 0) continue;
                // NearestFhBelow 放宽 ±12px 找台：穿台点可能落在落点台 X 范围外 3px（离线 sim 101030402
                // dropX=1271、台从 1274 起 → 穿过去掉到再下一层）。把穿台点挪进落点台里（留 6px），
                // 挪完还得在本段里，否则这个点不连。
                const int bmin = (std::min)(g.x1[best], g.x2[best]) + 6;
                const int bmax = (std::max)(g.x1[best], g.x2[best]) - 6;
                if (bmax < bmin) continue;
                if (x < bmin) x = bmin;
                if (x > bmax) x = bmax;
                if (x < xmin + 4 || x > xmax - 4) continue;
                // 绳顶 ±20px 内按 ↓ 会上绳：穿台点往两边挪 24px 避开，挪不开就放弃这个点。
                if (RopeNearForDrop(x, FhYAtX(xa, g.y1[i], xb, g.y2[i], x))) {
                    bool moved = false;  // X 可为负，不能拿 -1 当哨兵
                    for (int sgn = -1; sgn <= 1 && !moved; sgn += 2) {
                        const int xx = x + sgn * (kDropRopeAvoidPx + 4);
                        if (xx < bmin || xx > bmax || xx < xmin + 4 || xx > xmax - 4) continue;
                        if (RopeNearForDrop(xx, FhYAtX(xa, g.y1[i], xb, g.y2[i], xx))) continue;
                        x = xx;
                        moved = true;
                    }
                    if (!moved) continue;
                }
                // 挪过位置后正下方最近的台可能换了（挪进去的那几像素上方/下方另有台）：换了就按新点重算。
                const int yAt = FhYAtX(xa, g.y1[i], xb, g.y2[i], x);
                const int best2 = NearestFhBelow(g, i, x, yAt);
                if (best2 < 0) continue;
                if (best2 != best) {
                    const int b2min = (std::min)(g.x1[best2], g.x2[best2]) + 6;
                    const int b2max = (std::max)(g.x1[best2], g.x2[best2]) - 6;
                    if (x < b2min || x > b2max) continue;  // 新落点台在这一点上也站不稳：放弃这个点
                }
                AddEdge(g, i, best2, EdgeKind::FallDown, -1, x, yAt);
            }
        }

        // 走崖：人带着走速掉下去会往前飘几十像素，下面的台常从崖沿再往外一点才开始，
        // 端点外 10 / 40 / 80 三处依次找，取最近命中的那处。
        const int ends[2] = {xmin, xmax};
        const int dirs[2] = {-1, 1};
        const int outs[3] = {kCliffStepOutPx, 40, 80};
        for (int e = 0; e < 2; ++e) {
            if (HasWalkNeighborAtX(g, i, ends[e])) continue;  // 段缝 / 台阶：不是崖
            const int yFrom = FhYAtX(xa, g.y1[i], xb, g.y2[i], ends[e]);
            if (WallRisesAt(g, ends[e], yFrom, i)) continue;   // 端点立着墙：走不出去
            // 端点外 6px 处若有同高的台（相接但 Y 差 4~8px、AddSeamWalkEdges 没连上的）：走出去是踏上它不是掉下去。
            {
                const int probeX = ends[e] + dirs[e] * 6;
                bool seam = false;
                for (int k = 0; k < g.n && !seam; ++k) {
                    if (k == i || IsRopeNode(g, k) || IsVerticalWall(g, k)) continue;
                    if (probeX < (std::min)(g.x1[k], g.x2[k]) || probeX > (std::max)(g.x1[k], g.x2[k])) continue;
                    seam = std::abs(FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], probeX) - yFrom) <= 8;
                }
                if (seam) continue;
            }
            // 人是带着走速（125px/s）走出崖沿的，落多深就飘多远：155px 的落差飘 53px。以前只在端点外 10/40/80
            // 找「正下方最近的台」，落点台的远端离崖沿 3px 也连（101040000 fh113→fh203：台在崖沿正下方 155px、
            // 只伸出崖沿 3px），人飘过去掉进虚空。按抛物线逐 8px 采样，找**第一块被穿过**的台；没有就不连。
            // 每块候选台按自己的落差解出**精确**穿越点 x_c = 崖沿 + v·sqrt(2h/g)，要求落在台内且离台沿 ≥4px
            //（110040000 fh24→fh82：落差 60 飘 33.1px，穿越点 -163.9 离 fh82 左端 -163 差 0.9px，8px 步进采样
            // 判成「接住」，实机 / sim 都擦着崖面掉了 700px）。同时满足的取落差最小的那块（先碰到的）。
            // 先按物理找**第一块被穿过**的台（穿越点在台内即算接住，不带余量），再看这块台接得稳不稳（穿越点离台沿
            // ≥4px）；不稳就整个崖沿不连——不能因为第一块只差 1px 就跳过它去连下面那块（105040300 fh30：fh112
            // 穿越点 283.2 离台沿 282 只有 1.2px 被余量排掉，连到 60px 下的 fh108，人实际落在 fh112 → 每次都 fall_landed）。
            // 抛物线按 2px 步进、逐台查「上一步在台面上方、这一步在下方」（斜坡上解不动点会漏：103010000 fh268 崖沿外
            // 是往上爬的陡坡 fh140，穿越点在 h≈7 处，按台中点估的落差 22 解出来在台外）。候选先按 X 侧向 / 高度粗筛。
            int landFh = -1;
            {
                int cand[96];
                int cn = 0;
                const float xFar = static_cast<float>(ends[e]) + static_cast<float>(dirs[e]) * 480.f;
                for (int k = 0; k < g.n && cn < 96; ++k) {
                    if (k == i || IsRopeNode(g, k) || IsVerticalWall(g, k)) continue;
                    const int kmin = (std::min)(g.x1[k], g.x2[k]), kmax = (std::max)(g.x1[k], g.x2[k]);
                    if (dirs[e] > 0 ? (kmax < ends[e] || static_cast<float>(kmin) > xFar)
                                    : (kmin > ends[e] || static_cast<float>(kmax) < xFar))
                        continue;
                    const int kyHi = (std::max)(g.y1[k], g.y2[k]);
                    if (kyHi > yFrom + 2) continue;  // 比崖沿还高的不接（走出去撞不到它的面）
                    if (yFrom - (std::min)(g.y1[k], g.y2[k]) > kFallMaxDyPx + 80) continue;
                    cand[cn++] = k;
                }
                float prevY = static_cast<float>(yFrom);
                float prevX = static_cast<float>(ends[e]);
                float landX = 0.f;
                for (int step = 1; step <= 240 && landFh < 0; ++step) {
                    const float xs = static_cast<float>(ends[e]) + static_cast<float>(dirs[e] * step * 2);
                    const float t = static_cast<float>(step * 2) / kJumpAirVxPxPerSec;
                    const float ys = static_cast<float>(yFrom) - 0.5f * kJumpGravityPxPerSec2 * t * t;
                    if (yFrom - ys > kFallMaxDyPx) break;
                    float bestKy = -1e9f;
                    for (int c = 0; c < cn; ++c) {
                        const int k = cand[c];
                        const int kmin = (std::min)(g.x1[k], g.x2[k]), kmax = (std::max)(g.x1[k], g.x2[k]);
                        const bool covNow = xs >= static_cast<float>(kmin) && xs <= static_cast<float>(kmax);
                        const bool covPrev = prevX >= static_cast<float>(kmin) && prevX <= static_cast<float>(kmax);
                        if (!covNow && !covPrev) continue;
                        const int xqNow = (std::min)((std::max)(static_cast<int>(xs), kmin), kmax);
                        const int xqPrev = (std::min)((std::max)(static_cast<int>(prevX), kmin), kmax);
                        const float kyNow = static_cast<float>(FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], xqNow));
                        const float kyPrev = static_cast<float>(FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], xqPrev));
                        if (prevY >= kyPrev - 0.5f && ys <= kyNow && yFrom - kyNow >= 2.f && kyNow > bestKy) {
                            bestKy = kyNow;
                            landFh = k;
                            landX = xs;
                        }
                    }
                    prevY = ys;
                    prevX = xs;
                }
                // 接得稳：穿越点离台沿 ≥4px（差 1px 擦着崖面掉下去 700px 的账已经交过）；落在 <16px 的碎片上也不算稳。
                if (landFh >= 0) {
                    const int kmin = (std::min)(g.x1[landFh], g.x2[landFh]), kmax = (std::max)(g.x1[landFh], g.x2[landFh]);
                    const bool safe = landX >= static_cast<float>(kmin) + 4.f && landX <= static_cast<float>(kmax) - 4.f &&
                                      !IsWallFh(g, landFh);
                    if (!safe) landFh = -1;
                }
            }
            // 抛物线 720px 内没接到台 = 走出去是掉进虚空 / 掉楼：不连（老口径「端点外 10/40/80 找正下方最近的台」
            // 会把飘不到的台也连上，人走出去就死）。
            if (landFh >= 0) AddEdge(g, i, landFh, EdgeKind::FallDown, -1, ends[e] + dirs[e] * kCliffStepOutPx, yFrom);
            (void)outs;
        }
    }
}

void Mid(const Graph& g, int idx, int& ox, int& oy) {
    ox = (static_cast<int>(g.x1[idx]) + static_cast<int>(g.x2[idx])) / 2;
    oy = (static_cast<int>(g.y1[idx]) + static_cast<int>(g.y2[idx])) / 2;
}

// ───────── 极限跳（JumpAcross / RopeJump）─────────
// 有些图的台面是刻意设计的：要从崖沿助跑一跳越过空隙、跳上不相连的高台，或先爬一根绳再从
// 绳上侧跳到台 / 另一根绳，才能站上去。Prev/Next 步行边、绳顶绳底、下跳都覆盖不到这些，
// 以前这类区域直接 no_path。这里按跳跃物理把「够得着」的候选连成边，导航按点起跳。
constexpr int kAcrossMinGapPx = 8;        // 比这近的两段其实是接缝，Walk 边管
constexpr int kAcrossMaxDropPx = 260;     // 落点比起跳低这么多以内（更低的交给下跳边）
constexpr int kAcrossMaxRisePx = kJumpApexPx - 10;  // 顶点 80 留 10：帧量化 + 键延迟下 72~78 的台常差 1~2px 落不上
constexpr int kAcrossLandMarginPx = 14;   // 落点至少进台沿这么多
// 助跑：走速 0→125 只要 ≈8px（BIN 落地 vx 62→104→125 共 ~130ms），旧 40px 把 24~45px 的窄台
// （楼梯 / 树枝，54 张图连通性体检里最常见的断点）全判成跳不起来。
constexpr int kAcrossMinRunUpPx = 16;
constexpr int kAcrossMinSpanPx = 24;      // 太短的台跳不起来也站不稳
// 原地竖直跳上头顶的台：台面单向，从下往上能穿。上台与下台 X 重叠 ≥20px、高 6~72px（顶点 80 留余量）、
// 中间没有别的台先接住。54 张图体检里「dx=0 dy=+60」是最多的一类断点（树枝 / 楼层 60px 一层）。
constexpr int kStepUpDyPx = 12;           // 高这么多才算台阶（与 ProbeWalkAhead 的 kStepUpMin 同口径）
constexpr int kJumpUpMinOverlapPx = 8;    // 竖直跳落点准（起跳前停稳 |vx|≤15），窄台 / 错位 10px 的台也站得上
constexpr int kJumpUpEdgePadPx = 2;
constexpr int kJumpUpMinRisePx = 6;
constexpr int kJumpUpMaxRisePx = kAcrossMaxRisePx;
constexpr int kJumpUpMaxPerNode = 3;
constexpr int kRopeJumpMinGapPx = 6;      // 绳侧台离绳至少这么远（更近的是穿洞台，不能中途下）
constexpr int kRopeJumpMaxGapPx = 100;
constexpr int kRopeJumpAboveLandPx = 30;  // 从落点上方 30px 起跳：往下落到台上，比往上够更稳
constexpr int kRopeJumpMaxDropPx = 160;   // 绳侧跳落点最多比起跳点低这么多（再低的是掉楼，交给 FallDown）
constexpr int kRopeEndMarginPx = 10;      // 绳两端 10px 内不起跳（顶/底自有边）
// uf=0 的绳顶端封住、爬不出去，可以站到最顶再跳（爬到 wy±6 就跳）。接力绳等高排列、间距 ~100px，
// 从顶端起跳落到下一根中下段刚刚好，多留 6px 就把 map101040000 rope#18→#19 判成不可达。
// uf=1 的绳顶再按 ↑ 会走上台，仍留 10。
constexpr int kRopeTopLaunchMarginNoExitPx = 4;
int RopeTopLaunchMargin(const foothold::LadderLite& lr) {
    return lr.isUpperFh ? kRopeEndMarginPx : kRopeTopLaunchMarginNoExitPx;
}
// 绳到绳横距：map101040000 的接力绳相邻 89~110px（x=920/1017/1114/1203/1309/1407/1516），旧上限 90
// 把大半都排除了；能不能到由下落段可行域算，不靠这个上限兜。
constexpr int kRopeToRopeMaxDx = 130;
constexpr int kRopeToRopeMinDx = 12;
constexpr int kRopeToRopeGrabMarginPx = 20;  // 飞到目标绳时人的 Y 要在绳段内留这么多余量
constexpr int kRopeGrabHalfWinPx = 10;       // 抓绳 X 窗半宽（实测 ±8~11）

int gRopeUp[foothold::kMaxLadders];
int gRopeDn[foothold::kMaxLadders];
// 绳侧台：不在绳正下方、但站在上面朝绳助跑一跳（台外绳口径）能在下落段穿过绳段的台，每侧最多一块。
// 绳底吊得比地面高 120~190px 的绳（体检 x=-498 dy=187 / x=1629 dy=125 / x=320 dy=132）只能这样上。
int gRopeSide[foothold::kMaxLadders][2];
constexpr int kRopeSideMinGapPx = 8;   // ≥8 才保证 ClimbGrabHint 判成台外绳（pastEdge 用 >6 严格比较）
constexpr int kRopeSideMaxGapPx = 60;
constexpr int kRopeSideMinSpanPx = 24;
constexpr int kRopeSideYMarginPx = 6;

// a 与 b 是否几步 Walk 边就连着（构图期 walkComp 还没标，用小步 BFS）。
bool WalkLinkedWithin(const Graph& g, int a, int b, int maxDepth) {
    if (a == b) return true;
    int frontier[64];
    int fn = 0;
    frontier[fn++] = a;
    static uint8_t seen[(foothold::kMaxFootholds + 7) / 8];
    std::memset(seen, 0, sizeof(seen));
    auto mark = [&](int v) { seen[v >> 3] = static_cast<uint8_t>(seen[v >> 3] | (1u << (v & 7))); };
    auto isSeen = [&](int v) -> bool { return (seen[v >> 3] >> (v & 7)) & 1u; };
    mark(a);
    for (int depth = 0; depth < maxDepth && fn > 0; ++depth) {
        int next[64];
        int nn = 0;
        for (int i = 0; i < fn; ++i) {
            const int u = frontier[i];
            for (int e = 0; e < g.deg[u]; ++e) {
                if (g.adj[u][e].kind != EdgeKind::Walk) continue;
                const int v = g.adj[u][e].to;
                if (v == b) return true;
                if (v < 0 || v >= g.n || isSeen(v) || nn >= 64) continue;
                mark(v);
                next[nn++] = v;
            }
        }
        fn = nn;
        for (int i = 0; i < nn; ++i) frontier[i] = next[i];
    }
    return false;
}

void FindRopeSidePlatforms(const Graph& g, int ri, int up, const int* dns, int dnN) {
    gRopeSide[ri][0] = gRopeSide[ri][1] = -1;
    const auto& lr = gLadders[ri];
    const int yBot = (std::min)(lr.y1, lr.y2);
    const int yTop = (std::max)(lr.y1, lr.y2);
    // 绳底站在底台上就够得着（落地绳，dns 里的都在 kRopeYTol 内）：与任一底台同一条地板上的邻段不必当侧台——
    // 从邻段「台外助跑跳抓」是把落地绳当成了悬绳，导航在绳下左右蹭 9s 不跳（离线 sim 107000401 fh616→绳
    // x=-756：绳底 598 就是 fh614 地面，fh616 只是右边 10px 外的下一段）。走到底台按 ↑ 即可。
    // 小段地板一条链可能十几段，BFS 放到 12 步。
    auto isDn = [&](int k) {
        for (int i = 0; i < dnN; ++i)
            if (dns[i] == k) return true;
        return false;
    };
    auto onDnFloor = [&](int k) {
        for (int i = 0; i < dnN; ++i)
            if (WalkLinkedWithin(g, k, dns[i], 12)) return true;
        return false;
    };
    int bestGap[2] = {1 << 30, 1 << 30};
    for (int k = 0; k < g.n; ++k) {
        if (k == up || isDn(k) || IsRopeNode(g, k) || IsWallFh(g, k) || SpanX(g, k) < kRopeSideMinSpanPx) continue;
        if (dnN > 0 && onDnFloor(k)) continue;
        const int kmin = (std::min)(g.x1[k], g.x2[k]);
        const int kmax = (std::max)(g.x1[k], g.x2[k]);
        int side, gap, nearX;
        if (kmax < lr.x) {
            side = 0;
            gap = lr.x - kmax;
            nearX = kmax;
        } else if (kmin > lr.x) {
            side = 1;
            gap = kmin - lr.x;
            nearX = kmin;
        } else {
            continue;  // 盖住绳 X：那是正下方 / 正上方的台，走 dn / up 口径
        }
        if (gap < kRopeSideMinGapPx || gap > kRopeSideMaxGapPx) continue;
        // 绳离台沿 ≤48 时起跳点在绳前 48px 处，台面得从台沿往回至少有 (48−gap)+8 px 才站得下这个起跳点；
        // 不够长就得刹到中间速度再跳——MS 走速基本是 0/125 二值，刹不出中间速度（离线 sim 102000000 fh352：
        // 27px 台、绳在沿外 11px，走/停交替永远等不到起跳窗，走出台沿掉下去 ×7）。窄台不当侧台。
        if (gap <= 48 && SpanX(g, k) < 48 - gap + 8) continue;
        const int fy = FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], nearX);
        // 台外绳起跳点在绳前 ≈48px：绳离台沿 ≤48 就在顶点（+80）处穿绳，再远就从台沿起跳、
        // 到绳时已在下落段。穿绳窗 ±10px 里人的 Y 从 hIn 掉到 hOut，这一段要与绳段有交集。
        // 台沿起跳其实发生在台沿内侧 7px + 键延迟提前量（≈12px）处，飞行距离按 gap+12 算（离线 sim
        // 1000002 fh28→绳 1410：gap=60 按 60 算穿绳高 +73，实飞 71px 到绳时已在绳底下 25px）。
        float hIn, hOut;
        if (gap <= 48) {
            hIn = static_cast<float>(kJumpApexPx);
            hOut = JumpHeightAt(kJumpV0PxPerSec / kJumpGravityPxPerSec2 + 20.f / kJumpAirVxPxPerSec);
        } else {
            const int effGap = gap + 12;
            hIn = JumpHeightAt(static_cast<float>(effGap - 10) / kJumpAirVxPxPerSec);
            hOut = JumpHeightAt(static_cast<float>(effGap + 10) / kJumpAirVxPxPerSec);
        }
        if (fy + hIn < static_cast<float>(yBot + kRopeSideYMarginPx)) continue;   // 最高点还在绳底之下
        if (fy + hOut > static_cast<float>(yTop - kRopeSideYMarginPx)) continue;  // 最低点还在绳顶之上
        // 台沿到绳之间立着墙（跨过人飞行高度带 fy..fy+80）就跳不过去。
        bool walled = false;
        const int wlo = (std::min)(nearX, lr.x), whi = (std::max)(nearX, lr.x);
        for (int w = 0; w < g.n && !walled; ++w) {
            if (IsRopeNode(g, w) || !IsWallFh(g, w) || std::abs(g.x1[w] - g.x2[w]) > 2) continue;
            if (g.x1[w] <= wlo || g.x1[w] >= whi) continue;
            const int top = (std::max)(g.y1[w], g.y2[w]), bot = (std::min)(g.y1[w], g.y2[w]);
            walled = top > fy + 4 && bot < fy + kJumpApexPx;
        }
        if (walled) continue;
        if (gap < bestGap[side]) {
            bestGap[side] = gap;
            gRopeSide[ri][side] = k;
        }
    }
}

int NearEndX(const Graph& g, int j, int dir) {
    return dir > 0 ? (std::min)(g.x1[j], g.x2[j]) : (std::max)(g.x1[j], g.x2[j]);
}
int FarEndX(const Graph& g, int j, int dir) {
    return dir > 0 ? (std::max)(g.x1[j], g.x2[j]) : (std::min)(g.x1[j], g.x2[j]);
}
int YAtClamped(const Graph& g, int idx, int x) {
    const int lo = (std::min)(g.x1[idx], g.x2[idx]);
    const int hi = (std::max)(g.x1[idx], g.x2[idx]);
    const int cx = x < lo ? lo : (x > hi ? hi : x);
    return FhYAtX(g.x1[idx], g.y1[idx], g.x2[idx], g.y2[idx], cx);
}

// 起跳点 xFrom 到落点 xTo 之间有没有别的台会先把人接住：台面是单向的（从下往上能穿过），
// 只有轨迹在某段台的 X 范围内**从上往下穿过**台面才会被接住。轨迹 y(x) = 起跳 Y + 抛物线。
// 单测 A→E：绳顶台 D 在起跳点上方、轨迹在 D 右端以上飞过再落到 E，粗判「Y 在区间内」会误杀。
bool JumpPathBlockedAt(const Graph& g, int skipA, int skipB, int xFrom, int xTo, int yJump, float airVx, float v0) {
    const int d = xTo >= xFrom ? 1 : -1;
    const int lo = (std::min)(xFrom, xTo) + 4;
    const int hi = (std::max)(xFrom, xTo) - 4;
    if (hi <= lo || airVx <= 1.f) return false;
    auto trajY = [&](int x) -> float {
        const float t = static_cast<float>(std::abs(x - xFrom)) / airVx;
        return static_cast<float>(yJump) + v0 * t - 0.5f * kJumpGravityPxPerSec2 * t * t;
    };
    for (int k = 0; k < g.n; ++k) {
        if (k == skipA || k == skipB || IsRopeNode(g, k)) continue;
        const int kmin = (std::min)(g.x1[k], g.x2[k]);
        const int kmax = (std::max)(g.x1[k], g.x2[k]);
        // 飞行路径上立着的竖墙：人飞到墙的 X 时脚到头（40px）这一段与墙段有交集就撞墙停下（MS 撞墙横速归零，
        // 沿墙滑落）。起跳台自己的崖面在台面之下、目标台的崖面在落点之下，不会撞；夹在中间的墙会。
        if (IsVerticalWall(g, k)) {
            const int wxk = g.x1[k];
            if (wxk <= lo || wxk >= hi) continue;
            const float feet = trajY(wxk);
            const float wTop = static_cast<float>((std::max)(g.y1[k], g.y2[k]));
            const float wBot = static_cast<float>((std::min)(g.y1[k], g.y2[k]));
            if (wTop >= feet + 2.f && wBot <= feet + 40.f) return true;
            continue;
        }
        if (IsWallFh(g, k)) continue;
        if (kmax < lo || kmin > hi) continue;
        const int sx = (std::max)(lo, kmin);
        const int ex = (std::min)(hi, kmax);
        if (ex - sx < 2) continue;
        // 沿飞行方向：先进入的 X 到后离开的 X。台面上方→下方的穿越点可能在段内任何位置（回到起跳高度
        // 的那一点常落在段中间），按 8px 步进逐点查，不只看进出两端。
        const int xin = d > 0 ? sx : ex;
        const int xout = d > 0 ? ex : sx;
        // 台面单向：从下往上穿过去不算被接住；只有先在台面上方、之后下落段落到台面才算。
        // 从下方进入再升到上方再落回来的也要算（跨过一块中间高台时）。
        auto descendingAt = [&](int x) -> bool {
            const float t = static_cast<float>(std::abs(x - xFrom)) / airVx;
            return v0 - kJumpGravityPxPerSec2 * t < 0.f;
        };
        bool wasAbove = false;
        auto probe = [&](int x) -> bool {
            const float dy = trajY(x) - static_cast<float>(FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], x));
            if (dy > 3.f) {
                wasAbove = true;
                return false;
            }
            return wasAbove && descendingAt(x);  // 从上方落到台面 → 被接住
        };
        // 擦沿：下落段飞到这块台的近沿时，人只比台面低 ≤8px——差两像素就落在它上面（1000001 fh67→fh163：
        // 飞过 fh64 近沿时 +52~+63 对 +60，sim 落到 fh64）。这种台当作会接住，别赌。
        auto grazesEdge = [&](int x) -> bool {
            if (!descendingAt(x)) return false;
            const float dy = trajY(x) - static_cast<float>(FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], x));
            return dy <= 3.f && dy >= -8.f;
        };
        bool hit = grazesEdge(xin);
        for (int x = xin; (d > 0 ? x <= xout : x >= xout) && !hit; x += d * 8) hit = probe(x);
        if (!hit) hit = probe(xout);
        if (hit) return true;
    }
    return false;
}

// 起跳点 xFrom 到落点 xTo 之间有没有别的台会先把人接住。规划按 92% 横速算可达（保守），但被接住
// 与否两头都要看：横速慢一点会提前落回起跳高度、快一点会飞过缝落到对面同高的台上
//（离线 sim map30000 fh26→fh37：按 115px/s 算刚好从 12px 缝里落下去，实飞 125 就落到了 fh25 上）。
bool JumpPathBlocked(const Graph& g, int skipA, int skipB, int xFrom, int xTo, int yJump, float airVx,
                     float v0 = kJumpV0PxPerSec) {
    if (JumpPathBlockedAt(g, skipA, skipB, xFrom, xTo, yJump, airVx, v0)) return true;
    const float full = airVx / kJumpReachFactor;
    if (full > airVx + 1.f && JumpPathBlockedAt(g, skipA, skipB, xFrom, xTo, yJump, full, v0)) return true;
    return false;
}

void AddJumpAcrossEdges(Graph& g) {
    for (int i = 0; i < g.n; ++i) {
        if (IsWallFh(g, i) || SpanX(g, i) < kAcrossMinSpanPx) continue;
        const int imin = (std::min)(g.x1[i], g.x2[i]);
        const int imax = (std::max)(g.x1[i], g.x2[i]);
        const int ends[2] = {imin, imax};
        const int dirs[2] = {-1, 1};
        for (int e = 0; e < 2; ++e) {
            const int endX = ends[e];
            const int d = dirs[e];
            if (HasWalkNeighborAtX(g, i, endX)) continue;  // 接缝 / 台阶不是崖沿
            const int yEnd = YAtClamped(g, i, endX);
            const int otherEnd = ends[1 - e];
            // 每个崖沿最多连 4 个落点（按空隙从近到远），别把邻接表塞满。
            struct Cand {
                int j = -1;
                int gap = 0;
                int xOff = 0;
                int aimX = 0;
            };
            Cand cands[4]{};
            int nCand = 0;
            for (int j = 0; j < g.n; ++j) {
                if (j == i || IsWallFh(g, j) || SpanX(g, j) < 16) continue;
                const int nearX = NearEndX(g, j, d);
                // 落点远端按整条 Walk 链算：台面常被切成 23px 的小段（map101040000 fh172 787..810），
                // 只看本段会把「落到隔壁段」判成落不下，44px 的同高空隙就此 no_path。
                int farX = FarEndX(g, j, d);
                int clo = 0, chi = 0;
                if (ChainSafeXRange(g, j, &clo, &chi, /*endInset=*/0)) farX = d > 0 ? chi : clo;
                const int gap = d * (nearX - endX);
                const int yLand = YAtClamped(g, j, nearX);
                const int h = yLand - yEnd;  // AbsPos：>0 落点更高
                if (h > kAcrossMaxRisePx || h < -kAcrossMaxDropPx) continue;
                // 重叠 / 相接的两段本是接缝、Walk 边管——但这一端已确认没有 Walk 邻台（崖沿），
                // 贴着崖沿更高 ≥12px 的不相连台就是要跳上去的台阶（101000000 fh992→fh731 dx=4 dy=+42）。
                if (gap < kAcrossMinGapPx && !(h >= kStepUpDyPx && gap >= -4)) continue;
                if (gap < -4) continue;  // 真重叠 / 在身后
                const float t = JumpLandTime(static_cast<float>(h));
                if (t <= 0.f) continue;
                const float reach = kJumpAirVxPxPerSec * t * kJumpReachFactor;
                if (static_cast<float>(gap + kAcrossLandMarginPx) > reach) continue;
                // 落点：进台沿 min(30, 半段)；起跳点 = 落点倒推一跳距离，钳进本段并留助跑。
                const int inset = (std::min)(30, SpanX(g, j) / 2);
                const int aimX = nearX + d * inset;
                int xOff = aimX - d * static_cast<int>(std::lround(reach));
                // 不得越过崖沿：留 4px（跳键到引擎起跳还有一帧 ≈2px，贴到 2px 内易走出崖沿）
                if (d * (xOff - endX) > -4) xOff = endX - d * 4;
                if (d * (xOff - otherEnd) < kAcrossMinRunUpPx) {
                    // 助跑不够长就从段头起跑（落点会偏远），再核一次仍落在台上。
                    xOff = otherEnd + d * kAcrossMinRunUpPx;
                    if (d * (xOff - endX) > -4) continue;
                    const int xLand = xOff + d * static_cast<int>(std::lround(reach));
                    if (d * (xLand - nearX) < kAcrossLandMarginPx || d * (farX - xLand) < 4) continue;
                }
                // 全速（100%）飞比按 92% 规划的落点远 9%：窄落点台（28px）远端离落点只有几像素时全速就飞过去了
                //（lat100 102010000 fh155→fh150：aim 离远端 14px，落 -821 出台 8px 掉到 fh40 且无路）。落点按
                // 全速也得离远端 ≥6px，不够就把起跳点往后挪，挪后近端余量 / 助跑长度仍要成立。
                {
                    const int xLandFull = xOff + d * static_cast<int>(std::lround(reach / kJumpReachFactor));
                    const int over = 6 - d * (farX - xLandFull);
                    if (over > 0) {
                        xOff -= d * over;
                        if (d * (xOff - otherEnd) < kAcrossMinRunUpPx) continue;
                        const int xLandSlow = xOff + d * static_cast<int>(std::lround(reach));
                        if (d * (xLandSlow - nearX) < kAcrossLandMarginPx) continue;
                    }
                }
                // 查到真正的落点（不是台沿）：飞过台沿后还要再飞 inset 才落地，叠台楼梯里那一段会先被
                // 上一层接住（离线 sim 103000002 fh14→fh8：台沿前没挡，落点前被 fh10 接住 → elsewhere）。
                const int xLandChk = xOff + d * static_cast<int>(std::lround(reach));
                if (JumpPathBlocked(g, i, j, xOff, xLandChk, yEnd, kJumpAirVxPxPerSec * kJumpReachFactor))
                    continue;
                // 插入有序小表（gap 升序），满 4 个就挤掉最远的。
                int pos = nCand;
                while (pos > 0 && cands[pos - 1].gap > gap) --pos;
                if (pos >= 4) continue;
                const int last = nCand < 4 ? nCand : 3;
                for (int k = last; k > pos; --k) cands[k] = cands[k - 1];
                cands[pos] = Cand{j, gap, xOff, aimX};
                if (nCand < 4) ++nCand;
            }
            for (int c = 0; c < nCand; ++c)
                AddEdge(g, i, cands[c].j, EdgeKind::JumpAcross, -1, cands[c].xOff, yEnd, cands[c].aimX);
        }
    }
}

// 原地竖直跳上头顶的台。起跳 X 取「下台可站带 ∩ 上台 X 范围」的中点；上下台之间在该 X 上不能夹着
// 别的台（会先落到它上面——那就连到它）。已有 Walk 边相连的不重复连。
void AddJumpUpEdges(Graph& g) {
    for (int i = 0; i < g.n; ++i) {
        if (IsRopeNode(g, i) || IsWallFh(g, i) || SpanX(g, i) < 12) continue;
        int slo = 0, shi = 0;
        if (!SafeStandXRange(g, i, &slo, &shi, /*avoidWalkJunction=*/false, /*endInset=*/2)) continue;
        if (shi - slo < 8) continue;
        struct Cand {
            int j = -1;
            int rise = 0;
            int x = 0;
        };
        Cand cands[kJumpUpMaxPerNode]{};
        int nCand = 0;
        for (int j = 0; j < g.n; ++j) {
            if (j == i || IsRopeNode(g, j) || IsWallFh(g, j) || SpanX(g, j) < 16) continue;
            const int jmin = (std::min)(g.x1[j], g.x2[j]);
            const int jmax = (std::max)(g.x1[j], g.x2[j]);
            const int olo = (std::max)(slo, jmin + kJumpUpEdgePadPx);
            const int ohi = (std::min)(shi, jmax - kJumpUpEdgePadPx);
            if (ohi - olo < kJumpUpMinOverlapPx) continue;
            // 上台是斜坡时高差随 X 变：在重叠带里挑高差最小的点起跳，且人站偏 ±10px（导航容差）处也得
            // 够得着（离线 sim 103000000 fh192→fh220：中点算 72 刚好，人站偏 7px 处 78，三跳全落回）。
            auto riseAt = [&](int xx) {
                return FhYAtX(g.x1[j], g.y1[j], g.x2[j], g.y2[j], xx) - FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], xx);
            };
            int x = (olo + ohi) / 2;
            int rise = riseAt(x);
            for (int xx = olo; xx <= ohi; xx += 4) {
                const int r = riseAt(xx);
                if (r < rise) { rise = r; x = xx; }
            }
            if (rise < kJumpUpMinRisePx || rise > kJumpUpMaxRisePx) continue;
            {
                const int lo10 = (std::max)(olo, x - 10), hi10 = (std::min)(ohi, x + 10);
                if (riseAt(lo10) > kJumpUpMaxRisePx || riseAt(hi10) > kJumpUpMaxRisePx) continue;
            }
            bool walkLinked = false;
            for (int ei = 0; ei < g.deg[i] && !walkLinked; ++ei)
                walkLinked = g.adj[i][ei].kind == EdgeKind::Walk && g.adj[i][ei].to == j;
            if (walkLinked) continue;
            // 上台必须是起跳 X（±10）上、顶点以下**最高**的那块台：台面从下往上可穿，人升到 +80 后下落，
            // 被顶点以下最高的台接住——比 j 更高又在顶点以下的台会抢先接住（离线 sim 103000002 楼梯：
            // fh10→fh11 只高 21，人落到了 42px 上的 fh12）。夹在 i、j 之间的台无所谓（穿过去）。
            bool blocked = false;
            for (int k = 0; k < g.n && !blocked; ++k) {
                if (k == i || k == j || IsRopeNode(g, k) || IsWallFh(g, k)) continue;
                const int kmin = (std::min)(g.x1[k], g.x2[k]);
                const int kmax = (std::max)(g.x1[k], g.x2[k]);
                for (int xx = x - 10; xx <= x + 10 && !blocked; xx += 10) {
                    if (xx < kmin || xx > kmax) continue;
                    const int yk = FhYAtX(g.x1[k], g.y1[k], g.x2[k], g.y2[k], xx);
                    const int ykRel = yk - FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], xx);
                    blocked = ykRel > rise + 2 && ykRel <= kJumpApexPx - 2;
                }
            }
            if (blocked) continue;
            // 有序小表（rise 升序），满了挤掉最高的。
            int pos = nCand;
            while (pos > 0 && cands[pos - 1].rise > rise) --pos;
            if (pos >= kJumpUpMaxPerNode) continue;
            const int last = nCand < kJumpUpMaxPerNode ? nCand : kJumpUpMaxPerNode - 1;
            for (int k = last; k > pos; --k) cands[k] = cands[k - 1];
            cands[pos] = Cand{j, rise, x};
            if (nCand < kJumpUpMaxPerNode) ++nCand;
        }
        for (int c = 0; c < nCand; ++c) {
            const int yi = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], cands[c].x);
            AddEdge(g, i, cands[c].j, EdgeKind::JumpUp, -1, cands[c].x, yi, cands[c].x);
        }
    }
}

// 绳到绳的起跳 Y：飞到目标绳 X 窗（±10）时人必须**已过顶点**且 Y 在目标绳段内（上下留 20）。
// 横速不确定（140~170），两个端值都要可行；返回可行区间的中点（最抗误差），没有 → false。
// 小跳顶点只有 ≈21px、0.16s：dx=100 时到窗要 0.6~0.7s，人已比起跳点低 150~230px，
// 所以接力绳通常要从**接近顶端**起跳、落到下一根的中下段——这正是这类图的设计。
bool RopeToRopeLaunchY(int rTop, int rBot, int rTopMargin, int sTop, int sBot, int dx, int* outY0,
                       int* outYAtS) {
    int lo = rBot + kRopeEndMarginPx;
    int hi = rTop - rTopMargin;
    if (hi < lo) return false;
    const float bandMid = 0.5f * static_cast<float>(sBot + sTop);
    float prefer = 0.f;
    const float vxs[2] = {kRopeHopVxLoPxPerSec, kRopeHopVxHiPxPerSec};
    for (float vx : vxs) {
        float tIn = static_cast<float>(dx - kRopeGrabHalfWinPx) / vx;
        const float tOut = static_cast<float>(dx + kRopeGrabHalfWinPx) / vx;
        if (tIn < kRopeHopApexSec) tIn = kRopeHopApexSec;  // 上升段抓不住：最早在顶点
        if (tOut <= tIn) return false;
        const float hiArc = RopeHopHeightAt(tIn);   // 窗内最高点（刚进窗 / 顶点）
        const float loArc = RopeHopHeightAt(tOut);  // 窗内最低点（出窗）
        // 需要 [y0+loArc, y0+hiArc] 与 [sBot+m, sTop-m] 有交集：
        //   y0 + hiArc >= sBot + m  且  y0 + loArc <= sTop - m
        const int y0Lo = static_cast<int>(std::ceil(static_cast<float>(sBot + kRopeToRopeGrabMarginPx) - hiArc));
        const int y0Hi = static_cast<int>(std::floor(static_cast<float>(sTop - kRopeToRopeGrabMarginPx) - loArc));
        if (y0Lo > lo) lo = y0Lo;
        if (y0Hi < hi) hi = y0Hi;
        if (hi < lo) return false;
        prefer += 0.5f * (bandMid - 0.5f * (loArc + hiArc));  // 让窗内扫过的 Y 段正好压在绳段中间
    }
    // 理想值多半高于绳顶（人在窗里已下落很多），钳到顶端 = 尽量从高处跳，多留掉到绳底以下的余量。
    int y0 = static_cast<int>(std::lround(prefer));
    if (y0 > hi) y0 = hi;
    if (y0 < lo) y0 = lo;
    if (outY0) *outY0 = y0;
    if (outYAtS) {
        // 名义横速 150 下到窗中心时的 Y（日志 / 校核用）。
        float t = static_cast<float>(dx) / kRopeJumpAirVxPxPerSec;
        if (t < kRopeHopApexSec) t = kRopeHopApexSec;
        *outYAtS = y0 + static_cast<int>(std::lround(RopeHopHeightAt(t)));
    }
    return true;
}

void AddRopeJumpEdges(Graph& g) {
    for (int ri = 0; ri < gLadderN; ++ri) {
        const auto& R = gLadders[ri];
        const int rTop = (std::max)(R.y1, R.y2);
        const int rBot = (std::min)(R.y1, R.y2);
        if (rTop - rBot < 2 * kRopeEndMarginPx + 10) continue;
        const int topMargin = RopeTopLaunchMargin(R);
        // 出发点：底台 / 顶台 / 两侧的绳侧台（先上绳再爬到起跳 Y），以及绳节点本身（人已经挂在
        // 这根绳上——接力跳上来的、或中途丢了计划的）。
        const int fromNodes[5] = {gRopeDn[ri], gRopeUp[ri], g.ropeNode[ri], gRopeSide[ri][0], gRopeSide[ri][1]};
        constexpr int kFromN = 5;
        if (fromNodes[0] < 0 && fromNodes[1] < 0 && fromNodes[2] < 0) continue;

        // ① 绳侧台：不盖住绳 X、离绳 6~100px。绳上是小跳（vy0=270），从落点上方 30px 起跳往下落到台上。
        for (int j = 0; j < g.n; ++j) {
            if (IsWallFh(g, j) || SpanX(g, j) < 16) continue;
            if (j == gRopeUp[ri] || j == gRopeDn[ri]) continue;
            const int jmin = (std::min)(g.x1[j], g.x2[j]);
            const int jmax = (std::max)(g.x1[j], g.x2[j]);
            if (R.x >= jmin - kRopeJumpMinGapPx && R.x <= jmax + kRopeJumpMinGapPx) continue;
            const int d = (jmin > R.x) ? 1 : -1;
            const int nearX = NearEndX(g, j, d);
            const int gap = d * (nearX - R.x);
            if (gap < kRopeJumpMinGapPx || gap > kRopeJumpMaxGapPx) continue;
            const int yLand = YAtClamped(g, j, nearX);
            int y0 = yLand + kRopeJumpAboveLandPx;
            if (y0 > rTop - topMargin) y0 = rTop - topMargin;
            if (y0 < rBot + kRopeEndMarginPx) y0 = rBot + kRopeEndMarginPx;
            const int h = yLand - y0;
            // 落点比绳底还低很多的不算绳侧跳：从绳底起跳「落」3000px 到楼下的台在几何上也「够得着」
            //（离线 sim 101000000 rope x=428 → fh52 y=-290，h=-3272 照样连边），实际中途被别的台接住 →
            // 每次 rope_jump_miss。深落点交给落地后的 FallDown 边。
            if (h < -kRopeJumpMaxDropPx) continue;
            const float t = RopeHopLandTime(static_cast<float>(h));
            if (t <= 0.f) continue;  // 落点比小跳顶点还高
            const float reach = kRopeJumpAirVxPxPerSec * t * kJumpReachFactor;
            if (static_cast<float>(gap + kAcrossLandMarginPx) > reach) continue;
            if (JumpPathBlocked(g, -1, j, R.x, nearX, y0, kRopeJumpAirVxPxPerSec * kJumpReachFactor,
                                kRopeHopV0PxPerSec))
                continue;
            const int inset = (std::min)(30, SpanX(g, j) / 2);
            const int aimX = nearX + d * inset;
            for (int f = 0; f < kFromN; ++f) {
                if (fromNodes[f] < 0 || fromNodes[f] == j) continue;
                AddEdge(g, fromNodes[f], j, EdgeKind::RopeJump, ri, R.x, y0, aimX);
            }
        }

        // ② 绳到绳：目标是**绳节点**（抓住即到；再往顶爬 / 再跳下一根是绳节点自己的边）。
        //    目标绳有没有台都能连——uf=0 的接力绳正是没顶台的那种。横向 12~130px，
        //    起跳 Y 按下落段可行域算（RopeToRopeLaunchY），算不出就是物理上跳不到。
        for (int si = 0; si < gLadderN; ++si) {
            if (si == ri) continue;
            const int sNode = g.ropeNode[si];
            if (sNode < 0) continue;
            const auto& S = gLadders[si];
            const int sTop = (std::max)(S.y1, S.y2);
            const int sBot = (std::min)(S.y1, S.y2);
            const int dx = std::abs(S.x - R.x);
            if (dx < kRopeToRopeMinDx || dx > kRopeToRopeMaxDx) continue;
            int y0 = 0, yAtS = 0;
            if (!RopeToRopeLaunchY(rTop, rBot, topMargin, sTop, sBot, dx, &y0, &yAtS)) continue;
            if (JumpPathBlocked(g, -1, -1, R.x, S.x, y0, kRopeJumpAirVxPxPerSec, kRopeHopV0PxPerSec)) continue;
            for (int f = 0; f < kFromN; ++f) {
                if (fromNodes[f] < 0 || fromNodes[f] == sNode) continue;
                AddEdge(g, fromNodes[f], sNode, EdgeKind::RopeJump, ri, R.x, y0, S.x, si);
            }
        }
    }
}

// 接缝 Walk 边（见 kSeamEdgeRopeIdx）。每段两端各找相接同高的台，最多各连一块（取最近）。
void AddSeamWalkEdges(Graph& g) {
    for (int i = 0; i < g.n; ++i) {
        if (IsRopeNode(g, i) || IsVerticalWall(g, i) || SpanX(g, i) < 8) continue;
        const int imin = (std::min)(g.x1[i], g.x2[i]);
        const int imax = (std::max)(g.x1[i], g.x2[i]);
        const int ends[2] = {imin, imax};
        const int dirs[2] = {-1, 1};
        for (int e = 0; e < 2; ++e) {
            const int endX = ends[e];
            const int d = dirs[e];
            const int yEnd = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], endX);
            int best = -1, bestGap = 1 << 30;
            for (int j = 0; j < g.n; ++j) {
                if (j == i || IsRopeNode(g, j) || IsVerticalWall(g, j) || SpanX(g, j) < 8) continue;
                const int jmin = (std::min)(g.x1[j], g.x2[j]);
                const int jmax = (std::max)(g.x1[j], g.x2[j]);
                const int nearJ = d > 0 ? jmin : jmax;
                const int gap = std::abs(nearJ - endX);
                if (gap > kSeamTouchPx) continue;
                if (std::abs(FhYAtX(g.x1[j], g.y1[j], g.x2[j], g.y2[j], nearJ) - yEnd) > kSeamDyPx) continue;
                bool already = false;
                for (int ei = 0; ei < g.deg[i] && !already; ++ei)
                    already = g.adj[i][ei].kind == EdgeKind::Walk && g.adj[i][ei].to == j;
                if (already) continue;
                if (gap < bestGap) {
                    bestGap = gap;
                    best = j;
                }
            }
            if (best >= 0) {
                int wx, wy;
                Mid(g, best, wx, wy);
                AddEdge(g, i, best, EdgeKind::Walk, kSeamEdgeRopeIdx, wx, wy);
            }
        }
    }
}

void LabelWalkComps(Graph& g) {
    std::memset(g.walkComp, 0, sizeof(g.walkComp));
    uint16_t q[foothold::kMaxFootholds];
    uint16_t label = 0;
    for (int s = 0; s < g.n; ++s) {
        if (g.walkComp[s] != 0) continue;
        ++label;
        int qh = 0;
        int qt = 0;
        q[qt++] = static_cast<uint16_t>(s);
        g.walkComp[s] = label;
        while (qh < qt) {
            const int u = q[qh++];
            for (int ei = 0; ei < g.deg[u]; ++ei) {
                if (g.adj[u][ei].kind != EdgeKind::Walk) continue;
                const int v = g.adj[u][ei].to;
                if (g.walkComp[v] != 0) continue;
                g.walkComp[v] = label;
                q[qt++] = static_cast<uint16_t>(v);
            }
        }
    }
}

void BuildUnlocked(const foothold::Snapshot& snap) {
    Graph* g = EnsureGraphObj();
    // 禁止 `*g = Graph{}`：那会在**栈上**构造一个 ~1.1MB 的临时 Graph（adj 2048×24×20B），
    // 战斗 worker 默认 1MB 栈直接 STACK_OVERFLOW（离线单测复现 0xC00000FD）。就地清零即可：
    // 每条真实加入的边都由 AddEdge 显式写全字段，未用槽位不会被读。
    std::memset(g, 0, sizeof(Graph));
    g->mapId = snap.mapId;
    for (int i = 0; i < foothold::kMaxFootholds; ++i) g->nodeRope[i] = -1;
    for (int i = 0; i < foothold::kMaxLadders; ++i) g->ropeNode[i] = -1;
    gLadderN = 0;
    for (int i = 0; i < snap.ladderN && i < foothold::kMaxLadders; ++i) {
        gLadders[gLadderN++] = snap.ladders[i];
    }

    for (int i = 0; i < snap.footholdN && g->n < foothold::kMaxFootholds; ++i) {
        const auto& f = snap.footholds[i];
        if (f.id == 0) continue;
        const int idx = g->n++;
        g->ids[idx] = f.id;
        g->x1[idx] = f.x1;
        g->y1[idx] = f.y1;
        g->x2[idx] = f.x2;
        g->y2[idx] = f.y2;
        g->forbidFall[idx] = f.forbidFall ? 1 : 0;
        g->zMass[idx] = f.zMass;
    }

    // Prev / Next walk —— 仅同 zMass 才连通。
    // BIN b71cfd map=101030102：fh112(z=44) prev=fh96(z=10)，异 z 伪链会把 60px 短台
    // 算进长链「内部安全点」→ fill 落到 542 后穿落回 410（land_miss 死循环）。
    // 竖直段（x1==x2，台阶立面 / 崖壁）站不上去，也走不过去：
    //   · 高 ≤ kWallStepMaxPx：是台阶，Walk 链**穿过**它接到对面的台（拟人到端点跳上去）；
    //   · 更高：是墙，链在这里**断开**——两侧不再算同一层，得走绳 / 下跳绕。
    // 拟人 hop 以「踩到目标 FH」为到站，把立面当路点永远到不了
    //（BIN 14:25 fh35→fh15 顶住 → 跳过该边 → 整层 no_path；17:16 怪在墙上层却被当同层贴墙硬走）。
    // 窄而不竖的小台（SpanX 3~15）照常连：绳顶常是十几像素的小台，孤立掉会让人站上去后
    // 整图 no_path（BIN 17:13 fromComp=34 一秒 385 次）。
    auto liteById = [&](uint32_t id) -> const foothold::FootholdLite* {
        for (int k = 0; k < snap.footholdN; ++k)
            if (snap.footholds[k].id == id) return &snap.footholds[k];
        return nullptr;
    };
    auto isVertical = [&](int idx) -> bool { return SpanX(*g, idx) <= kVerticalSpanPx; };
    auto wallRise = [&](int idx) -> int { return std::abs(g->y1[idx] - g->y2[idx]); };
    auto throughWalls = [&](uint32_t startId, uint32_t cameFrom) -> int {
        uint32_t id = startId;
        uint32_t prevId = cameFrom;
        for (int depth = 0; depth < 4 && id; ++depth) {
            const int idx = IndexOf(*g, id);
            if (idx < 0) return -1;
            if (!isVertical(idx)) return idx;
            if (wallRise(idx) > kWallStepMaxPx) return -1;  // 真墙：链断
            const foothold::FootholdLite* w = liteById(id);
            if (!w) return -1;
            const uint32_t nextId = (w->prev == prevId) ? w->next : w->prev;
            if (nextId == prevId || nextId == id) return -1;
            prevId = id;
            id = nextId;
        }
        return -1;
    };
    for (int i = 0; i < snap.footholdN; ++i) {
        const auto& f = snap.footholds[i];
        const int from = IndexOf(*g, f.id);
        if (from < 0 || isVertical(from)) continue;
        if (f.prev) {
            const int to = throughWalls(f.prev, f.id);
            if (to >= 0 && to != from && g->zMass[from] == g->zMass[to]) {
                int wx, wy;
                Mid(*g, to, wx, wy);
                AddEdge(*g, from, to, EdgeKind::Walk, -1, wx, wy);
            }
        }
        if (f.next) {
            const int to = throughWalls(f.next, f.id);
            if (to >= 0 && to != from && g->zMass[from] == g->zMass[to]) {
                int wx, wy;
                Mid(*g, to, wx, wy);
                AddEdge(*g, from, to, EdgeKind::Walk, -1, wx, wy);
            }
        }
    }
    // 相接同高但没 prev/next（异组）的台之间补接缝 Walk 边（先于绳/崖判定：段端有它就不是崖）。
    AddSeamWalkEdges(*g);

    // Rope / ladder：AbsPos 更大 Y = 更高。顶 = max Y，底 = min Y。
    // 挂台先认 **X 真盖住绳子** 的段（±4），没有才放宽到 48px 邻段。
    // 绳穿地板洞 / 贴段缝时（BIN 12:55 wx=381 在 fh12 上却挂到 fh2；wx=1241 在 fh17 左沿却挂到 fh1），
    // 挂错段会让拟人把「站绳顶按 ↓」做成「从邻段助跑跳」，在两段之间来回跳。
    for (int ri = 0; ri < gLadderN; ++ri) {
        const auto& lr = gLadders[ri];
        const int yTop = (std::max)(lr.y1, lr.y2);
        const int yBot = (std::min)(lr.y1, lr.y2);
        int up = BestFhNear(*g, lr.x, yTop, kRopeCoverXTol, kRopeYTol, /*skipWall=*/true);
        if (up < 0) up = BestFhNear(*g, lr.x, yTop, kRopeXTol, kRopeYTol);
        // WZ uf=0：绳顶不接台，爬到顶也上不去，只能侧跳——顶上哪怕挨着一块台也不当出口
        //（map101040000 x=54/151/252… 15 根接力绳全是 uf=0）。
        if (!lr.isUpperFh) up = -1;
        int dn = BestFhAtOrBelow(*g, lr.x, yBot, kRopeCoverXTol, kRopeYTol);
        if (dn < 0) dn = BestFhAtOrBelow(*g, lr.x, yBot, kRopeXTol, kRopeYTol);
        if (dn < 0) dn = BestFhAtOrBelow(*g, lr.x, yBot, kRopeCoverXTol, kRopeJumpGrabDy);
        if (dn < 0) dn = BestFhAtOrBelow(*g, lr.x, yBot, kRopeXTol, kRopeJumpGrabDy);
        // 靷 48px 容差挂上的底台不盖住绳 X = 实际是台外绳、要助跑跳抓：台面从近沿往回得站得下起跳点
        //（48−gap+8），不然只能刹到中间速度——MS 走速二值，刹不出来（102030000 fh120 17px 台、绳在沿外 12px：
        // 走/停交替 9s 到超时）。这种底台不挂。
        if (dn >= 0 && !CoversXExact(*g, dn, lr.x)) {
            const int dmin = (std::min)(g->x1[dn], g->x2[dn]), dmax = (std::max)(g->x1[dn], g->x2[dn]);
            const int gap = lr.x < dmin ? dmin - lr.x : lr.x - dmax;
            if (gap <= 48 && SpanX(*g, dn) < 48 - gap + 8) dn = -1;
        }
        // 记下每根绳挂到的顶/底台（没挂上 = -1）：绳侧跳 / 绳到绳要用。
        // 顶台只要认得出就记，即便底台没挂上（够不着的高绳正是绳到绳的目标）。
        gRopeUp[ri] = up;
        gRopeDn[ri] = (dn >= 0 && dn != up) ? dn : -1;
        // 绳底正下方可能叠着不止一块台（101030103 绳 x=138：贴底 28px 的斜坡碎片 fh44 + 59px 下的地板 fh43）。
        // 最近的那块是 dn（下绳落点）；其余盖住绳 X、在 kRopeYTol 内的台也各给一条 ClimbUp 边——只连最近块，
        // 站在地板上的人就没边可走，只好绕到邻段当「台外绳」助跑，走上地板又被判换台，退/冲死循环。
        int dnAll[8];
        int dnN = 0;
        if (gRopeDn[ri] >= 0) dnAll[dnN++] = gRopeDn[ri];
        for (int k = 0; k < g->n && dnN < 8; ++k) {
            if (k == up || k == dn || IsRopeNode(*g, k) || IsVerticalWall(*g, k)) continue;
            if (!CoversXExact(*g, k, lr.x)) continue;
            const int fy = FhYAtX(g->x1[k], g->y1[k], g->x2[k], g->y2[k], lr.x);
            if (fy > yBot + 12 || yBot - fy > kRopeYTol) continue;
            dnAll[dnN++] = k;
        }
        FindRopeSidePlatforms(*g, ri, up, dnAll, dnN);
        // 绳侧台 → 顶台：从侧台朝绳助跑跳抓，再爬到顶（导航按台外绳口径走）。
        if (up >= 0)
            for (int s = 0; s < 2; ++s)
                if (gRopeSide[ri][s] >= 0 && gRopeSide[ri][s] != up)
                    AddEdge(*g, gRopeSide[ri][s], up, EdgeKind::ClimbUp, ri, lr.x, yBot);
        if (up < 0 || dn < 0 || up == dn) continue;
        ++g->ropeLinked;
        AddEdge(*g, up, dn, EdgeKind::ClimbDown, ri, lr.x, yTop);
        for (int i = 0; i < dnN; ++i)
            if (dnAll[i] != up) AddEdge(*g, dnAll[i], up, EdgeKind::ClimbUp, ri, lr.x, yBot);
    }

    // 绳节点：每根绳一个（x1==x2 的竖段，几何查询把它当墙跳过）。「挂在这根绳上」是一个可规划的
    // 位置：从它出发能爬到顶台 / 底台出去（下面两条边），或再侧跳到台 / 下一根绳（AddRopeJumpEdges）。
    // 绳到绳的边一律落在绳节点上，uf=0 / 两头都没台的接力绳因此也进得来、出得去。
    for (int ri = 0; ri < gLadderN && g->n < foothold::kMaxFootholds; ++ri) {
        const auto& lr = gLadders[ri];
        const int yTop = (std::max)(lr.y1, lr.y2);
        const int yBot = (std::min)(lr.y1, lr.y2);
        if (yTop - yBot < 2 * kRopeEndMarginPx + 10) continue;
        const int idx = g->n++;
        g->ids[idx] = kRopeNodeIdBase + static_cast<uint32_t>(ri);
        g->x1[idx] = lr.x;
        g->x2[idx] = lr.x;
        g->y1[idx] = yBot;
        g->y2[idx] = yTop;
        g->forbidFall[idx] = 1;
        g->zMass[idx] = -1 - ri;  // 独一份：别和任何台算同 zMass
        g->nodeRope[idx] = static_cast<int16_t>(ri);
        g->ropeNode[ri] = static_cast<int16_t>(idx);
        ++g->ropeNodes;
        if (gRopeUp[ri] >= 0) AddEdge(*g, idx, gRopeUp[ri], EdgeKind::ClimbUp, ri, lr.x, yBot);
        if (gRopeDn[ri] >= 0) AddEdge(*g, idx, gRopeDn[ri], EdgeKind::ClimbDown, ri, lr.x, yTop);
    }

    // uf=0 天空绳：主循环在 up<0 时 continue，底台到不了绳节点。补 dn→node 的 ClimbUp
    //（拟人打怪仍要能爬这些绳；赶路进绳顶碰撞盒改走旋翼 overlap，不靠这条边）。
    for (int ri = 0; ri < gLadderN; ++ri) {
        const int node = g->ropeNode[ri];
        if (node < 0 || gRopeUp[ri] >= 0 || gRopeDn[ri] < 0) continue;
        const auto& lr = gLadders[ri];
        const int yBot = (std::min)(lr.y1, lr.y2);
        AddEdge(*g, gRopeDn[ri], node, EdgeKind::ClimbUp, ri, lr.x, yBot);
        for (int k = 0; k < g->n; ++k) {
            if (k == node || k == gRopeDn[ri] || IsRopeNode(*g, k) || IsVerticalWall(*g, k)) continue;
            if (!CoversXExact(*g, k, lr.x)) continue;
            const int fy = FhYAtX(g->x1[k], g->y1[k], g->x2[k], g->y2[k], lr.x);
            if (fy > yBot + 12 || yBot - fy > kRopeYTol) continue;
            AddEdge(*g, k, node, EdgeKind::ClimbUp, ri, lr.x, yBot);
        }
    }

    AddFallDownEdges(*g);
    AddJumpAcrossEdges(*g);
    AddJumpUpEdges(*g);
    AddRopeJumpEdges(*g);
    LabelWalkComps(*g);

    gHopSkipN = 0;
    std::memset(gHopSkip, 0, sizeof(gHopSkip));
    gDeadEdgeN = 0;
    std::memset(gDeadEdge, 0, sizeof(gDeadEdge));

    g->ok = g->n > 0;

    // 上次在这张图学到的死边直接带进来（bin\state\navmem\map_<id>.txt），别每天重新在高绳下面跳 3 次。
    // 只认两端仍在图里的；FH id 是地图数据固定的，换版本地图改了就对不上、自然丢弃。
    if (g->ok && snap.mapId) {
        nav_memory::MapMemory mem{};
        if (nav_memory::Load(snap.mapId, &mem) && mem.deadN > 0) {
            int kept = 0;
            for (int i = 0; i < mem.deadN && gDeadEdgeN < kDeadEdgeCap; ++i) {
                const auto& d = mem.dead[i];
                if (IndexOf(*g, d.from) < 0 || IndexOf(*g, d.to) < 0) continue;
                if (DeadEdgeUnlocked(d.from, d.to, static_cast<EdgeKind>(d.kind))) continue;
                gDeadEdge[gDeadEdgeN++] = DeadEdge{d.from, d.to, d.kind};
                ++kept;
            }
            if (kept > 0)
                x::runtime::LogI("Foothold", "graph map=%d restored %d dead edge(s) from navmem", snap.mapId,
                                 kept);
        }
    }
    EnsureEtaScaleLoaded();
}

bool RebuildFromCacheUnlocked() {
    foothold::Snapshot* heap = new foothold::Snapshot{};
    const bool ok = foothold::GetCached(*heap) && heap->ok;
    if (ok) BuildUnlocked(*heap);
    delete heap;
    return ok && gGraph && gGraph->ok;
}

bool GraphMatchesCacheUnlocked() {
    if (!gGraph || !gGraph->ok) return false;
    foothold::SnapshotMeta meta{};
    if (!foothold::GetCachedMeta(&meta) || !meta.ok) return false;
    return meta.mapId == gGraph->mapId;
}

}  // namespace

bool EnsureGraph() {
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (GraphMatchesCacheUnlocked()) return true;
    }

    foothold::SnapshotMeta meta{};
    if (!foothold::GetCachedMeta(&meta) || !meta.ok) {
        if (!foothold::CollectToCache(&meta) || !meta.ok) return false;
    }

    std::lock_guard<std::mutex> lock(gMu);
    if (GraphMatchesCacheUnlocked()) return true;
    const bool ok = RebuildFromCacheUnlocked();
    if (ok && gGraph && gGraph->ok) {
        int noTop = 0;
        for (int ri = 0; ri < gLadderN; ++ri)
            if (!gLadders[ri].isUpperFh) ++noTop;
        int degFull = 0;  // 邻接表满员的节点：满了之后加的边（JumpUp / RopeJump 排最后）静默丢
        for (int i = 0; i < gGraph->n; ++i)
            if (gGraph->deg[i] >= kMaxDeg) ++degFull;
        x::runtime::LogI("Foothold",
                         "graph lazy-build map=%d n=%d walk=%d climb=%d fall=%d rope=%d jump=%d "
                         "ropeNodes=%d uf0=%d degFull=%d",
                         gGraph->mapId, gGraph->n - gGraph->ropeNodes, gGraph->walkEdges,
                         gGraph->climbEdges, gGraph->fallEdges, gGraph->ropeLinked, gGraph->jumpEdges,
                         gGraph->ropeNodes, noTop, degFull);
    }
    return ok;
}

bool FindNearestFh(float x, float y, uint32_t* outId, float* outDist) {
    if (outId) *outId = 0;
    if (outDist) *outDist = 1e9f;
    if (!outId) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(x);
    const int iy = static_cast<int>(y);

    int bestCover = -1;
    float bestCoverD = 1e9f;
    int bestAny = -1;
    float bestAnyD = 1e9f;

    for (int i = 0; i < g.n; ++i) {
        if (IsRopeNode(g, i)) continue;
        const int xa = g.x1[i], xb = g.x2[i];
        const int xmin = (std::min)(xa, xb);
        const int xmax = (std::max)(xa, xb);
        int mx, my;
        Mid(g, i, mx, my);
        const float dAny =
            std::sqrt(static_cast<float>((mx - ix) * (mx - ix) + (my - iy) * (my - iy)));
        if (dAny < bestAnyD) {
            bestAnyD = dAny;
            bestAny = i;
        }

        const bool cover = (ix >= xmin - 8 && ix <= xmax + 8);
        if (!cover) continue;
        const int fy = FhYAtX(xa, g.y1[i], xb, g.y2[i], ix);
        const int dy = std::abs(fy - iy);
        if (dy > kCoverYTol) continue;
        const float d = static_cast<float>(dy) + 0.01f * std::abs(ix - mx);
        if (d < bestCoverD) {
            bestCoverD = d;
            bestCover = i;
        }
    }

    const int pick = (bestCover >= 0) ? bestCover : bestAny;
    if (pick < 0) return false;
    *outId = g.ids[pick];
    if (outDist) *outDist = (bestCover >= 0) ? bestCoverD : bestAnyD;
    return true;
}

bool FindNearestStand(float x, float y, float* outX, float* outY, uint32_t* outFhId, float* outDist,
                      bool avoidWalkJunction, bool cliffInset) {
    if (outX) *outX = x;
    if (outY) *outY = y;
    if (outFhId) *outFhId = 0;
    if (outDist) *outDist = 1e9f;
    if (!outX || !outY) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    const int endInset = cliffInset ? kEndInsetCliff : kEndInsetFly;

    auto consider = [&](bool allowNarrow, int* bestIdx, int* bestCx, int* bestFy,
                        float* bestD) {
        for (int i = 0; i < g.n; ++i) {
            if (IsWallFh(g, i)) continue;
            if (!allowNarrow && ChainTooNarrowToStand(g, i, endInset)) continue;
            int lo = 0, hi = 0;
            if (!SafeStandXRange(g, i, &lo, &hi, avoidWalkJunction, endInset)) continue;
            const int cx = ClampToSafeStandX(g, i, ix, avoidWalkJunction, endInset);
            const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], cx);
            const float dx = static_cast<float>(cx - ix);
            const float dy = static_cast<float>(fy - iy);
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d < *bestD) {
                *bestD = d;
                *bestIdx = i;
                *bestCx = cx;
                *bestFy = fy;
            }
        }
    };

    int bestIdx = -1, bestCx = ix, bestFy = iy;
    float bestD = 1e9f;
    consider(/*allowNarrow=*/false, &bestIdx, &bestCx, &bestFy, &bestD);
    if (bestIdx < 0)
        consider(/*allowNarrow=*/true, &bestIdx, &bestCx, &bestFy, &bestD);
    if (bestIdx < 0) return false;

    *outX = static_cast<float>(bestCx);
    *outY = static_cast<float>(bestFy);
    if (outFhId) *outFhId = g.ids[bestIdx];
    if (outDist) *outDist = bestD;
    return true;
}

bool SnapStandAt(float x, float y, float* outX, float* outY, uint32_t* outFhId, bool preferFlat,
                 bool avoidWalkJunction, bool cliffInset) {
    if (outX) *outX = x;
    if (outY) *outY = y;
    if (outFhId) *outFhId = 0;
    if (!outX || !outY) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    const int endInset = cliffInset ? kEndInsetCliff : kEndInsetFly;
    constexpr int kXPad = 10;
    // 同层带宽：优先 |fy-y| 落在此内，避免 BIN「怪 Y=-215 却落到 -155」窜层。
    constexpr int kStandYBand = 45;  // 与 simple_combat kSameLayerY 对齐
    // 同 X 叠台：在此带内取 |dy| 最小的那层，同 dy 取更高的（AbsPos 更大 Y = 更高）。
    // 旧口径「优先 min fy」写于屏幕坐标时代（BIN 7b792b：贴下层 526 人站 470），换成 AbsPos 后
    // min fy 就成了「优先最下层」：目标点正落在 fh73(y=265) 上，却贴到 60px 下面的 fh58 →
    // 规划成 hops=0 朝下层走，永远到不了（离线 sim map30000 fh58→fh73 90s 不动）。
    constexpr int kStandStackBand = 72;
    // 平台：|y1-y2|≤此值视为平（赶路贴门优先，斜面易滑出触发框）。
    constexpr int kFlatYTol = 3;

    // 命中档：cover 要求该点被段的 X 区间覆盖（台真在人脚下）；band 只保证同高、any 只保证
    // 「全图最近」，二者与该点的邻近性无关，属退化兜底。preferFlat 据此判断平台趟是否算数。
    enum class Tier { kNone, kCover, kBand, kAny };

    auto runPick = [&](bool flatOnly, bool allowNarrow, Tier* outTier) -> int {
        int bestSameYCover = -1;
        int bestSameYCoverDy = 0x7fffffff;
        int bestSameYCoverFy = 0x7fffffff;
        int bestYBand = -1;
        float bestYBandScore = 1e9f;
        int bestLooseCover = -1;
        int bestLooseCoverDy = 0x7fffffff;
        int bestStack = -1;
        int bestStackFy = -0x7fffffff;
        int bestStackDy = 0x7fffffff;
        int bestAny = -1;
        float bestAnyD = 1e9f;

        for (int i = 0; i < g.n; ++i) {
            if (IsWallFh(g, i)) continue;
            if (!allowNarrow && ChainTooNarrowToStand(g, i, endInset)) continue;
            if (flatOnly && std::abs(g.y1[i] - g.y2[i]) > kFlatYTol) continue;
            const int xa = g.x1[i], xb = g.x2[i];
            const int xmin = (std::min)(xa, xb);
            const int xmax = (std::max)(xa, xb);
            const int cx = (std::max)(xmin, (std::min)(xmax, ix));
            const int fy = FhYAtX(xa, g.y1[i], xb, g.y2[i], cx);
            const int dy = std::abs(fy - iy);

            int mx, my;
            Mid(g, i, mx, my);
            const float dAny =
                std::sqrt(static_cast<float>((mx - ix) * (mx - ix) + (my - iy) * (my - iy)));
            if (dAny < bestAnyD) {
                bestAnyD = dAny;
                bestAny = i;
            }

            if (dy <= kStandYBand) {
                const float score =
                    static_cast<float>(std::abs(cx - ix)) + 0.25f * static_cast<float>(dy);
                if (score < bestYBandScore) {
                    bestYBandScore = score;
                    bestYBand = i;
                }
            }

            const bool cover = (ix >= xmin - kXPad && ix <= xmax + kXPad);
            if (!cover) continue;
            if (dy <= kStandStackBand && (dy < bestStackDy || (dy == bestStackDy && fy > bestStackFy))) {
                bestStackDy = dy;
                bestStackFy = fy;
                bestStack = i;
            }
            if (dy <= kStandYBand) {
                if (dy < bestSameYCoverDy ||
                    (dy == bestSameYCoverDy && fy < bestSameYCoverFy)) {
                    bestSameYCoverDy = dy;
                    bestSameYCoverFy = fy;
                    bestSameYCover = i;
                }
            }
            if (dy <= kCoverYTol && dy < bestLooseCoverDy) {
                bestLooseCoverDy = dy;
                bestLooseCover = i;
            }
        }

        int pick = -1;
        Tier tier = Tier::kNone;
        if (bestStack >= 0) {
            pick = bestStack;
            tier = Tier::kCover;
        } else if (bestSameYCover >= 0) {
            pick = bestSameYCover;
            tier = Tier::kCover;
        } else if (bestYBand >= 0) {
            pick = bestYBand;
            tier = Tier::kBand;
        } else if (bestLooseCover >= 0) {
            pick = bestLooseCover;
            tier = Tier::kCover;
        } else if (bestAny >= 0) {
            pick = bestAny;
            tier = Tier::kAny;
        }
        if (outTier) *outTier = tier;
        return pick;
    };

    // 排序原则：**是否覆盖该点**优先于平台/宽窄。band（同高但 X 隔很远）与 any（全图最近台）
    // 都与该点邻近性无关，只能当最后兜底——把它们当正常结果就会贴到几百 px 外的台上：
    //   BIN 25e8cc 怪在 y=419 贴到 y=-123（dY=-542）→ 同层门判死 → 沼澤地 noLand=100%；
    //   BIN 0ea69f 怪在 dx=-650 贴到脚下同高台 → 谎报 hop~0 → 对空开枪 + sticky_spin 空转。
    int pick = -1;
    int bandFallback = -1;
    int anyFallback = -1;
    auto tryPass = [&](bool flatOnly, bool allowNarrow) {
        if (pick >= 0) return;
        Tier tier = Tier::kNone;
        const int p = runPick(flatOnly, allowNarrow, &tier);
        if (p < 0) return;
        if (tier == Tier::kCover) {
            pick = p;
        } else if (tier == Tier::kBand) {
            if (bandFallback < 0) bandFallback = p;  // 宽链趟先跑，故宽台优先于短台
        } else if (tier == Tier::kAny) {
            if (anyFallback < 0) anyFallback = p;
        }
    };

    if (preferFlat) tryPass(/*flatOnly=*/true, /*allowNarrow=*/false);
    tryPass(/*flatOnly=*/false, /*allowNarrow=*/false);
    // 短链台（碎台面 / 水上小台）站着确实易滑，但「怪脚下的短台」永远胜过「同高的远台」。
    tryPass(/*flatOnly=*/false, /*allowNarrow=*/true);
    if (pick < 0) pick = (bandFallback >= 0) ? bandFallback : anyFallback;
    if (pick < 0) return false;

    const int xa = g.x1[pick], xb = g.x2[pick];
    const int xmin = (std::min)(xa, xb);
    const int xmax = (std::max)(xa, xb);
    // 一律以 ix 为期望 X：退化档下 ix 可能在段外，由 ClampToSafeStandX 收进段内安全区间。
    const int wishX = ix;
    const int cx = ClampToSafeStandX(g, pick, wishX, avoidWalkJunction, endInset);
    const int fy = FhYAtX(xa, g.y1[pick], xb, g.y2[pick], cx);
    *outX = static_cast<float>(cx);
    *outY = static_cast<float>(fy);
    if (outFhId) *outFhId = g.ids[pick];
    (void)xmin;
    (void)xmax;
    return true;
}

bool CensusStandAt(float x, float y, StandCensus* out) {
    if (!out) return false;
    *out = {};
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    constexpr int kBand = 45;  // 与 kStandYBand / simple_combat kSameLayerY 对齐

    out->nodes = g.n - g.ropeNodes;
    int bestIdx = -1;
    for (int i = 0; i < g.n; ++i) {
        if (IsRopeNode(g, i)) continue;
        const int xa = g.x1[i], xb = g.x2[i];
        const int xmin = (std::min)(xa, xb);
        const int xmax = (std::max)(xa, xb);
        const int cx = (std::max)(xmin, (std::min)(xmax, ix));
        const int fy = FhYAtX(xa, g.y1[i], xb, g.y2[i], cx);
        if (std::abs(fy - iy) > kBand) continue;
        ++out->inBand;
        const bool wall = IsWallFh(g, i);
        const bool narrow = !wall && ChainTooNarrowToStand(g, i);
        if (wall) ++out->wall;
        if (narrow) ++out->narrow;
        if (!wall && !narrow) ++out->usable;
        const int span = SpanX(g, i);
        if (span > out->bestSpan) {
            out->bestSpan = span;
            bestIdx = i;
        }
    }
    if (bestIdx >= 0) {
        int lo = 0, hi = 0;
        if (ChainSafeXRange(g, bestIdx, &lo, &hi)) {
            out->bestChainLo = lo;
            out->bestChainHi = hi;
        }
    }
    return true;
}

int ProbeColumn(float x, float y, int yWindow, ColumnHit* out, int maxOut, int* outTotal) {
    if (outTotal) *outTotal = 0;
    if (!out || maxOut <= 0) return -1;
    if (!EnsureGraph()) return -1;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return -1;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    const int win = (yWindow > 0) ? yWindow : 200;

    auto inWindow = [&](int i, int* outFy) -> bool {
        if (IsRopeNode(g, i)) return false;  // 绳不是台：探列 / 箱子 / 地板都不算它
        const int xa = g.x1[i], xb = g.x2[i];
        if (ix < (std::min)(xa, xb) || ix > (std::max)(xa, xb)) return false;
        const int fy = FhYAtX(xa, g.y1[i], xb, g.y2[i], ix);
        if (std::abs(fy - iy) > win) return false;
        if (outFy) *outFy = fy;
        return true;
    };

    int total = 0;
    for (int i = 0; i < g.n; ++i)
        if (inWindow(i, nullptr)) ++total;
    if (outTotal) *outTotal = total;

    // 选择法按 (y, fh) 升序取前 maxOut 条：n≈1e3、maxOut≈8，开销可忽略，免去大栈缓冲。
    int n = 0;
    int prevY = 0;
    uint32_t prevFh = 0;
    bool hasPrev = false;
    while (n < maxOut) {
        int best = -1, bestFy = 0;
        for (int i = 0; i < g.n; ++i) {
            int fy = 0;
            if (!inWindow(i, &fy)) continue;
            if (hasPrev && (fy < prevY || (fy == prevY && g.ids[i] <= prevFh))) continue;
            if (best >= 0 && (fy > bestFy || (fy == bestFy && g.ids[i] > g.ids[best]))) continue;
            best = i;
            bestFy = fy;
        }
        if (best < 0) break;
        ColumnHit& h = out[n++];
        h.fh = g.ids[best];
        h.y = bestFy;
        h.span = SpanX(g, best);
        h.slope = std::abs(g.y1[best] - g.y2[best]);
        h.wall = IsWallFh(g, best);
        h.narrow = !h.wall && ChainTooNarrowToStand(g, best);
        prevY = bestFy;
        prevFh = h.fh;
        hasPrev = true;
    }
    return n;
}

bool SnapStandForPortal(float x, float y, float rectL, float rectT, float rectR, float rectB,
                        bool rectValid, float* outX, float* outY, uint32_t* outFhId) {
    if (outX) *outX = x;
    if (outY) *outY = y;
    if (outFhId) *outFhId = 0;
    if (!outX || !outY) return false;
    // 发门带 = travel kPortalFireMaxDx。门心可以无 cover（top00 x=65 缝，x=72 可站）。
    // 带宽外的台（沼泽远岸）进不了这个集合。
    // 门常在悬崖边：禁止套战斗 kEndInsetCliff=36（BIN 19:27 105050400 east00
    // portal=2881 被内缩出发门带 → 假空集悬停 → 合速永远 >18）。
    constexpr int kFireBandDx = 16;
    constexpr int kSeedYBand = 72;
    constexpr int kFlatYTol = 3;
    constexpr int kPortalEdgePad = kEndInsetFly;
    // 沼泽门口台常被切成 SpanX<16 的短节（BIN 107000000 east00：wall=12 假空集
    // → keep-station 浮空）。战斗/下落仍把它们当墙；贴门只收近水平短台。
    // 竖线 / rise>run 仍是墙。短台 Y 只认门心同层（-81 vs -122 是远岸错层，禁）。
    constexpr int kShortDeckDy = 24;

    auto portalFhCanStand = [](const Graph& g, int i) -> bool {
        const int span = SpanX(g, i);
        if (span <= 0) return false;
        const int slope = std::abs(g.y1[i] - g.y2[i]);
        if (span < kFallMinSpanX && slope > span) return false;
        return true;
    };

    if (!EnsureGraph()) return false;

    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    int fireLo = ix - kFireBandDx;
    int fireHi = ix + kFireBandDx;
    if (rectValid && std::isfinite(rectL) && std::isfinite(rectR) && rectR >= rectL) {
        const int rLo = static_cast<int>(std::floor(rectL));
        const int rHi = static_cast<int>(std::ceil(rectR));
        fireLo = (std::max)(fireLo, rLo);
        fireHi = (std::min)(fireHi, rHi);
        if (fireLo > fireHi) {
            fireLo = ix - kFireBandDx;
            fireHi = ix + kFireBandDx;
        }
    }
    int yLo = iy - 80;
    int yHi = iy + 80;
    if (rectValid && std::isfinite(rectT) && std::isfinite(rectB) && rectB >= rectT) {
        yLo = static_cast<int>(std::floor(rectT));
        yHi = static_cast<int>(std::ceil(rectB));
    }

    auto emitPick = [&](const Graph& g, int pick, int wish, const char* via) -> bool {
        if (pick < 0) return false;
        if (std::abs(wish - ix) > kFireBandDx) return false;
        const int xa = g.x1[pick], xb = g.x2[pick];
        const int fy = FhYAtX(xa, g.y1[pick], xb, g.y2[pick], wish);
        *outX = static_cast<float>(wish);
        *outY = static_cast<float>(fy);
        if (outFhId) *outFhId = g.ids[pick];
        x::runtime::LogI("FhPath",
                         "portalSnap via=%s pickFh=%u wish=%d -> (%.0f,%.0f) "
                         "portal=(%d,%d) fireBand=1",
                         via ? via : "?", g.ids[pick], wish, *outX, *outY, ix, iy);
        return true;
    };

    // 本段原始 X（只留 2px 边）与发门带有交集。不用战斗悬崖/段缝内缩。
    auto pickInFireBand = [&](const Graph& g, bool flatOnly, int* outWish) -> int {
        int best = -1, bestDy = 0x7fffffff, bestSlope = 0x7fffffff, bestDx = 0x7fffffff;
        int bestSpan = -1;
        int bestWish = ix;
        bool bestInY = false;
        for (int i = 0; i < g.n; ++i) {
            if (!portalFhCanStand(g, i)) continue;
            if (flatOnly && std::abs(g.y1[i] - g.y2[i]) > kFlatYTol) continue;
            const int span = SpanX(g, i);
            const int xmin = (std::min)(g.x1[i], g.x2[i]);
            const int xmax = (std::max)(g.x1[i], g.x2[i]);
            int sLo = xmin + kPortalEdgePad;
            int sHi = xmax - kPortalEdgePad;
            if (sLo > sHi) {
                sLo = (xmin + xmax) / 2;
                sHi = sLo;
            }
            int oLo = (std::max)(sLo, fireLo);
            int oHi = (std::min)(sHi, fireHi);
            if (oLo > oHi) {
                if (ix < xmin || ix > xmax) continue;
                oLo = ix;
                oHi = ix;
            }
            const int wish = (std::max)(oLo, (std::min)(oHi, ix));
            const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], wish);
            const int dy = std::abs(fy - iy);
            if (dy > kSeedYBand) continue;
            if (span < kFallMinSpanX && dy > kShortDeckDy) continue;
            const int slope = std::abs(g.y1[i] - g.y2[i]);
            const int dx = std::abs(wish - ix);
            const bool inY = (fy >= yLo && fy <= yHi);
            const bool better = (best < 0) || (inY != bestInY && inY) ||
                                (inY == bestInY && dy < bestDy) ||
                                (inY == bestInY && dy == bestDy && slope < bestSlope) ||
                                (inY == bestInY && dy == bestDy && slope == bestSlope &&
                                 span > bestSpan) ||
                                (inY == bestInY && dy == bestDy && slope == bestSlope &&
                                 span == bestSpan && dx < bestDx);
            if (!better) continue;
            best = i;
            bestDy = dy;
            bestSlope = slope;
            bestSpan = span;
            bestDx = dx;
            bestWish = wish;
            bestInY = inY;
        }
        if (outWish) *outWish = bestWish;
        return best;
    };

    {
        std::lock_guard<std::mutex> lock(gMu);
        if (!gGraph || !gGraph->ok) return false;
        const Graph& g = *gGraph;
        int wish = ix;
        int pick = pickInFireBand(g, /*flatOnly=*/true, &wish);
        if (pick < 0) pick = pickInFireBand(g, /*flatOnly=*/false, &wish);
        const char* via = "none";
        if (pick >= 0) via = IsWallFh(g, pick) ? "fireBandShort" : "fireBand";
        if (emitPick(g, pick, wish, via)) return true;

        int rawX = 0, rawY = 0, wallN = 0, shortN = 0, vertN = 0;
        for (int i = 0; i < g.n; ++i) {
            const int xmin = (std::min)(g.x1[i], g.x2[i]);
            const int xmax = (std::max)(g.x1[i], g.x2[i]);
            if (xmax < fireLo || xmin > fireHi) continue;
            if (!portalFhCanStand(g, i)) {
                ++vertN;
                ++wallN;
                continue;
            }
            if (IsWallFh(g, i)) {
                ++shortN;
                ++wallN;
            } else {
                ++rawX;
            }
            const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i],
                                  (std::max)(xmin, (std::min)(xmax, ix)));
            if (std::abs(fy - iy) <= kSeedYBand) ++rawY;
        }
        x::runtime::LogI("FhPath",
                         "portalSnap miss portal=(%d,%d) fire-band empty "
                         "rawX=%d inY=%d wall=%d short=%d vert=%d "
                         "(no cliff-inset, no far-band)",
                         ix, iy, rawX, rawY, wallN, shortN, vertN);
    }
    return false;
}

bool SnapOnFh(uint32_t fhId, float x, float* outX, float* outY, bool avoidWalkJunction,
              bool cliffInset) {
    if (outX) *outX = x;
    if (outY) *outY = 0.f;
    if (!outX || !outY || fhId == 0) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int idx = IndexOf(g, fhId);
    if (idx < 0 || IsWallFh(g, idx)) return false;

    const int xa = g.x1[idx], xb = g.x2[idx];
    const int endInset = cliffInset ? kEndInsetCliff : kEndInsetFly;
    const int ix = static_cast<int>(std::lround(x));
    const int cx = ClampToSafeStandX(g, idx, ix, avoidWalkJunction, endInset);
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi, avoidWalkJunction, endInset)) return false;
    (void)lo;
    (void)hi;
    const int fy = FhYAtX(xa, g.y1[idx], xb, g.y2[idx], cx);
    *outX = static_cast<float>(cx);
    *outY = static_cast<float>(fy);
    return true;
}

bool IsXSafeOnFh(uint32_t fhId, float x, bool avoidWalkJunction, bool cliffInset) {
    if (fhId == 0 || !std::isfinite(x)) return false;
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int idx = IndexOf(g, fhId);
    if (idx < 0 || IsWallFh(g, idx)) return false;
    const int endInset = cliffInset ? kEndInsetCliff : kEndInsetFly;
    if (ChainTooNarrowToStand(g, idx, endInset)) return false;
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi, avoidWalkJunction, endInset)) return false;
    const int ix = static_cast<int>(std::lround(x));
    return ix >= lo && ix <= hi;
}

void AddHopSkip(uint32_t fromFh, uint32_t toFh, EdgeKind kind, uint32_t forMs) {
    if (!fromFh || !toFh) return;
    const DWORD until = GetTickCount() + (forMs ? forMs : kHopSkipMs);
    std::lock_guard<std::mutex> lock(gMu);
    PurgeHopSkipsUnlocked(GetTickCount());
    const uint8_t k = static_cast<uint8_t>(kind);
    for (int i = 0; i < gHopSkipN; ++i) {
        if (gHopSkip[i].from == fromFh && gHopSkip[i].to == toFh && gHopSkip[i].kind == k) {
            if (until > gHopSkip[i].until) gHopSkip[i].until = until;
            return;
        }
    }
    if (gHopSkipN >= kHopSkipCap) {
        int victim = 0;
        for (int i = 1; i < gHopSkipN; ++i) {
            if (gHopSkip[i].until < gHopSkip[victim].until) victim = i;
        }
        gHopSkip[victim] = HopSkip{fromFh, toFh, k, until};
        return;
    }
    gHopSkip[gHopSkipN++] = HopSkip{fromFh, toFh, k, until};
}

void ClearHopSkips() {
    std::lock_guard<std::mutex> lock(gMu);
    gHopSkipN = 0;
    std::memset(gHopSkip, 0, sizeof(gHopSkip));
    // gDeadEdge 不清：那是实机试出来的物理事实，赶路起步清 8s 跳过表时不该跟着丢。
}

void MarkEdgeDead(uint32_t fromFh, uint32_t toFh, EdgeKind kind) {
    if (!fromFh || !toFh) return;
    std::lock_guard<std::mutex> lock(gMu);
    if (DeadEdgeUnlocked(fromFh, toFh, kind)) return;
    const uint8_t k = static_cast<uint8_t>(kind);
    if (gDeadEdgeN >= kDeadEdgeCap) {
        // 满了就顶掉最早的一条；32 条对一张图绝对够，真满说明别处在乱标。
        for (int i = 1; i < gDeadEdgeN; ++i) gDeadEdge[i - 1] = gDeadEdge[i];
        gDeadEdgeN = kDeadEdgeCap - 1;
    }
    gDeadEdge[gDeadEdgeN++] = DeadEdge{fromFh, toFh, k};
    if (gGraph && gGraph->ok && gGraph->mapId) nav_memory::NoteDeadEdge(gGraph->mapId, fromFh, toFh, k);
}

int DeadEdgeCount() {
    std::lock_guard<std::mutex> lock(gMu);
    return gDeadEdgeN;
}

bool FhXRange(uint32_t fhId, int* xmin, int* xmax) {
    if (!xmin || !xmax || !fhId) return false;
    *xmin = 0;
    *xmax = 0;
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const int idx = IndexOf(*gGraph, fhId);
    if (idx < 0) return false;
    *xmin = (std::min)(gGraph->x1[idx], gGraph->x2[idx]);
    *xmax = (std::max)(gGraph->x1[idx], gGraph->x2[idx]);
    return true;
}

bool FhGeom(uint32_t fhId, FhGeomInfo* out) {
    if (!out || !fhId) return false;
    *out = FhGeomInfo{};
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int idx = IndexOf(g, fhId);
    if (idx < 0) return false;
    out->x1 = g.x1[idx];
    out->y1 = g.y1[idx];
    out->x2 = g.x2[idx];
    out->y2 = g.y2[idx];
    out->vertical = SpanX(g, idx) <= kVerticalSpanPx;
    out->wall = IsWallFh(g, idx);
    out->walkDeg = 0;
    for (int e = 0; e < g.deg[idx]; ++e)
        if (g.adj[idx][e].kind == EdgeKind::Walk) ++out->walkDeg;
    out->walkComp = g.walkComp[idx];
    return true;
}

bool FhYAt(uint32_t fhId, float x, float* outY) {
    if (!outY || !fhId) return false;
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int idx = IndexOf(g, fhId);
    if (idx < 0) return false;
    int ix = static_cast<int>(std::lround(x));
    const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
    const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
    if (ix < xmin) ix = xmin;
    if (ix > xmax) ix = xmax;
    *outY = static_cast<float>(FhYAtX(g.x1[idx], g.y1[idx], g.x2[idx], g.y2[idx], ix));
    return true;
}

bool ZMassOfFh(uint32_t fhId, int32_t* outZMass) {
    if (!outZMass || fhId == 0) return false;
    *outZMass = 0;
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const int idx = IndexOf(*gGraph, fhId);
    if (idx < 0) return false;
    *outZMass = gGraph->zMass[idx];
    return true;
}

bool ZMassAt(float x, float y, int32_t* outZMass, uint32_t* outFhId) {
    if (outFhId) *outFhId = 0;
    if (!outZMass) return false;
    *outZMass = 0;
    float sx = x, sy = y;
    uint32_t fh = 0;
    if (!SnapStandAt(x, y, &sx, &sy, &fh) || !fh) return false;
    if (outFhId) *outFhId = fh;
    return ZMassOfFh(fh, outZMass);
}

int WalkCompOf(uint32_t fh) {
    if (!fh) return 0;
    if (!EnsureGraph()) return 0;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return 0;
    const int i = IndexOf(*gGraph, fh);
    if (i < 0) return 0;
    return static_cast<int>(gGraph->walkComp[i]);
}

bool SameWalkComponent(uint32_t fhA, uint32_t fhB) {
    if (!fhA || !fhB) return false;
    if (fhA == fhB) return true;
    const int a = WalkCompOf(fhA);
    const int b = WalkCompOf(fhB);
    return a > 0 && a == b;
}

int MarkFallWalkReachable(uint32_t fromFh, int maxFallHops, uint8_t* outMask, int maxMask,
                          int* outReachCnt) {
    if (outReachCnt) *outReachCnt = 0;
    if (!outMask || maxMask <= 0) return 0;
    std::memset(outMask, 0, static_cast<size_t>(maxMask));
    if (!fromFh) return 0;
    if (maxFallHops < 0) maxFallHops = 0;
    if (maxFallHops > 4) maxFallHops = 4;
    if (!EnsureGraph()) return 0;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return 0;
    const Graph& g = *gGraph;
    const int n = (g.n < maxMask) ? g.n : maxMask;
    const int src = IndexOf(g, fromFh);
    if (src < 0 || src >= n) return 0;

    static uint8_t best[foothold::kMaxFootholds];
    static uint16_t qn[foothold::kMaxFootholds * 5];
    static uint8_t qf[foothold::kMaxFootholds * 5];
    std::memset(best, 0xFF, static_cast<size_t>(n));
    int qh = 0;
    int qt = 0;
    qn[qt] = static_cast<uint16_t>(src);
    qf[qt] = 0;
    ++qt;
    best[src] = 0;

    const int qCap = foothold::kMaxFootholds * 5;
    while (qh < qt) {
        const int u = qn[qh];
        const int f = qf[qh];
        ++qh;
        outMask[u] = 1;
        for (int ei = 0; ei < g.deg[u]; ++ei) {
            const EdgeKind kind = g.adj[u][ei].kind;
            const int v = g.adj[u][ei].to;
            if (v < 0 || v >= n) continue;
            int nf = -1;
            if (kind == EdgeKind::Walk) {
                nf = f;
            } else if (kind == EdgeKind::FallDown && f < maxFallHops) {
                nf = f + 1;
            }
            if (nf < 0) continue;
            if (nf >= static_cast<int>(best[v])) continue;
            if (qt >= qCap) break;
            best[v] = static_cast<uint8_t>(nf);
            qn[qt] = static_cast<uint16_t>(v);
            qf[qt] = static_cast<uint8_t>(nf);
            ++qt;
        }
        if (f >= maxFallHops) continue;
        for (int ri = 0; ri < g.revFallDeg[u]; ++ri) {
            const int v = g.revFall[u][ri];
            if (v < 0 || v >= n) continue;
            const int nf = f + 1;
            if (nf >= static_cast<int>(best[v])) continue;
            if (qt >= qCap) break;
            best[v] = static_cast<uint8_t>(nf);
            qn[qt] = static_cast<uint16_t>(v);
            qf[qt] = static_cast<uint8_t>(nf);
            ++qt;
        }
    }
    if (outReachCnt) {
        int c = 0;
        for (int i = 0; i < n; ++i) {
            if (outMask[i]) ++c;
        }
        *outReachCnt = c;
    }
    return n;
}

bool MaskHasFh(const uint8_t* mask, int n, uint32_t fh) {
    if (!mask || n <= 0 || !fh) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const int i = IndexOf(*gGraph, fh);
    if (i < 0 || i >= n) return false;
    return mask[i] != 0;
}

int HopSkipCount() {
    std::lock_guard<std::mutex> lock(gMu);
    PurgeHopSkipsUnlocked(GetTickCount());
    return gHopSkipN;
}

// ───────── 拟人耗时模型（规划与选怪 ETA 同一套，否则「选它因为近」和「怎么走过去」会打架）─────────
// 走 125px/s；上绳 ≈100px/s 外加走到绳下、对齐、抓、到顶走出约 2.2s；下绳不滑到底——低于顶台 60px
// 就方向键+跳脱离（human_nav kDismountBelowTopPx），余下按坠落算，开销 1.5s；下跳 / 走崖 0.9s 再加
// 坠落；每换一段 0.15s（台阶小跳 / 接缝）。另加「失手风险」：下跳偶尔穿不下去 / 极限跳偶尔摔回，
// 一次失手就是一轮重来，折成 0.6s / 0.8s 的期望成本——时间差不多时宁可走稳路，差得远就抄近路。
// 之前 PlanFirst 用固定权重（走 1 / 绳 4 / 下跳 11）：站在 240px 高台上要去正下方，宁可走到绳边
// 慢慢爬也不肯直接跳下去（BIN 2026-09-09 13:58 fh12→fh53 爬绳 6.5s，下跳 1.5s）。
constexpr int kEtaWalkPxPerSec = 125;
constexpr int kEtaClimbPxPerSec = 100;
constexpr int kEtaClimbOverheadMs = 2200;
constexpr int kEtaClimbDownOverheadMs = 1500;
constexpr int kEtaDismountBelowTopPx = 40;  // 与 human_nav kDismountBelowTopPx 同值
constexpr int kEtaFallOverheadMs = 900;
constexpr int kEtaFallPxPerSec = 550;
constexpr int kEtaHopOverheadMs = 150;
constexpr int kEtaFallRiskMs = 600;
constexpr int kEtaAcrossRiskMs = 800;

// 一条边的拟人耗时（ms）：先从 entryX 走到路点 wx，再做边本身的动作；*outArriveX = 做完后站的 X。
int EdgeEtaMsUnlocked(const Graph& g, int u, const Edge& e, int entryX, int* outArriveX) {
    auto yAt = [&](int idx, int x) {
        // 走崖下跳边的 wx 在段外：钳进段内再取 Y，别外推出一个假高差。
        const int lo = (std::min)(g.x1[idx], g.x2[idx]);
        const int hi = (std::max)(g.x1[idx], g.x2[idx]);
        const int cx = x < lo ? lo : (x > hi ? hi : x);
        return FhYAtX(g.x1[idx], g.y1[idx], g.x2[idx], g.y2[idx], cx);
    };
    const int v = e.to;
    const int walkPx = std::abs(e.wx - entryX);
    int ms = walkPx * 1000 / kEtaWalkPxPerSec + kEtaHopOverheadMs;
    int arrive = e.wx;
    if (e.kind == EdgeKind::ClimbUp || e.kind == EdgeKind::ClimbDown) {
        // 两台在绳 X 处的高差；台面取不到才退回绳长。
        int dy = (v >= 0 && v < g.n) ? std::abs(yAt(v, e.wx) - yAt(u, e.wx)) : 0;
        if (dy <= 0 && e.ropeIdx >= 0 && e.ropeIdx < gLadderN)
            dy = std::abs(gLadders[e.ropeIdx].y1 - gLadders[e.ropeIdx].y2);
        if (e.kind == EdgeKind::ClimbUp) {
            ms += dy * 1000 / kEtaClimbPxPerSec + kEtaClimbOverheadMs;
        } else {
            const int climbPx = dy < kEtaDismountBelowTopPx ? dy : kEtaDismountBelowTopPx;
            const int fallPx = dy - climbPx;
            ms += climbPx * 1000 / kEtaClimbPxPerSec + fallPx * 1000 / kEtaFallPxPerSec +
                  kEtaClimbDownOverheadMs;
        }
    } else if (e.kind == EdgeKind::FallDown) {
        const int dy = (v >= 0 && v < g.n) ? std::abs(yAt(u, e.wx) - yAt(v, e.wx)) : 0;
        ms += dy * 1000 / kEtaFallPxPerSec + kEtaFallOverheadMs + kEtaFallRiskMs;
    } else if (e.kind == EdgeKind::JumpAcross) {
        ms += 1200 + kEtaAcrossRiskMs;  // 助跑起跳到落地
        arrive = e.aimX;
    } else if (e.kind == EdgeKind::JumpUp) {
        ms += 900 + kEtaAcrossRiskMs / 2;  // 站住、起跳、落到上台
        arrive = e.aimX;
    } else if (e.kind == EdgeKind::RopeJump) {
        // 走到绳下、抓、爬到起跳 Y、侧跳。从绳节点出发（人已在绳上）不用走也不用抓：
        // yAt 给的是绳中点，按中点到起跳 Y 的爬行算。抓另一根绳 = 落在它的绳节点上，
        // 再往顶爬 / 再跳是那个节点自己的边，这里只算抓住后稳一拍。
        const bool onRope = IsRopeNode(g, u);
        const int climbDy = std::abs(e.wy - yAt(u, e.wx));
        ms += climbDy * 1000 / kEtaClimbPxPerSec + (onRope ? 0 : kEtaClimbOverheadMs) + 1000 +
              kEtaAcrossRiskMs;
        if (e.ropeIdx2 >= 0 && e.ropeIdx2 < gLadderN) ms += 400;
        arrive = e.aimX;
    }
    if (outArriveX) *outArriveX = arrive;
    // 实测校准：走到路点那段按走速算得准，只对边本身的动作部分乘比例。
    const int walkMs = walkPx * 1000 / kEtaWalkPxPerSec;
    const float sc = EtaScaleOf(e.kind);
    if (sc != 1.f && ms > walkMs) ms = walkMs + static_cast<int>(static_cast<float>(ms - walkMs) * sc);
    return ms;
}

// 找 from→to 这条边（同 kind；wx 给了就要对上，±2）。
const Edge* FindEdgeUnlocked(const Graph& g, int u, int v, EdgeKind kind, int32_t wx) {
    const Edge* best = nullptr;
    for (int ei = 0; ei < g.deg[u]; ++ei) {
        const Edge& e = g.adj[u][ei];
        if (e.to != v || e.kind != kind) continue;
        if (wx != kPlanNoX && std::abs(e.wx - wx) > 2) {
            if (!best) best = &e;  // 记一个同类边兜底
            continue;
        }
        return &e;
    }
    return best;
}

int EstimateEdgeMs(uint32_t fromFh, uint32_t toFh, EdgeKind kind, int32_t wx, int32_t entryX) {
    if (!fromFh || !toFh) return -1;
    if (!EnsureGraph()) return -1;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return -1;
    const Graph& g = *gGraph;
    const int u = IndexOf(g, fromFh);
    const int v = IndexOf(g, toFh);
    if (u < 0 || v < 0) return -1;
    const Edge* e = FindEdgeUnlocked(g, u, v, kind, wx);
    if (!e) return -1;
    int ex = entryX;
    if (ex == kPlanNoX) {
        int32_t mx = 0, my = 0;
        Mid(g, u, mx, my);
        ex = mx;
    }
    return EdgeEtaMsUnlocked(g, u, *e, ex, nullptr);
}

void NoteEdgeObserved(EdgeKind kind, int modelMs, int observedMs) {
    const int k = static_cast<int>(kind);
    if (k < 0 || k >= kEtaKindN) return;
    if (modelMs <= 0 || observedMs <= 0) return;
    std::lock_guard<std::mutex> lock(gMu);
    EnsureEtaScaleLoaded();
    // modelMs 是带当前比例的估计；还原成未校准模型再算这次的比例，否则会自我放大。
    const float sc = gEtaScale[k] > 0.f ? gEtaScale[k] : 1.f;
    const float unscaled = static_cast<float>(modelMs) / sc;
    if (unscaled < 100.f) return;
    float ratio = static_cast<float>(observedMs) / unscaled;
    if (ratio < 0.4f) ratio = 0.4f;
    if (ratio > 3.0f) ratio = 3.0f;
    const int n = gEtaSamples[k];
    const float alpha = n < 3 ? 0.5f : 0.15f;
    float next = n == 0 ? ratio : gEtaScale[k] * (1.f - alpha) + ratio * alpha;
    if (next < 0.5f) next = 0.5f;
    if (next > 2.5f) next = 2.5f;
    gEtaScale[k] = next;
    gEtaSamples[k] = n + 1;
    if (gEtaSamples[k] <= 3 || (gEtaSamples[k] % 20) == 0)
        x::runtime::LogI("Foothold", "eta calib kind=%d model=%dms(raw %.0f) observed=%dms ratio=%.2f scale=%.2f n=%d",
                         k, modelMs, unscaled, observedMs, ratio, next, gEtaSamples[k]);
    const DWORD now = GetTickCount();
    if (!gEtaSavedMs || now - gEtaSavedMs >= kEtaSaveGapMs) {
        gEtaSavedMs = now;
        nav_memory::SaveEtaScale(gEtaScale, gEtaSamples, kEtaKindN);
    }
}

bool PlanFirst(uint32_t fromFh, uint32_t toFh, FirstAction* out, bool ignoreSkips, int32_t fromX,
               int32_t toX) {
    if (out) *out = FirstAction{};
    if (!out) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int src = IndexOf(g, fromFh);
    const int dst = IndexOf(g, toFh);
    if (src < 0 || dst < 0) return false;

    if (src == dst) {
        out->ok = true;
        out->kind = EdgeKind::Walk;
        out->fromFh = fromFh;
        out->toFh = toFh;
        Mid(g, src, out->wx, out->wy);
        out->hops = 0;
        return true;
    }

    // 几乎同高才 hops=0 朝目标 X 走；楼梯每级常见 30~60px，80 会把最后几级当平地直冲立面
    //（BIN 12:01:47 no_move）。
    const bool sameWalk =
        g.walkComp[src] != 0 && g.walkComp[src] == g.walkComp[dst];
    const int ySrc = (g.y1[src] + g.y2[src]) / 2;
    const int yDst = (g.y1[dst] + g.y2[dst]) / 2;
    if (sameWalk && std::abs(ySrc - yDst) <= 24) {
        out->ok = true;
        out->kind = EdgeKind::Walk;
        out->fromFh = fromFh;
        out->toFh = toFh;
        Mid(g, dst, out->wx, out->wy);
        out->hops = 0;
        return true;
    }

    // 按拟人耗时找最快路（EdgeEtaMsUnlocked，与选怪 ETA 同模型）：走 / 爬 / 下跳 / 极限跳一律折成
    // 毫秒比，站在哪、目标在哪一起算——同一条绳从台的左端出发和右端出发不是一个价。
    static int32_t dist[foothold::kMaxFootholds];
    static int32_t arriveX[foothold::kMaxFootholds];
    static int16_t parent[foothold::kMaxFootholds];
    static int16_t parentEdge[foothold::kMaxFootholds];
    static uint8_t used[foothold::kMaxFootholds];
    constexpr int32_t kInf = 0x3fffffff;
    const int n = g.n;
    for (int i = 0; i < n; ++i) {
        dist[i] = kInf;
        arriveX[i] = 0;
        parent[i] = -1;
        parentEdge[i] = -1;
        used[i] = 0;
    }
    dist[src] = 0;
    if (fromX != kPlanNoX) {
        arriveX[src] = fromX;
    } else {
        int32_t mx = 0, my = 0;
        Mid(g, src, mx, my);
        arriveX[src] = mx;
    }
    const DWORD nowTick = GetTickCount();

    for (int it = 0; it < n; ++it) {
        int u = -1;
        int32_t best = kInf;
        for (int i = 0; i < n; ++i) {
            if (used[i] || dist[i] >= best) continue;
            best = dist[i];
            u = i;
        }
        if (u < 0 || best == kInf) break;
        used[u] = 1;
        if (u == dst) break;
        const int entryX = arriveX[u];
        for (int ei = 0; ei < g.deg[u]; ++ei) {
            const Edge& e = g.adj[u][ei];
            const int v = static_cast<int>(e.to);
            if (v < 0 || v >= n) continue;
            // 物理死边（跳不到的绳）连兜底规划也不走；8s 软跳过才受 ignoreSkips 管。
            if (DeadEdgeUnlocked(g.ids[u], g.ids[v], e.kind)) continue;
            if (!ignoreSkips && HopSkippedUnlocked(g.ids[u], g.ids[v], e.kind, nowTick)) continue;
            int arrive = e.wx;
            int ms = EdgeEtaMsUnlocked(g, u, e, entryX, &arrive);
            // 最后一段：落到目标台后还得走到目标 X，落点离目标远的路线要算上这段。
            if (v == dst && toX != kPlanNoX) ms += std::abs(arrive - toX) * 1000 / kEtaWalkPxPerSec;
            const int64_t nd = static_cast<int64_t>(dist[u]) + ms;
            if (nd >= kInf) continue;
            if (static_cast<int32_t>(nd) < dist[v]) {
                dist[v] = static_cast<int32_t>(nd);
                arriveX[v] = arrive;
                parent[v] = static_cast<int16_t>(u);
                parentEdge[v] = static_cast<int16_t>(ei);
            }
        }
    }
    if (dist[dst] >= kInf) return false;

    // unwind to first hop from src
    int cur = dst;
    int hops = 0;
    int firstChild = -1;
    int firstEdge = -1;
    while (parent[cur] >= 0) {
        ++hops;
        if (parent[cur] == src) {
            firstChild = cur;
            firstEdge = parentEdge[cur];
            break;
        }
        cur = parent[cur];
    }
    if (firstChild < 0 || firstEdge < 0) return false;

    const Edge& e = g.adj[src][firstEdge];
    out->ok = true;
    out->kind = e.kind;
    out->fromFh = fromFh;
    out->toFh = g.ids[firstChild];
    out->wx = e.wx;
    out->wy = e.wy;
    out->aimX = e.aimX;
    out->hops = hops;
    if (e.ropeIdx >= 0 && e.ropeIdx < gLadderN) {
        const auto& lr = gLadders[e.ropeIdx];
        out->ropeId = lr.id;
        out->ropeYBot = (std::min)(lr.y1, lr.y2);
        out->ropeYTop = (std::max)(lr.y1, lr.y2);
    }
    if (e.ropeIdx2 >= 0 && e.ropeIdx2 < gLadderN) out->ropeId2 = gLadders[e.ropeIdx2].id;
    return true;
}

WalkAhead ProbeWalkAhead(float px, float py, int dir, uint32_t curFh, int aheadPx) {
    if (dir != -1 && dir != 1) return WalkAhead::Floor;
    if (!curFh) return WalkAhead::Floor;
    if (aheadPx < 16) aheadPx = 16;
    if (aheadPx > 96) aheadPx = 96;
    if (!EnsureGraph()) return WalkAhead::Floor;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return WalkAhead::Floor;
    const Graph& g = *gGraph;
    const int src = IndexOf(g, curFh);
    if (src < 0) return WalkAhead::Floor;

    const int ix = static_cast<int>(std::lround(px));
    const int iy = static_cast<int>(std::lround(py));
    const int ax = ix + dir * aheadPx;
    constexpr int kFloorYTol = 48;
    // 一跳能上的高度：实测起跳 vy≈470、滞空≈600ms → 顶点≈73px；140 会对着上不去的台面反复跳。
    constexpr int kJumpY = 84;
    constexpr int kStepUpMin = 12;
    constexpr int kGapJumpPx = 14;

    auto coversX = [&](int idx, int x, int xPad) -> bool {
        const int xmin = (std::min)(g.x1[idx], g.x2[idx]) - xPad;
        const int xmax = (std::max)(g.x1[idx], g.x2[idx]) + xPad;
        return x >= xmin && x <= xmax;
    };
    // 只认走道上的箱子，不认悬崖立面（BIN 02:22 cur=72 沿台乱跳）。
    // 短而陡的斜坡段（矮坡）离人近的那一端与脚下同高：能直接走上去，不是箱子。
    auto crateAt = [&](int idx, int x, int y) -> bool {
        if (!coversX(idx, x, 6)) return false;
        const int span = SpanX(g, idx);
        const int ymin = (std::min)(g.y1[idx], g.y2[idx]);
        const int ymax = (std::max)(g.y1[idx], g.y2[idx]);
        const int rise = ymax - ymin;
        if (span < 10 || span > 72) return false;
        if (rise < 28 || rise > 110) return false;
        const int nearX = (std::abs(g.x1[idx] - ix) <= std::abs(g.x2[idx] - ix)) ? g.x1[idx] : g.x2[idx];
        const int yNear = FhYAtX(g.x1[idx], g.y1[idx], g.x2[idx], g.y2[idx], nearX);
        if (std::abs(yNear - y) <= 16) return false;  // 坡脚接地：斜坡
        return std::abs(ymin - y) <= 36 && y <= ymax + 8;
    };
    auto floorAt = [&](int x, int y, int yTol) -> bool {
        for (int i = 0; i < g.n; ++i) {
            if (IsWallFh(g, i) || i == src) continue;
            if (!coversX(i, x, 4)) continue;
            const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], x);
            if (std::abs(fy - y) <= yTol) return true;
        }
        return false;
    };

    // 箱子：沿朝向采几列，别等撞上才跳。
    for (int t = 16; t <= aheadPx; t += 12) {
        const int x = ix + dir * t;
        for (int i = 0; i < g.n; ++i) {
            if (i == src || IsRopeNode(g, i)) continue;
            if (crateAt(i, x, iy)) return WalkAhead::Jump;
        }
    }

    const int xmin = (std::min)(g.x1[src], g.x2[src]);
    const int xmax = (std::max)(g.x1[src], g.x2[src]);
    if (ax >= xmin - 2 && ax <= xmax + 2) return WalkAhead::Floor;

    // 当前段盖不住 ahead：看 Prev/Next 邻台是无缝还是缺口/台阶。
    // 禁止用整条 Walk 链 X 范围当「能走」——图论把缝也连上，BIN 会在 fh 尽头空走。
    const int endX = (dir > 0) ? xmax : xmin;
    const int yEnd = FhYAtX(g.x1[src], g.y1[src], g.x2[src], g.y2[src], endX);
    // 段端立着高出台面的墙：既不是坑也跳不过（墙顶比台面高 ≥12 的当 Floor 让人顶着停——PlanDrop 据此
    // 不会把它当崖走出去；≤72 的矮墙后面若有台由 floorAt 判 Jump）。
    if (WallRisesAt(g, endX, yEnd, src)) {
        int wallTop = yEnd;
        for (int k = 0; k < g.n; ++k) {
            if (k == src || IsRopeNode(g, k) || !IsWallFh(g, k) || std::abs(g.x1[k] - g.x2[k]) > 2) continue;
            if (std::abs(g.x1[k] - endX) > 2) continue;
            wallTop = (std::max)(wallTop, (std::max)(g.y1[k], g.y2[k]));
        }
        if (wallTop - yEnd > kJumpY) return WalkAhead::Floor;
        return floorAt(ax, wallTop, kJumpY) ? WalkAhead::Jump : WalkAhead::Floor;
    }
    bool sawNeighbor = false;
    for (int e = 0; e < g.deg[src]; ++e) {
        if (g.adj[src][e].kind != EdgeKind::Walk) continue;
        const int v = static_cast<int>(g.adj[src][e].to);
        if (v < 0 || v >= g.n || IsVerticalWall(g, v)) continue;  // 15px 的接缝段是地面，不是墙
        const int nmin = (std::min)(g.x1[v], g.x2[v]);
        const int nmax = (std::max)(g.x1[v], g.x2[v]);
        const int gap = (dir > 0) ? (nmin - xmax) : (xmin - nmax);
        if (gap < -48) continue;  // 明显在身后
        sawNeighbor = true;
        if (gap > kGapJumpPx) return WalkAhead::Jump;
        const int nx = (dir > 0) ? nmin : nmax;
        const int yN = FhYAtX(g.x1[v], g.y1[v], g.x2[v], g.y2[v], nx);
        if (yN > yEnd + kStepUpMin && yN <= yEnd + kJumpY) return WalkAhead::Jump;
        if (coversX(v, ax, 4) && std::abs(yN - yEnd) <= kFloorYTol) return WalkAhead::Floor;
        // 邻段太短盖不到 ahead（台面常切成 23px 小段）：顺着 Walk 链再往前接几段，一路同高无缝就是地面。
        // 只看一段会在 ahead 落到第三段上时误判成 Jump（离线 sim 101030100：离台尾 42px 空跳进虚空）。
        int u = v;
        int prevU = src;
        int chainY = FhYAtX(g.x1[v], g.y1[v], g.x2[v], g.y2[v], (dir > 0) ? nmax : nmin);
        for (int hop = 0; hop < 4; ++hop) {
            const int umin = (std::min)(g.x1[u], g.x2[u]);
            const int umax = (std::max)(g.x1[u], g.x2[u]);
            int w = -1;
            for (int e2 = 0; e2 < g.deg[u]; ++e2) {
                if (g.adj[u][e2].kind != EdgeKind::Walk) continue;
                const int cand = static_cast<int>(g.adj[u][e2].to);
                if (cand == prevU || cand == src || cand < 0 || cand >= g.n || IsVerticalWall(g, cand)) continue;
                const int cmin = (std::min)(g.x1[cand], g.x2[cand]);
                const int cmax = (std::max)(g.x1[cand], g.x2[cand]);
                const int g2 = (dir > 0) ? (cmin - umax) : (umin - cmax);
                if (g2 < -8 || g2 > kGapJumpPx) continue;
                w = cand;
                break;
            }
            if (w < 0) break;
            const int wmin = (std::min)(g.x1[w], g.x2[w]);
            const int wmax = (std::max)(g.x1[w], g.x2[w]);
            const int yIn = FhYAtX(g.x1[w], g.y1[w], g.x2[w], g.y2[w], (dir > 0) ? wmin : wmax);
            if (std::abs(yIn - chainY) > kFloorYTol) break;  // 台阶 / 坡：交给下面的判定
            if (coversX(w, ax, 4)) {
                const int yA = FhYAtX(g.x1[w], g.y1[w], g.x2[w], g.y2[w], ax);
                if (std::abs(yA - yEnd) <= kFloorYTol) return WalkAhead::Floor;
                break;
            }
            chainY = FhYAtX(g.x1[w], g.y1[w], g.x2[w], g.y2[w], (dir > 0) ? wmax : wmin);
            prevU = u;
            u = w;
        }
    }

    if (floorAt(ax, iy, kFloorYTol)) return WalkAhead::Jump;
    if (floorAt(ax, iy + kJumpY / 2, kJumpY) || floorAt(ax, iy + kJumpY, kFloorYTol))
        return WalkAhead::Jump;
    if (sawNeighbor) return WalkAhead::Floor;
    return WalkAhead::Pit;
}

bool ClimbGrabHint(uint32_t fromFh, int32_t ropeX, int32_t ropeYBot, int32_t* outWalkX,
                   bool* outNeedJump) {
    if (outWalkX) *outWalkX = ropeX;
    if (outNeedJump) *outNeedJump = false;
    if (!fromFh) return false;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const int idx = IndexOf(*gGraph, fromFh);
    if (idx < 0) return false;
    const Graph& g = *gGraph;
    const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
    const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
    int walkX = ropeX;
    if (walkX < xmin + 2) walkX = xmin + 2;
    if (walkX > xmax - 2) walkX = xmax - 2;
    if (walkX > xmax) walkX = xmax;
    if (walkX < xmin) walkX = xmin;
    const bool pastEdge = ropeX < xmin - 6 || ropeX > xmax + 6;
    const int yStand = FhYAtX(g.x1[idx], g.y1[idx], g.x2[idx], g.y2[idx], walkX);
    // 绳底比脚下高 4px 以上就当悬空梯竖直跳抓（顶点后抓，4~78px 都稳）；站着按 ↑ 只留给绳底贴着 /
    // 低于台面的梯。旧阈 12 对绳底高 5~12px 的梯站着干等 1.5s×3 → grab_fail（离线 sim 101030200 fh65→43）。
    const bool hang = ropeYBot > yStand + 4;
    if (outWalkX) *outWalkX = walkX;
    if (outNeedJump) *outNeedJump = hang || pastEdge;
    return true;
}

int MarkAllPathCost(uint32_t fromFh, int16_t* outCost, int maxN) {
    if (!outCost || maxN <= 0) return 0;
    if (!EnsureGraph()) return 0;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return 0;
    const Graph& g = *gGraph;
    const int n = (g.n < maxN) ? g.n : maxN;
    for (int i = 0; i < n; ++i) outCost[i] = -1;
    const int src = IndexOf(g, fromFh);
    if (src < 0 || src >= n) return n;

    static int16_t dist[foothold::kMaxFootholds];
    static uint8_t used[foothold::kMaxFootholds];
    constexpr int16_t kInf = 0x7fff;
    for (int i = 0; i < n; ++i) {
        dist[i] = kInf;
        used[i] = 0;
    }
    dist[src] = 0;
    const DWORD nowTick = GetTickCount();
    for (int it = 0; it < n; ++it) {
        int u = -1;
        int16_t best = kInf;
        for (int i = 0; i < n; ++i) {
            if (used[i] || dist[i] >= best) continue;
            best = dist[i];
            u = i;
        }
        if (u < 0 || best == kInf) break;
        used[u] = 1;
        for (int ei = 0; ei < g.deg[u]; ++ei) {
            const Edge& e = g.adj[u][ei];
            const int v = e.to;
            if (v < 0 || v >= n) continue;
            if (HopSkippedUnlocked(g.ids[u], g.ids[v], e.kind, nowTick)) continue;
            const int nd = static_cast<int>(dist[u]) + EdgeWeight(e.kind);
            if (nd >= kInf) continue;
            if (static_cast<int16_t>(nd) < dist[v]) dist[v] = static_cast<int16_t>(nd);
        }
    }
    for (int i = 0; i < n; ++i) {
        if (dist[i] < kInf) outCost[i] = dist[i];
    }
    return n;
}

bool PathCostOfFh(uint32_t fh, const int16_t* cost, int n, int* outCost) {
    if (outCost) *outCost = -1;
    if (!outCost || !cost || n <= 0 || !fh) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const int idx = IndexOf(*gGraph, fh);
    if (idx < 0 || idx >= n) return false;
    if (cost[idx] < 0) return false;
    *outCost = cost[idx];
    return true;
}

int MarkAllPathTime(uint32_t fromFh, float fromX, int32_t* outMs, int32_t* outArriveX, int maxN) {
    if (!outMs || !outArriveX || maxN <= 0) return 0;
    if (!EnsureGraph()) return 0;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return 0;
    const Graph& g = *gGraph;
    const int n = (g.n < maxN) ? g.n : maxN;
    for (int i = 0; i < n; ++i) {
        outMs[i] = -1;
        outArriveX[i] = 0;
    }
    const int src = IndexOf(g, fromFh);
    if (src < 0 || src >= n) return n;

    static int32_t dist[foothold::kMaxFootholds];
    static int32_t arriveX[foothold::kMaxFootholds];
    static uint8_t used[foothold::kMaxFootholds];
    constexpr int32_t kInf = 0x3fffffff;
    for (int i = 0; i < n; ++i) {
        dist[i] = kInf;
        arriveX[i] = 0;
        used[i] = 0;
    }
    dist[src] = 0;
    arriveX[src] = static_cast<int32_t>(std::lround(fromX));
    const DWORD nowTick = GetTickCount();
    for (int it = 0; it < n; ++it) {
        int u = -1;
        int32_t best = kInf;
        for (int i = 0; i < n; ++i) {
            if (used[i] || dist[i] >= best) continue;
            best = dist[i];
            u = i;
        }
        if (u < 0 || best == kInf) break;
        used[u] = 1;
        const int entryX = arriveX[u];
        for (int ei = 0; ei < g.deg[u]; ++ei) {
            const Edge& e = g.adj[u][ei];
            const int v = e.to;
            if (v < 0 || v >= n) continue;
            if (HopSkippedUnlocked(g.ids[u], g.ids[v], e.kind, nowTick)) continue;
            int arrive = e.wx;
            const int ms = EdgeEtaMsUnlocked(g, u, e, entryX, &arrive);
            const int64_t nd = static_cast<int64_t>(dist[u]) + ms;
            if (nd >= kInf) continue;
            if (static_cast<int32_t>(nd) < dist[v]) {
                dist[v] = static_cast<int32_t>(nd);
                arriveX[v] = arrive;
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        if (dist[i] < kInf) {
            outMs[i] = dist[i];
            outArriveX[i] = arriveX[i];
        }
    }
    return n;
}

bool PathTimeOfFh(uint32_t fh, const int32_t* ms, const int32_t* arriveX, int n, int* outMs,
                  int* outArriveX) {
    if (outMs) *outMs = -1;
    if (outArriveX) *outArriveX = 0;
    if (!outMs || !ms || !arriveX || n <= 0 || !fh) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const int idx = IndexOf(*gGraph, fh);
    if (idx < 0 || idx >= n) return false;
    if (ms[idx] < 0) return false;
    *outMs = ms[idx];
    if (outArriveX) *outArriveX = arriveX[idx];
    return true;
}

namespace {
// 构图时的 ClimbUp 边：from=底台 to=顶台。没挂上 → upFh/dnFh 留 0。
void FillRopeInfoUnlocked(const Graph& g, int ri, RopeInfo* out) {
    const auto& lr = gLadders[ri];
    out->x = lr.x;
    out->yTop = (std::max)(lr.y1, lr.y2);
    out->yBot = (std::min)(lr.y1, lr.y2);
    out->isLadder = lr.isLadder;
    out->upperFh = lr.isUpperFh;
    out->nodeId = (ri < foothold::kMaxLadders && g.ropeNode[ri] >= 0) ? g.ids[g.ropeNode[ri]] : 0;
    // 顶台 / 底台直接取构图记下的挂台（绳侧台也有 ClimbUp 边，按边扫会把侧台当底台）。
    // 只认得出一头的绳也把那头给出去：人从别的绳跳上来后若中途丢了计划，ResumeOnRope 还能
    // 顺着往那头爬，而不是当「不认识的绳」跳下去。
    out->upFh = (ri < foothold::kMaxLadders && gRopeUp[ri] >= 0 && gRopeUp[ri] < g.n) ? g.ids[gRopeUp[ri]] : 0;
    out->dnFh = (ri < foothold::kMaxLadders && gRopeDn[ri] >= 0 && gRopeDn[ri] < g.n) ? g.ids[gRopeDn[ri]] : 0;
}
}  // namespace

bool RopeNearDropPoint(float x, float y) {
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    return RopeNearForDrop(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
}

uint32_t FirstFhBelow(float x, float y) {
    if (!EnsureGraph()) return 0;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return 0;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    int best = -1;
    int bestFy = -0x7fffffff;
    for (int i = 0; i < g.n; ++i) {
        // 短水平碎片也接得住人（只排真竖墙 / 绳节点）
        if (IsRopeNode(g, i) || IsVerticalWall(g, i)) continue;
        if (!CoversXExact(g, i, ix)) continue;
        const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], ix);
        if (iy - fy < kFallMinDropPx) continue;
        if (fy > bestFy) {
            bestFy = fy;
            best = i;
        }
    }
    return best >= 0 ? g.ids[best] : 0u;
}

bool FindRopeAt(float x, float y, int xTol, RopeInfo* out) {
    if (!out) return false;
    *out = RopeInfo{};
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    int best = -1;
    int bestDx = 0x7fffffff;
    for (int ri = 0; ri < gLadderN; ++ri) {
        const auto& lr = gLadders[ri];
        const int dx = std::abs(lr.x - ix);
        if (dx > xTol) continue;
        const int yTop = (std::max)(lr.y1, lr.y2);
        const int yBot = (std::min)(lr.y1, lr.y2);
        if (iy < yBot - 24 || iy > yTop + 24) continue;
        if (dx < bestDx) {
            bestDx = dx;
            best = ri;
        }
    }
    if (best < 0) return false;
    FillRopeInfoUnlocked(g, best, out);
    return true;
}

bool FindNearestRope(float x, float y, RopeInfo* out) {
    if (!out) return false;
    *out = RopeInfo{};
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    int best = -1;
    float bestD = 1e30f;
    for (int ri = 0; ri < gLadderN; ++ri) {
        RopeInfo info{};
        FillRopeInfoUnlocked(g, ri, &info);
        if (!info.upFh || !info.dnFh) continue;  // 没挂上台的绳爬不了
        if (info.yTop - info.yBot < 60) continue;  // 太短挂不住
        const float dx = std::fabs(static_cast<float>(info.x) - x);
        float cy = y;
        if (cy < static_cast<float>(info.yBot)) cy = static_cast<float>(info.yBot);
        if (cy > static_cast<float>(info.yTop)) cy = static_cast<float>(info.yTop);
        const float dy = std::fabs(cy - y);
        const float d = dx + dy * 0.6f;
        if (d < bestD) {
            bestD = d;
            best = ri;
        }
    }
    if (best < 0) return false;
    FillRopeInfoUnlocked(g, best, out);
    return true;
}

bool GetGraphMeta(GraphMeta* out) {
    if (!out) return false;
    *out = GraphMeta{};
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    out->ok = true;
    out->mapId = gGraph->mapId;
    out->nodes = gGraph->n - gGraph->ropeNodes;
    out->walkEdges = gGraph->walkEdges;
    out->climbEdges = gGraph->climbEdges;
    out->fallEdges = gGraph->fallEdges;
    out->ropeLinked = gGraph->ropeLinked;
    out->jumpEdges = gGraph->jumpEdges;
    out->ropeNodes = gGraph->ropeNodes;
    return true;
}

}  // namespace x::features::ports::foothold_path
