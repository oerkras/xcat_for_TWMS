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

// 指定 FH 线段上可站的 X = 本段 ∩ 链条安全带。
bool SafeStandXRange(const Graph& g, int idx, int* outLo, int* outHi) {
    if (!outLo || !outHi || idx < 0 || idx >= g.n || IsWallFh(g, idx)) return false;
    const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
    const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
    if (xmax - xmin < kFallMinSpanX) return false;

    int chainLo = 0, chainHi = 0;
    if (!ChainSafeXRange(g, idx, &chainLo, &chainHi)) return false;

    int lo = (std::max)(xmin, chainLo);
    int hi = (std::min)(xmax, chainHi);
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

int ClampToSafeStandX(const Graph& g, int idx, int ix) {
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi)) {
        const int xmin = (std::min)(g.x1[idx], g.x2[idx]);
        const int xmax = (std::max)(g.x1[idx], g.x2[idx]);
        return (xmin + xmax) / 2;
    }
    return (std::max)(lo, (std::min)(hi, ix));
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

    // Prev / Next walk
    for (int i = 0; i < snap.footholdN; ++i) {
        const auto& f = snap.footholds[i];
        const int from = IndexOf(*g, f.id);
        if (from < 0) continue;
        if (f.prev) {
            const int to = IndexOf(*g, f.prev);
            if (to >= 0) {
                int wx, wy;
                Mid(*g, to, wx, wy);
                AddEdge(*g, from, to, EdgeKind::Walk, -1, wx, wy);
            }
        }
        if (f.next) {
            const int to = IndexOf(*g, f.next);
            if (to >= 0) {
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

bool SnapStandAt(float x, float y, float* outX, float* outY, uint32_t* outFhId) {
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

    int bestSameYCover = -1;
    int bestSameYCoverDy = 0x7fffffff;
    int bestYBand = -1;
    float bestYBandScore = 1e9f;
    int bestLooseCover = -1;
    int bestLooseCoverDy = 0x7fffffff;
    int bestAny = -1;
    float bestAnyD = 1e9f;

    for (int i = 0; i < g.n; ++i) {
        if (IsWallFh(g, i)) continue;
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

        // Y 带：即使 X 略出台面，也优先贴目标高度（怪悬空/贴边时仍落到同层）。
        if (dy <= kStandYBand) {
            const float score = static_cast<float>(std::abs(cx - ix)) + 0.25f * static_cast<float>(dy);
            if (score < bestYBandScore) {
                bestYBandScore = score;
                bestYBand = i;
            }
        }

        const bool cover = (ix >= xmin - kXPad && ix <= xmax + kXPad);
        if (!cover) continue;
        if (dy <= kStandYBand && dy < bestSameYCoverDy) {
            bestSameYCoverDy = dy;
            bestSameYCover = i;
        }
        if (dy <= kCoverYTol && dy < bestLooseCoverDy) {
            bestLooseCoverDy = dy;
            bestLooseCover = i;
        }
    }

    // 优先级：同层且覆盖 X → 同层 Y 带 → 宽松覆盖(|dy|≤80) → 最近点
    const int pick = (bestSameYCover >= 0) ? bestSameYCover
                     : (bestYBand >= 0)      ? bestYBand
                     : (bestLooseCover >= 0) ? bestLooseCover
                                             : bestAny;
    if (pick < 0) return false;

    const int xa = g.x1[pick], xb = g.x2[pick];
    const int xmin = (std::min)(xa, xb);
    const int xmax = (std::max)(xa, xb);
    const bool clampX = (bestSameYCover >= 0 || bestLooseCover >= 0 || bestYBand >= 0);
    // 钳进本段∩链条安全带（仅链条端点内缩；Prev/Next 接合处可站）。
    const int wishX = clampX ? ix : (xmin + xmax) / 2;
    const int cx = ClampToSafeStandX(g, pick, wishX);
    const int fy = FhYAtX(xa, g.y1[pick], xb, g.y2[pick], cx);
    *outX = static_cast<float>(cx);
    *outY = static_cast<float>(fy);
    if (outFhId) *outFhId = g.ids[pick];
    return true;
}

bool SnapOnFh(uint32_t fhId, float x, float* outX, float* outY) {
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
    // 钳进本段∩链条安全带（仅链条端点内缩）。
    const int ix = static_cast<int>(std::lround(x));
    const int cx = ClampToSafeStandX(g, idx, ix);
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi)) return false;
    (void)lo;
    (void)hi;
    const int fy = FhYAtX(xa, g.y1[idx], xb, g.y2[idx], cx);
    *outX = static_cast<float>(cx);
    *outY = static_cast<float>(fy);
    return true;
}

bool IsXSafeOnFh(uint32_t fhId, float x) {
    if (fhId == 0 || !std::isfinite(x)) return false;
    if (!EnsureGraph()) return false;
    std::lock_guard<std::mutex> lock(gMu);
    if (!gGraph || !gGraph->ok) return false;
    const Graph& g = *gGraph;
    const int idx = IndexOf(g, fhId);
    if (idx < 0 || IsWallFh(g, idx)) return false;
    int lo = 0, hi = 0;
    if (!SafeStandXRange(g, idx, &lo, &hi)) return false;
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
