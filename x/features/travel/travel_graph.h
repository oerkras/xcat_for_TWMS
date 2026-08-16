#pragma once
// travel_graph.h — 跨图路由图（对照枫星；经典版节点=9 位 mapId）
//
// 持久化 TSV：state\travel_graph.tsv / travel_graph.seed.tsv
// 每行：fromMap / portalId / name / vis / fm / dest / destKey
// 经典版 seed 已含真 tm 边；运行时仍可学习补边。

#include "../ports/travel_port.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace x::features::travel {

struct Portal {
    std::string name;
    bool        vis  = false;
    bool        fm   = false;
    std::string dest;     // "" 未知 / "__DEAD__" / 目标图名（韩文显示名）
    std::string destKey;  // 目标图本地化 key（采集 rawPath）；盲发学边留空
};

// 一跳：从某图经某门到 destMap（路由身份=Artale Lua CurrentMapName/key；portalId 仅作种子 hint，开火时实时解门）。
struct Hop {
    std::string map;       // 起跳图
    std::string portalId;  // 种子/学习图里的门 GUID（hint，非执行必需）
    std::string destMap;   // 本跳目标 Artale Lua map key/name
};

class Graph {
public:
    // 合并当前图枚举结果（新增节点/门；不覆盖已学 dest）。返回是否有新增门。
    bool UpsertMap(const std::string& map, const std::vector<ports::travel::PortalInfo>& portals);

    // 记录一条学习到的边（门→目标图）。dest 可为 "__DEAD__"。若 map/门不存在则创建。
    // destKey 非空时一并写入（采集路径传 rawPath；盲发学边留空，不覆盖已有 key）。
    // measured=true 仅限「实测换图到达」铁证，可纠正误标 __DEAD__。
    // EnumPortals/MapData/tip 必须 measured=false：PortalComp dest 可撒谎，不得复活假火死门。
    // 返回是否真有字段变化（同值重写返回 false，供调用方避免无意义 Save）。
    // 非 measured：不得用不同 dest 覆盖已有真实边（脚本提示/被动重放只能补空）。
    bool SetDest(const std::string& map, const std::string& portalId, const std::string& dest,
                 const std::string& destKey = "", bool measured = false);

    // 是否已"探访"过该图（图中已有 >=1 个门记录）。
    bool IsVisited(const std::string& map) const;

    // 当前图是否还有"值得发"的门：可见非市场非陷阱，且
    //   dest 未知（未采集，盲发兜底） 或 dest 已知但目标图尚未探访（已知去向、去采集）。
    bool HasUnexplored(const std::string& map) const;
    // 取当前图一个值得发的门 id；优先"已知去向且目标未探访"，其次"未采集"；无则 ""。
    std::string PickUnexplored(const std::string& map) const;
    // 取当前图一个「去向未学」的可发门 id（盲发探路用：可见、非市场/陷阱、dest 为空）；无则 ""。
    // 用于启发式已知前沿穷尽时，走盲发门换图学边，打破"死角图唯一出口是未学门"的僵局。
    std::string PickBlind(const std::string& map) const;

    // 经已知边 BFS：从 src 到 dst 的门序列（首元素为第一跳）。空=不可达。
    std::vector<Hop> PathTo(const std::string& src, const std::string& dst) const;
    int PathDistance(const std::string& src, const std::string& dst) const;

    // 经已知边 BFS：朝"最近含可探索门的图"走的第一跳；无可达未探索点返回 false。
    bool NextUnexploredStep(const std::string& src, bool& outLocal, Hop& out) const;

    // PathTo empty: pick local hop facing target (BFS remain / map-id heuristic), else NextUnexploredStep.
    bool NextStepTowardTarget(const std::string& src, const std::string& target, bool& outLocal,
                              Hop& out) const;

    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    int MapCount() const { return static_cast<int>(maps_.size()); }
    int EdgeCount() const;     // 已知 dest（非空非 __DEAD__）的门数
    int VisitedCount() const;  // 已探访（有门记录）的图数

    // 全部已知地图规范名（maps_ 节点 ∪ 所有真实 dest），去重；供解析器建别名索引。
    void ListMaps(std::vector<std::string>& out) const;
    // 取某地图名的本地化 key（任一指向它的门的非空 destKey）；无则 ""。
    std::string DestKeyOf(const std::string& mapName) const;

    // 取当前图(map)上 dest==destMap 的门 id（图内 hint；执行时应优先实时 EnumPortals 解门）。
    std::string PortalToDest(const std::string& map, const std::string& destMap) const;

    // 图内某门的已学 dest / 门名（供实时解门时作 hint）。
    std::string PortalDest(const std::string& map, const std::string& portalId) const;
    std::string PortalName(const std::string& map, const std::string& portalId) const;

    // 将 map 上所有 name==portalName 的门写入同一 dest（通常 "__DEAD__"）。
    // 同逻辑门会因 PathTo/解门留下多份 UUID 边；只标 live+seed 会漏杀 → 死循环（BIN under00）。
    // 返回实际变更条数。
    int SetDestForPortalName(const std::string& map, const std::string& portalName,
                             const std::string& dest);

    // BFS：是否存在避开「门名==avoidName」的路径（用于唯一桥判定）。
    bool PathExistsAvoidingPortalName(const std::string& src, const std::string& dst,
                                      const std::string& avoidName) const;
    // src→dst 当前有路，但避开 portalName 后无路 → 该逻辑门是通向目标的唯一桥。
    bool IsUniqueBridgeName(const std::string& src, const std::string& dst,
                            const std::string& portalName) const;

    // 把 map 上 dest=__DEAD__ 且 destKey 仍为真目标的边复活（measured）。
    // BIN 08:32：假火误杀唯一桥后 PathTo 空；destKey 列仍留着原目标可恢复。
    int ReviveDeadFromDestKey(const std::string& map);

    // 回填：对 dest 空、门名精确等于某已知图节点名的门，用门名补 dest。
    // 很多过图门的 name 字段本身就是目标图韩名（如 "솟아오른나무2"），但 crawl 单向爬只学了正向边，
    // 回程门 dest 常空 → BFS 单向断链。用门名补全双向连通（仅精确匹配节点名，不动陷阱/占位门）。
    // 返回补填条数。
    int BackfillDestByName();

    // 扫描同行 UUID 拓扑候选。注意 peer 的 entity/<uuid> 是沿途地图 UUID，不是 portalId；
    // 当前只做行数诊断，严禁直接注入门图。UUID→韩 key 的有限锚点由 peer_planner 独立读取离线表。
    int ImportPeerEdges(const std::string& path);

    // 有效过图目标（BFS/选门/建边）；排除占位 tm（0 / -1 / 999999999）。
    // 出生图 Field 0 只作 src，不作 dest（WZ tm=0 = 未填，与出生图同号）。
    static bool IsBogusDest(const std::string& dest);
    static bool IsRealDest(const std::string& dest);

private:
    // PickUnexplored + 填 outDest（盲探时 outDest 仍空）。
    std::string PickUnexploredWithDest(const std::string& map, std::string& outDest) const;

    // mapName -> (portalId -> Portal)
    std::unordered_map<std::string, std::unordered_map<std::string, Portal>> maps_;
};

}  // namespace x::features::travel
