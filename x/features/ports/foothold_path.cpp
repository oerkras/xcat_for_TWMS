// Classic TWMS — foothold adjacency + BFS first hop.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "foothold_path.h"

#include "foothold_port.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace x::features::ports::foothold_path {
namespace {

constexpr int kMaxDeg = 16;
constexpr int kRopeXTol = 48;
constexpr int kRopeYTol = 60;
constexpr int kCoverYTol = 80;
constexpr int kFallMinDy = 45;    // 目标必须在下方（MS +Y 向下）
constexpr int kFallMaxDy = 720;
constexpr int kFallMinSpanX = 16; // 过窄视为墙，不做下跳起点/落点
constexpr int kFallXTol = 12;
// 站立落点：FH 是 Prev/Next 连成的线段链，不是孤立板。
// 内缩只作用在**整条 Walk 链的真正端点**（悬崖）；段与段接合处不内缩。
constexpr int kEndInsetCliff = 36;  // 链条左右端点（无更外侧 Walk）
// BIN d1a58e / 0.1.69：战斗落在段缝（fh5 右端=fh6 左端）→ RelPos.V=nan → Walk 滑链滑出图。
// 仅战斗 Snap 开启：有 Walk 邻台的端点再缩一点；贴门 / 赶路 SnapOnFh 仍只做悬崖内缩。
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

void AddEdge(Graph& g, int from, int to, EdgeKind kind, int ropeIdx, int wx, int wy) {
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
    if (kind == EdgeKind::Walk)
        ++g.walkEdges;
    else if (kind == EdgeKind::FallDown)
        ++g.fallEdges;
    else
        ++g.climbEdges;
}

int SpanX(const Graph& g, int idx) {
    return std::abs(g.x2[idx] - g.x1[idx]);
}

bool IsWallFh(const Graph& g, int idx) { return SpanX(g, idx) < kFallMinSpanX; }

// 沿 Walk（Prev/Next）展开整条连续台面，得到链条 X 范围；仅链条端点做悬崖内缩。
// 段间接合处不内缩——那是链表节点缝，不是掉落边。
bool ChainSafeXRange(const Graph& g, int seed, int* outLo, int* outHi) {
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
            const int v = static_cast<int>(g.adj[u][e].to);
            if (v < 0 || v >= g.n || IsWallFh(g, v) || seen[v]) continue;
            seen[v] = true;
            stack[sn++] = v;
        }
    }
    if (visited <= 0 || chainMax < chainMin) return false;

    // 链条左右端 = 真正可能掉下去的地方；中间节点缝不缩。
    int lo = chainMin + kEndInsetCliff;
    int hi = chainMax - kEndInsetCliff;
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

// 本段某端 X 是否与 Walk 邻台共享（段缝 / 交接节点）。
bool HasWalkNeighborAtX(const Graph& g, int idx, int edgeX) {
    for (int e = 0; e < g.deg[idx]; ++e) {
        if (g.adj[idx][e].kind != EdgeKind::Walk) continue;
        const int v = static_cast<int>(g.adj[idx][e].to);
        if (v < 0 || v >= g.n || IsWallFh(g, v)) continue;
        if (g.x1[v] == edgeX || g.x2[v] == edgeX) return true;
    }
    return false;
}

// 指定 FH 线段上可站的 X = 本段 ∩ 链条安全带。
// avoidWalkJunction：战斗用，再避开 Walk 段缝；贴门/赶路传 false（接合处可站）。
bool SafeStandXRange(const Graph& g, int idx, int* outLo, int* outHi, bool avoidWalkJunction) {
    if (!outLo || !outHi || idx < 0 || idx >= g.n || IsWallFh(g, idx)) return false;
    const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
    const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
    if (xmax - xmin < kFallMinSpanX) return false;

    int chainLo = 0, chainHi = 0;
    if (!ChainSafeXRange(g, idx, &chainLo, &chainHi)) return false;

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

int ClampToSafeStandX(const Graph& g, int idx, int ix, bool avoidWalkJunction) {
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi, avoidWalkJunction)) {
        const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
        const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
        return (xmin + xmax) / 2;
    }
    int cx = (std::max)(lo, (std::min)(hi, ix));
    // 宽段才偏置：贴 lo/hi 时往段内收，躲开接缝刀刃（赶路 avoidWalkJunction=false 同样受益）。
    if (hi - lo >= kEdgeBiasMinSpan) {
        if (cx == lo) cx = lo + kEdgeBiasPx;
        else if (cx == hi) cx = hi - kEdgeBiasPx;
    }
    return cx;
}

// 同 z 链被端点内缩压成单点 = 台面过短，fill+Doing 易滑落（勿当战斗落点）。
bool ChainTooNarrowToStand(const Graph& g, int idx) {
    int lo = 0, hi = 0;
    if (!ChainSafeXRange(g, idx, &lo, &hi)) return true;
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

int BestFhNear(const Graph& g, int x, int y, int xTol, int yTol) {
    int best = -1;
    int bestDy = 0x7fffffff;
    for (int i = 0; i < g.n; ++i) {
        if (!FhNearPoint(g, i, x, y, xTol, yTol)) continue;
        const int fy = FhYAtX(g.x1[i], g.y1[i], g.x2[i], g.y2[i], x);
        const int dy = std::abs(fy - y);
        if (dy < bestDy) {
            bestDy = dy;
            best = i;
        }
    }
    return best;
}

// 从可穿台 FH 垂落到正下方最近水平台（采样 3 点 X）
void AddFallDownEdges(Graph& g) {
    for (int i = 0; i < g.n; ++i) {
        if (g.forbidFall[i]) continue;
        if (IsWallFh(g, i)) continue;

        const int xa = g.x1[i], xb = g.x2[i];
        const int xs[3] = {xa + (xb - xa) / 4, (xa + xb) / 2, xa + 3 * (xb - xa) / 4};

        for (int s = 0; s < 3; ++s) {
            const int x = xs[s];
            const int yFrom = FhYAtX(xa, g.y1[i], xb, g.y2[i], x);
            int best = -1;
            int bestDy = 0x7fffffff;
            for (int j = 0; j < g.n; ++j) {
                if (j == i) continue;
                if (IsWallFh(g, j)) continue;
                const int jmin = (std::min)(g.x1[j], g.x2[j]) - kFallXTol;
                const int jmax = (std::max)(g.x1[j], g.x2[j]) + kFallXTol;
                if (x < jmin || x > jmax) continue;
                const int yTo = FhYAtX(g.x1[j], g.y1[j], g.x2[j], g.y2[j], x);
                const int dy = yTo - yFrom;  // >0 = 下方
                if (dy < kFallMinDy || dy > kFallMaxDy) continue;
                if (dy < bestDy) {
                    bestDy = dy;
                    best = j;
                }
            }
            if (best >= 0) AddEdge(g, i, best, EdgeKind::FallDown, -1, x, yFrom);
        }
    }
}

void Mid(const Graph& g, int idx, int& ox, int& oy) {
    ox = (static_cast<int>(g.x1[idx]) + static_cast<int>(g.x2[idx])) / 2;
    oy = (static_cast<int>(g.y1[idx]) + static_cast<int>(g.y2[idx])) / 2;
}

void BuildUnlocked(const foothold::Snapshot& snap) {
    Graph* g = EnsureGraphObj();
    *g = Graph{};
    g->mapId = snap.mapId;
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
    for (int i = 0; i < snap.footholdN; ++i) {
        const auto& f = snap.footholds[i];
        const int from = IndexOf(*g, f.id);
        if (from < 0) continue;
        if (f.prev) {
            const int to = IndexOf(*g, f.prev);
            if (to >= 0 && g->zMass[from] == g->zMass[to]) {
                int wx, wy;
                Mid(*g, to, wx, wy);
                AddEdge(*g, from, to, EdgeKind::Walk, -1, wx, wy);
            }
        }
        if (f.next) {
            const int to = IndexOf(*g, f.next);
            if (to >= 0 && g->zMass[from] == g->zMass[to]) {
                int wx, wy;
                Mid(*g, to, wx, wy);
                AddEdge(*g, from, to, EdgeKind::Walk, -1, wx, wy);
            }
        }
    }

    // Rope / ladder: geometry attach top(min Y) & bottom(max Y)
    for (int ri = 0; ri < gLadderN; ++ri) {
        const auto& lr = gLadders[ri];
        const int yTop = (std::min)(lr.y1, lr.y2);
        const int yBot = (std::max)(lr.y1, lr.y2);
        const int up = BestFhNear(*g, lr.x, yTop, kRopeXTol, kRopeYTol);
        const int dn = BestFhNear(*g, lr.x, yBot, kRopeXTol, kRopeYTol);
        if (up < 0 || dn < 0 || up == dn) continue;
        ++g->ropeLinked;
        AddEdge(*g, up, dn, EdgeKind::ClimbDown, ri, lr.x, yTop);
        AddEdge(*g, dn, up, EdgeKind::ClimbUp, ri, lr.x, yBot);
    }

    AddFallDownEdges(*g);

    g->ok = g->n > 0;
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
    return RebuildFromCacheUnlocked();
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

bool SnapStandAt(float x, float y, float* outX, float* outY, uint32_t* outFhId, bool preferFlat,
                 bool avoidWalkJunction) {
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
    constexpr int kXPad = 10;
    // 同层带宽：优先 |fy-y| 落在此内，避免 BIN「怪 Y=-215 却落到 -155」窜层。
    constexpr int kStandYBand = 45;  // 与 simple_combat kSameLayerY 对齐
    // 同 X 叠台：在此带内优先最上表面（min fy）。BIN 7b792b：贴下层 526 人站 470 → 死循环。
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
        int bestStackFy = 0x7fffffff;
        int bestAny = -1;
        float bestAnyD = 1e9f;

        for (int i = 0; i < g.n; ++i) {
            if (IsWallFh(g, i)) continue;
            if (!allowNarrow && ChainTooNarrowToStand(g, i)) continue;
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
            if (dy <= kStandStackBand && fy < bestStackFy) {
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
    const int cx = ClampToSafeStandX(g, pick, wishX, avoidWalkJunction);
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

    out->nodes = g.n;
    int bestIdx = -1;
    for (int i = 0; i < g.n; ++i) {
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
    // 贴门：只做悬崖内缩，不用段缝内缩（避免站位被挪出触发框）。
    constexpr bool kPortalAvoidJunction = false;
    if (!EnsureGraph()) {
        return SnapStandAt(x, y, outX, outY, outFhId, /*preferFlat=*/true, kPortalAvoidJunction);
    }

    const int ix = static_cast<int>(std::lround(x));
    const int iy = static_cast<int>(std::lround(y));
    int iLo = ix - 20;
    int iHi = ix + 20;
    if (rectValid && std::isfinite(rectL) && std::isfinite(rectR) && rectR >= rectL) {
        iLo = static_cast<int>(std::floor(rectL));
        iHi = static_cast<int>(std::ceil(rectR));
    }
    int yLo = iy - 80;
    int yHi = iy + 80;
    if (rectValid && std::isfinite(rectT) && std::isfinite(rectB) && rectB >= rectT) {
        yLo = static_cast<int>(std::floor(rectT));
        yHi = static_cast<int>(std::ceil(rectB));
    }
    constexpr int kSeedYBand = 72;
    constexpr int kXPad = 10;

    {
        std::lock_guard<std::mutex> lock(gMu);
        if (!gGraph || !gGraph->ok) {
            // fall through
        } else {
            const Graph& g = *gGraph;

            // 1) 种子：覆盖 portal.x、同层最近的 Walk 台（定链用）。
            int seed = -1;
            int bestSeedDy = 0x7fffffff;
            int bestSeedFy = 0x7fffffff;
            float bestSeedAny = 1e9f;
            int seedAny = -1;
            for (int i = 0; i < g.n; ++i) {
                if (IsWallFh(g, i) || ChainTooNarrowToStand(g, i)) continue;
                const int xa = g.x1[i], xb = g.x2[i];
                const int xmin = (std::min)(xa, xb);
                const int xmax = (std::max)(xa, xb);
                const int cx = (std::max)(xmin, (std::min)(xmax, ix));
                const int fy = FhYAtX(xa, g.y1[i], xb, g.y2[i], cx);
                const int dy = std::abs(fy - iy);
                int mx, my;
                Mid(g, i, mx, my);
                const float dAny = std::sqrt(static_cast<float>((mx - ix) * (mx - ix) +
                                                                (my - iy) * (my - iy)));
                if (dAny < bestSeedAny) {
                    bestSeedAny = dAny;
                    seedAny = i;
                }
                const bool cover = (ix >= xmin - kXPad && ix <= xmax + kXPad);
                if (!cover || dy > kSeedYBand) continue;
                if (dy < bestSeedDy || (dy == bestSeedDy && fy < bestSeedFy)) {
                    bestSeedDy = dy;
                    bestSeedFy = fy;
                    seed = i;
                }
            }
            if (seed < 0) seed = seedAny;
            if (seed >= 0) {
                // 2) 展开覆盖门的整条 Walk 链。
                bool seen[foothold::kMaxFootholds]{};
                int stack[foothold::kMaxFootholds];
                int chain[foothold::kMaxFootholds];
                int sn = 0, cn = 0;
                stack[sn++] = seed;
                seen[seed] = true;
                while (sn > 0) {
                    const int u = stack[--sn];
                    chain[cn++] = u;
                    for (int e = 0; e < g.deg[u]; ++e) {
                        if (g.adj[u][e].kind != EdgeKind::Walk) continue;
                        const int v = static_cast<int>(g.adj[u][e].to);
                        if (v < 0 || v >= g.n || IsWallFh(g, v) || seen[v]) continue;
                        seen[v] = true;
                        stack[sn++] = v;
                    }
                }

                // 3) 链内：与触发框 X 相交 → 最平；Y 落在框内优先。
                int best = -1;
                int bestSlope = 0x7fffffff;
                int bestDy = 0x7fffffff;
                int bestWish = ix;
                bool bestInY = false;
                bool bestCoverX = false;
                for (int ci = 0; ci < cn; ++ci) {
                    const int i = chain[ci];
                    if (IsWallFh(g, i) || ChainTooNarrowToStand(g, i)) continue;
                    const int xa = g.x1[i], xb = g.x2[i];
                    const int xmin = (std::min)(xa, xb);
                    const int xmax = (std::max)(xa, xb);
                    if (xmax < iLo || xmin > iHi) continue;  // 与兴趣带无交
                    int sLo = 0, sHi = 0;
                    if (!SafeStandXRange(g, i, &sLo, &sHi, kPortalAvoidJunction)) continue;
                    const int oLo = (std::max)(sLo, iLo);
                    const int oHi = (std::min)(sHi, iHi);
                    if (oLo > oHi) continue;
                    const int wish = (std::max)(oLo, (std::min)(oHi, ix));
                    const int fy = FhYAtX(xa, g.y1[i], xb, g.y2[i], wish);
                    const int slope = std::abs(g.y1[i] - g.y2[i]);
                    const int dy = std::abs(fy - iy);
                    const bool inY = (fy >= yLo && fy <= yHi);
                    const bool coverX = (ix >= xmin && ix <= xmax);
                    auto better = [&]() -> bool {
                        if (best < 0) return true;
                        if (inY != bestInY) return inY;
                        if (slope != bestSlope) return slope < bestSlope;
                        if (coverX != bestCoverX) return coverX;
                        if (dy != bestDy) return dy < bestDy;
                        return false;
                    };
                    if (!better()) continue;
                    best = i;
                    bestSlope = slope;
                    bestDy = dy;
                    bestWish = wish;
                    bestInY = inY;
                    bestCoverX = coverX;
                }

                if (best >= 0) {
                    const int xa = g.x1[best], xb = g.x2[best];
                    const int cx = ClampToSafeStandX(g, best, bestWish, kPortalAvoidJunction);
                    const int fy = FhYAtX(xa, g.y1[best], xb, g.y2[best], cx);
                    *outX = static_cast<float>(cx);
                    *outY = static_cast<float>(fy);
                    if (outFhId) *outFhId = g.ids[best];
                    x::runtime::LogI("FhPath",
                                     "portalSnap seedFh=%u pickFh=%u slope=%d wish=%d -> "
                                     "(%.0f,%.0f) rectX=%d..%d inY=%d",
                                     g.ids[seed], g.ids[best], bestSlope, bestWish, *outX, *outY,
                                     iLo, iHi, bestInY ? 1 : 0);
                    return true;
                }
            }
        }
    }

    return SnapStandAt(x, y, outX, outY, outFhId, /*preferFlat=*/true, kPortalAvoidJunction);
}

bool SnapOnFh(uint32_t fhId, float x, float* outX, float* outY, bool avoidWalkJunction) {
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
    // 钳进本段∩链条安全带；avoidWalkJunction 仅战斗侧开启。
    const int ix = static_cast<int>(std::lround(x));
    const int cx = ClampToSafeStandX(g, idx, ix, avoidWalkJunction);
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi, avoidWalkJunction)) return false;
    (void)lo;
    (void)hi;
    const int fy = FhYAtX(xa, g.y1[idx], xb, g.y2[idx], cx);
    *outX = static_cast<float>(cx);
    *outY = static_cast<float>(fy);
    return true;
}

bool IsXSafeOnFh(uint32_t fhId, float x, bool avoidWalkJunction) {
    if (fhId == 0 || !std::isfinite(x)) return false;
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int idx = IndexOf(g, fhId);
    if (idx < 0 || IsWallFh(g, idx)) return false;
    if (ChainTooNarrowToStand(g, idx)) return false;
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi, avoidWalkJunction)) return false;
    const int ix = static_cast<int>(std::lround(x));
    return ix >= lo && ix <= hi;
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

bool SameWalkComponent(uint32_t fhA, uint32_t fhB) {
    if (!fhA || !fhB) return false;
    if (fhA == fhB) return true;
    if (!EnsureGraph()) return false;

    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int src = IndexOf(g, fhA);
    const int dst = IndexOf(g, fhB);
    if (src < 0 || dst < 0) return false;
    if (src == dst) return true;

    // 仅 Walk 边 BFS（绳/下跳不算拟人可走）。
    static uint16_t q[foothold::kMaxFootholds];
    static uint8_t seen[foothold::kMaxFootholds];
    std::memset(seen, 0, g.n);
    int qh = 0, qt = 0;
    q[qt++] = static_cast<uint16_t>(src);
    seen[src] = 1;
    while (qh < qt) {
        const int u = q[qh++];
        if (u == dst) return true;
        for (int ei = 0; ei < g.deg[u]; ++ei) {
            if (g.adj[u][ei].kind != EdgeKind::Walk) continue;
            const int v = g.adj[u][ei].to;
            if (seen[v]) continue;
            seen[v] = 1;
            q[qt++] = static_cast<uint16_t>(v);
        }
    }
    return false;
}

bool PlanFirst(uint32_t fromFh, uint32_t toFh, FirstAction* out) {
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

    // BFS parents
    static int16_t parent[foothold::kMaxFootholds];
    static int16_t parentEdge[foothold::kMaxFootholds];  // edge index in adj[parent]
    static uint16_t q[foothold::kMaxFootholds];
    static uint8_t seen[foothold::kMaxFootholds];
    std::memset(seen, 0, g.n);
    for (int i = 0; i < g.n; ++i) {
        parent[i] = -1;
        parentEdge[i] = -1;
    }

    int qh = 0, qt = 0;
    q[qt++] = static_cast<uint16_t>(src);
    seen[src] = 1;

    bool found = false;
    while (qh < qt) {
        const int u = q[qh++];
        if (u == dst) {
            found = true;
            break;
        }
        for (int ei = 0; ei < g.deg[u]; ++ei) {
            const int v = g.adj[u][ei].to;
            if (seen[v]) continue;
            seen[v] = 1;
            parent[v] = static_cast<int16_t>(u);
            parentEdge[v] = static_cast<int16_t>(ei);
            q[qt++] = static_cast<uint16_t>(v);
        }
    }
    if (!found) return false;

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
    out->hops = hops;
    if (e.ropeIdx >= 0 && e.ropeIdx < gLadderN)
        out->ropeId = gLadders[e.ropeIdx].id;
    return true;
}

bool GetGraphMeta(GraphMeta* out) {
    if (!out) return false;
    *out = GraphMeta{};
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    out->ok = true;
    out->mapId = gGraph->mapId;
    out->nodes = gGraph->n;
    out->walkEdges = gGraph->walkEdges;
    out->climbEdges = gGraph->climbEdges;
    out->fallEdges = gGraph->fallEdges;
    out->ropeLinked = gGraph->ropeLinked;
    return true;
}

}  // namespace x::features::ports::foothold_path
