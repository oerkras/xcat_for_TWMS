#include "travel_graph.h"

#include "../../runtime/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace x::features::travel {
namespace {

// 爬图安全跳过：复活点 / 活动出口 / 事件门等陷阱门（按韩文名子串，UTF-8 字节匹配）。
bool NameIsTrap(const std::string& name) {
    static const char* kSkip[] = {
        "\xeb\xb6\x80\xed\x99\x9c",        // 부활 (复活)
        "\xed\x83\x88\xec\xb6\x9c",        // 탈출 (脱出/出口)
        "\xec\x9d\xb4\xeb\xb2\xa4\xed\x8a\xb8",  // 이벤트 (事件)
    };
    for (const char* k : kSkip) {
        if (name.find(k) != std::string::npos) return true;
    }
    return false;
}

// 门是否"可发候选"的静态部分（不含目标已否探访的判断）：可见、非市场、非陷阱。
bool PortalFireable(const Portal& p) {
    return p.vis && !p.fm && !NameIsTrap(p.name);
}

std::vector<std::string> SplitTabs(const std::string& s, int maxFields) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size() && static_cast<int>(out.size()) + 1 < maxFields) {
        const size_t tab = s.find('\t', pos);
        if (tab == std::string::npos) break;
        out.emplace_back(s.substr(pos, tab - pos));
        pos = tab + 1;
    }
    if (pos <= s.size()) out.emplace_back(s.substr(pos));
    return out;
}

void StripBom(std::string& s) {
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB && static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

}  // namespace

bool Graph::IsBogusDest(const std::string& dest) {
    if (dest == "0" || dest == "-1" || dest == "999999999") return true;
    // PadMapKey("0") → "000000000" 等同假目标
    if (!dest.empty() && dest.find_first_not_of('0') == std::string::npos) return true;
    return false;
}

bool Graph::IsRealDest(const std::string& dest) {
    return !dest.empty() && dest != "__DEAD__" && !IsBogusDest(dest);
}

bool Graph::UpsertMap(const std::string& map, const std::vector<ports::travel::PortalInfo>& portals) {
    auto& m = maps_[map];
    bool added = false;
    for (const auto& pi : portals) {
        auto it = m.find(pi.id);
        if (it == m.end()) {
            Portal p{};
            p.name = pi.name;
            p.vis = pi.vis;
            p.fm = pi.fm;
            m.emplace(pi.id, std::move(p));
            added = true;
        } else {
            // 刷新可变属性（名字/可见性可能随状态变），不动已学 dest。
            it->second.name = pi.name;
            it->second.vis = pi.vis;
            it->second.fm = pi.fm;
        }
    }
    return added;
}

bool Graph::SetDest(const std::string& map, const std::string& portalId, const std::string& dest,
                    const std::string& destKey, bool measured) {
    if (dest != "__DEAD__" && !dest.empty()) {
        if (IsBogusDest(dest) || dest == map || !IsRealDest(dest)) return false;
    }
    // 采集可能先于 EnumPortals 拿到门 → 缺失则创建占位（名字/可见性后续 UpsertMap 补全）。
    Portal& p = maps_[map][portalId];
    // 已标 __DEAD__ 的门不被「被动建边/盲发/MapData」覆盖（防超时后下 tick 又「可达」）；
    // 仅实测换图到达（measured=true，且调用方必须是「确认换图」收尾）可纠正误杀。
    if (p.dest == "__DEAD__" && dest != "__DEAD__" && !measured) return false;
    // 非实测：不得用不同目标覆盖已有真边（脚本 tip / 空 MapData 重放）；同值视为无变更。
    // __DEAD__ 仍允许（超时标记）；measured 可纠正错误 tip/旧边。
    if (!measured && dest != "__DEAD__" && IsRealDest(p.dest) && p.dest != dest) return false;

    bool changed = false;
    if (p.dest != dest) {
        p.dest = dest;
        changed = true;
    }
    if (!destKey.empty() && !IsBogusDest(destKey) && p.destKey != destKey) {
        p.destKey = destKey;  // 盲发无 key 时不抹掉已采集的 key
        changed = true;
    }
    return changed;
}

bool Graph::IsVisited(const std::string& map) const {
    auto mi = maps_.find(map);
    return mi != maps_.end() && !mi->second.empty();
}

bool Graph::HasUnexplored(const std::string& map) const {
    return !PickUnexplored(map).empty();
}

std::string Graph::PickUnexplored(const std::string& map) const {
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return "";
    // 一轮：MapData/离线 seed 已证 destKey，且目标未探访。
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (!p.destKey.empty() && IsRealDest(p.dest) && !IsVisited(p.dest)) return kv.first;
    }
    // 二轮：已知去向且目标尚未探访。
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (IsRealDest(p.dest) && !IsVisited(p.dest)) return kv.first;
    }
    // 三轮兜底：未采集到去向（盲发，靠换图学边）。
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (p.dest.empty()) return kv.first;
    }
    return "";
}

std::string Graph::PickBlind(const std::string& map) const {
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return "";
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (p.dest.empty()) return kv.first;  // 去向未学的可发门 = 盲发探路候选
    }
    return "";
}

int Graph::EdgeCount() const {
    int n = 0;
    for (const auto& mkv : maps_) {
        for (const auto& pkv : mkv.second) {
            if (IsRealDest(pkv.second.dest)) ++n;
        }
    }
    return n;
}

int Graph::VisitedCount() const {
    int n = 0;
    for (const auto& mkv : maps_) {
        if (!mkv.second.empty()) ++n;
    }
    return n;
}

void Graph::ListMaps(std::vector<std::string>& out) const {
    std::unordered_set<std::string> seen;
    for (const auto& mkv : maps_) {
        if (seen.insert(mkv.first).second) out.push_back(mkv.first);
        for (const auto& pkv : mkv.second) {
            const std::string& d = pkv.second.dest;
            if (IsRealDest(d) && seen.insert(d).second) out.push_back(d);
        }
    }
}

std::string Graph::DestKeyOf(const std::string& mapName) const {
    for (const auto& mkv : maps_) {
        for (const auto& pkv : mkv.second) {
            const Portal& p = pkv.second;
            if (p.dest == mapName && !p.destKey.empty()) return p.destKey;
        }
    }
    return "";
}

std::string Graph::PortalToDest(const std::string& map, const std::string& destMap) const {
    auto it = maps_.find(map);
    if (it == maps_.end() || destMap.empty()) return "";
    for (const auto& pkv : it->second) {
        if (pkv.second.dest == destMap) return pkv.first;
    }
    return "";
}

std::string Graph::PortalDest(const std::string& map, const std::string& portalId) const {
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return "";
    auto pi = mi->second.find(portalId);
    return pi == mi->second.end() ? std::string() : pi->second.dest;
}

std::string Graph::PortalName(const std::string& map, const std::string& portalId) const {
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return "";
    auto pi = mi->second.find(portalId);
    return pi == mi->second.end() ? std::string() : pi->second.name;
}

int Graph::SetDestForPortalName(const std::string& map, const std::string& portalName,
                                const std::string& dest) {
    if (map.empty() || portalName.empty()) return 0;
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return 0;
    std::vector<std::string> ids;
    ids.reserve(8);
    for (const auto& kv : mi->second) {
        if (kv.second.name == portalName) ids.push_back(kv.first);
    }
    int n = 0;
    for (const auto& id : ids) {
        if (SetDest(map, id, dest)) ++n;
    }
    return n;
}

bool Graph::PathExistsAvoidingPortalName(const std::string& src, const std::string& dst,
                                         const std::string& avoidName) const {
    if (src == dst) return true;
    if (src.empty() || dst.empty() || maps_.find(src) == maps_.end()) return false;
    std::unordered_set<std::string> seen{src};
    std::deque<std::string> q{src};
    while (!q.empty()) {
        const std::string cur = q.front();
        q.pop_front();
        auto mi = maps_.find(cur);
        if (mi == maps_.end()) continue;
        for (const auto& kv : mi->second) {
            if (!avoidName.empty() && kv.second.name == avoidName) continue;
            const std::string& dest = kv.second.dest;
            if (!IsRealDest(dest) || seen.count(dest)) continue;
            if (dest == dst) return true;
            seen.insert(dest);
            q.push_back(dest);
        }
    }
    return false;
}

bool Graph::IsUniqueBridgeName(const std::string& src, const std::string& dst,
                               const std::string& portalName) const {
    if (portalName.empty() || PathTo(src, dst).empty()) return false;
    return !PathExistsAvoidingPortalName(src, dst, portalName);
}

int Graph::ReviveDeadFromDestKey(const std::string& map) {
    if (map.empty()) return 0;
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return 0;
    std::vector<std::pair<std::string, std::string>> revive;
    revive.reserve(8);
    for (const auto& kv : mi->second) {
        if (kv.second.dest != "__DEAD__") continue;
        if (!IsRealDest(kv.second.destKey)) continue;
        revive.emplace_back(kv.first, kv.second.destKey);
    }
    int n = 0;
    for (const auto& r : revive) {
        if (SetDest(map, r.first, r.second, r.second, /*measured=*/true)) ++n;
    }
    return n;
}

int Graph::BackfillDestByName() {
    // 节点名集合 = 所有图节点 ∪ 所有真实 dest。
    std::unordered_set<std::string> nodes;
    for (const auto& mkv : maps_) {
        nodes.insert(mkv.first);
        for (const auto& pkv : mkv.second)
            if (IsRealDest(pkv.second.dest)) nodes.insert(pkv.second.dest);
    }
    int filled = 0;
    for (auto& mkv : maps_) {
        for (auto& pkv : mkv.second) {
            Portal& p = pkv.second;
            if (!p.dest.empty()) continue;          // 已学/已 __DEAD__ 不动
            if (p.fm || NameIsTrap(p.name)) continue;  // 自由市场/陷阱门不补
            if (p.name == mkv.first) continue;       // 自环不补
            if (nodes.count(p.name)) {              // 门名精确=某已知图节点
                p.dest = p.name;
                ++filled;
            }
        }
    }
    return filled;
}

int Graph::ImportPeerEdges(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return 0;

    char line[8192];
    int summaryRows = 0;
    int entityRows = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        std::string s(line);
        StripBom(s);
        if (s.empty() || s[0] == '#') continue;
        if (s.rfind("kind\t", 0) == 0) continue;

        const std::vector<std::string> cols = SplitTabs(s, 10);
        if (cols.size() < 5) continue;
        if (cols[0] == "summary") ++summaryRows;
        else if (cols[0] == "entity") ++entityRows;
        else continue;

        // peer 文件里的 entity/<uuid> 是“沿途地图 uuid”，不是当前 Artale 运行时 portalId。
        // 这里仅统计行数，严禁写入门图；UUID→韩 key 的有限锚点由 peer_planner 独立读取离线表。
    }
    fclose(f);
    x::runtime::LogI("TravelGraph",
                  "peer_edges scan %s: summary=%d entity=%d applied=0(地图级候选，未注入门图)",
                  path.c_str(), summaryRows, entityRows);
    return 0;
}

std::vector<Hop> Graph::PathTo(const std::string& src, const std::string& dst) const {
    std::vector<Hop> path;
    if (src == dst) return path;
    if (maps_.find(src) == maps_.end()) return path;

    // BFS：记录到达每个 map 的前驱 (prevMap, portalId)。
    std::unordered_map<std::string, Hop> prev;
    std::unordered_set<std::string> seen{src};
    std::deque<std::string> q{src};
    bool found = false;
    while (!q.empty()) {
        const std::string cur = q.front();
        q.pop_front();
        if (cur == dst) { found = true; break; }
        auto mi = maps_.find(cur);
        if (mi == maps_.end()) continue;
        for (const auto& kv : mi->second) {
            const std::string& dest = kv.second.dest;
            if (!IsRealDest(dest) || seen.count(dest)) continue;
            seen.insert(dest);
            prev[dest] = Hop{cur, kv.first, dest};
            q.push_back(dest);
        }
    }
    if (!found) return path;
    // 回溯 dst→src，反转得首跳在前。
    std::string cur = dst;
    while (cur != src) {
        auto it = prev.find(cur);
        if (it == prev.end()) return {};  // 异常
        path.push_back(it->second);
        cur = it->second.map;
    }
    std::vector<Hop> ordered(path.rbegin(), path.rend());
    return ordered;
}

int Graph::PathDistance(const std::string& src, const std::string& dst) const {
    if (src == dst) return 0;
    if (maps_.find(src) == maps_.end()) return -1;
    std::unordered_set<std::string> seen{src};
    std::deque<std::pair<std::string, int>> q{{src, 0}};
    while (!q.empty()) {
        const auto [cur, dist] = q.front();
        q.pop_front();
        auto mi = maps_.find(cur);
        if (mi == maps_.end()) continue;
        for (const auto& kv : mi->second) {
            const std::string& dest = kv.second.dest;
            if (!IsRealDest(dest) || seen.count(dest)) continue;
            if (dest == dst) return dist + 1;
            seen.insert(dest);
            q.emplace_back(dest, dist + 1);
        }
    }
    return -1;
}

std::string Graph::PickUnexploredWithDest(const std::string& map, std::string& outDest) const {
    outDest.clear();
    auto mi = maps_.find(map);
    if (mi == maps_.end()) return "";
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (!p.destKey.empty() && IsRealDest(p.dest) && !IsVisited(p.dest)) {
            outDest = p.dest;
            return kv.first;
        }
    }
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (IsRealDest(p.dest) && !IsVisited(p.dest)) {
            outDest = p.dest;
            return kv.first;
        }
    }
    for (const auto& kv : mi->second) {
        const Portal& p = kv.second;
        if (!PortalFireable(p)) continue;
        if (p.dest.empty()) return kv.first;
    }
    return "";
}

bool Graph::NextUnexploredStep(const std::string& src, bool& outLocal, Hop& out) const {
    outLocal = false;
    out = Hop{};
    if (HasUnexplored(src)) {
        outLocal = true;
        std::string dest;
        const std::string pid = PickUnexploredWithDest(src, dest);
        if (pid.empty()) return false;
        out.map = src;
        out.portalId = pid;
        out.destMap = dest;
        return true;
    }
    if (maps_.find(src) == maps_.end()) return false;

    std::unordered_map<std::string, Hop> prev;
    std::unordered_set<std::string> seen{src};
    std::deque<std::string> q{src};
    std::string goalMap;
    while (!q.empty()) {
        const std::string cur = q.front();
        q.pop_front();
        if (cur != src && HasUnexplored(cur)) { goalMap = cur; break; }
        auto mi = maps_.find(cur);
        if (mi == maps_.end()) continue;
        for (const auto& kv : mi->second) {
            const std::string& dest = kv.second.dest;
            if (!IsRealDest(dest) || seen.count(dest)) continue;
            seen.insert(dest);
            prev[dest] = Hop{cur, kv.first, dest};
            q.push_back(dest);
        }
    }
    if (goalMap.empty()) return false;
    std::string cur = goalMap;
    Hop first{};
    while (cur != src) {
        auto it = prev.find(cur);
        if (it == prev.end()) return false;
        first = it->second;
        cur = it->second.map;
    }
    out = first;
    out.map = src;
    return true;
}

namespace {

int ParseMapIdNum(const std::string& map) {
    if (map.empty()) return 0;
    char* end = nullptr;
    const long v = std::strtol(map.c_str(), &end, 10);
    if (!end || *end != '\0' || v <= 0) return 0;
    return static_cast<int>(v);
}

// Smaller is better. Known BFS path to target beats map-id heuristic.
long long ScoreDestTowardTarget(const Graph& g, const std::string& dest, const std::string& target) {
    constexpr long long kHuge = (std::numeric_limits<long long>::max)() / 8;
    if (!Graph::IsRealDest(dest)) return kHuge;
    const int pathDist = g.PathDistance(dest, target);
    if (pathDist >= 0) {
        // Prefer shorter remaining path; slight penalty if dest already visited.
        return static_cast<long long>(pathDist) * 1000000LL + (g.IsVisited(dest) ? 1LL : 0LL);
    }
    const int a = ParseMapIdNum(dest);
    const int b = ParseMapIdNum(target);
    if (a > 0 && b > 0) {
        const long long idDist = (a > b) ? (static_cast<long long>(a) - b)
                                         : (static_cast<long long>(b) - a);
        return 1000000000LL + idDist * 10LL + (g.IsVisited(dest) ? 1LL : 0LL);
    }
    return kHuge - 1;
}

}  // namespace

bool Graph::NextStepTowardTarget(const std::string& src, const std::string& target, bool& outLocal,
                                 Hop& out) const {
    outLocal = false;
    out = Hop{};
    if (src.empty() || target.empty() || src == target) return false;

    auto mi = maps_.find(src);
    if (mi != maps_.end()) {
        long long bestScore = (std::numeric_limits<long long>::max)();
        Hop best{};
        bool have = false;
        for (const auto& kv : mi->second) {
            const Portal& p = kv.second;
            if (!PortalFireable(p)) continue;
            const std::string& dest = p.dest;
            // Unknown dest: keep as weak candidate only if nothing better.
            const long long score = IsRealDest(dest) ? ScoreDestTowardTarget(*this, dest, target)
                                                     : (std::numeric_limits<long long>::max)() / 4;
            if (!have || score < bestScore) {
                have = true;
                bestScore = score;
                best.map = src;
                best.portalId = kv.first;
                best.destMap = IsRealDest(dest) ? dest : "";
            }
        }
        // Accept if we have a real-dest candidate with finite heuristic, or only unknowns.
        if (have && bestScore < (std::numeric_limits<long long>::max)() / 2) {
            outLocal = true;
            out = best;
            return true;
        }
    }

    return NextUnexploredStep(src, outLocal, out);
}

bool Graph::Load(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return false;
    char line[2048];
    int rows = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        // 去尾换行
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        std::string s(line);
        // 历史桥行：@bind\t<韩key>\t<uuid>。当前 Artale 自定义服不再把运行时 map.Id 当地图身份，忽略。
        if (s.rfind("@bind\t", 0) == 0) continue;
        // <fromMap>\t<portalId>\t<name>\t<vis>\t<fm>\t<dest>\t<destKey>
        std::string f0, f1, f2, f3, f4, f5, f6;
        size_t pos = 0;
        auto next = [&](std::string& out) -> bool {
            size_t t = s.find('\t', pos);
            if (t == std::string::npos) {
                out = s.substr(pos);
                pos = s.size();
                return false;
            }
            out = s.substr(pos, t - pos);
            pos = t + 1;
            return true;
        };
        if (!next(f0)) continue;
        if (!next(f1)) continue;
        if (!next(f2)) continue;
        if (!next(f3)) continue;
        next(f4);  // f4 后还有 dest/destKey（均可空）
        next(f5);
        next(f6);
        Portal p{};
        p.name = f2;
        p.vis = (f3 == "1");
        p.fm = (f4 == "1");
        // 保留 __DEAD__：假火/超时标死必须跨重载/种子叠加仍有效（BIN：Load 清空后
        // 种子 MapData 边把 under00 复活 → 死循环）。误杀仅可由实测换图 SetDest(measured) 纠正。
        p.dest = f5;
        p.destKey = f6;
        if (p.dest != "__DEAD__" && IsBogusDest(p.dest)) p.dest.clear();
        if (IsBogusDest(p.destKey)) p.destKey.clear();
        // 二次 Load（随包种子）不得覆盖已有 __DEAD__，只刷新门名/可见性。
        auto mi = maps_.find(f0);
        if (mi != maps_.end()) {
            auto pi = mi->second.find(f1);
            if (pi != mi->second.end() && pi->second.dest == "__DEAD__") {
                if (!p.name.empty()) pi->second.name = p.name;
                pi->second.vis = p.vis;
                pi->second.fm = p.fm;
                ++rows;
                continue;
            }
        }
        maps_[f0][f1] = std::move(p);
        ++rows;
    }
    fclose(f);
    x::runtime::LogI("TravelGraph", "load %s: maps=%d edges=%d rows=%d", path.c_str(),
                  MapCount(), EdgeCount(), rows);
    // 空壳/注释占位（如 purged_artale_seed）不算成功加载。
    return rows > 0;
}

bool Graph::Save(const std::string& path) const {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
    fprintf(f, "# travel_graph v3\tfromMap\tportalId\tname\tvis\tfm\tdest\tdestKey（枫星节点=Lua CurrentMapName/key）\n");
    for (const auto& mkv : maps_) {
        for (const auto& pkv : mkv.second) {
            const Portal& p = pkv.second;
            fprintf(f, "%s\t%s\t%s\t%d\t%d\t%s\t%s\n", mkv.first.c_str(), pkv.first.c_str(),
                    p.name.c_str(), p.vis ? 1 : 0, p.fm ? 1 : 0, p.dest.c_str(), p.destKey.c_str());
        }
    }
    fclose(f);
    return true;
}

}  // namespace x::features::travel
