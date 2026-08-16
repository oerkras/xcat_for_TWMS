#pragma once
// Classic TWMS 超级赶路 — 本板块 seed/学习图 goto（对照枫星 travel）
//
// 明确非目标：跨板块码头 / crawl v1 / Lua FireTeleport。
// 控制：state\travel_cmd.txt
//   goto <mapId或地图名>
//   stop / save
//   firemode stick|direct|…（进门策略）

namespace x::features::travel {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

// 磁盘 seed/学习图预载（不碰角色/地图托管对象）。LOGIN·MainPump 活后可调，削落地尖峰。
void PreloadGraph();

void RequestGoto(const char* target);
void RequestStop();
void RequestSave();

bool IsFeatureEnabled();
bool IsActive();

enum class UiMode { Idle, Goto };

// 赶路终态失败码（编排/面板用；勿只靠 lastMsg 文案）。
enum class FailKind : unsigned char {
    None = 0,
    Unreachable = 1,
    FakeFireStop = 2,
    BadTarget = 3,
    AlreadyThere = 4,
    FireStuck = 5,  // 贴门瞬移/站稳等瞬态失败持续超时（如 TELEPORT_FAIL）
    CombatOn = 6,   // F5 自动打怪开启时禁止赶路
};

struct Snapshot {
    UiMode   mode = UiMode::Idle;
    char     curMap[64]{};
    char     gotoTarget[64]{};
    int      mapCount = 0;
    int      edgeCount = 0;
    int      hopIdx = 0;
    int      hopTotal = 0;
    bool     playReady = false;
    char     lastMsg[160]{};
    unsigned lastUpdateMs = 0;
    FailKind failKind = FailKind::None;
    int      fakeFireCount = 0;
};

bool QuerySnapshot(Snapshot& out);

// 学习图 hop 距离：同图 0；不可达 -1；否则最短门数。
int PathHopCount(const char* srcMap, const char* dstMap);

// 回家卷轴落点（经典版≈最近城镇）：在学习图上对 map_info.town=1 取 PathHopCount 最小者；
// 无可达时若前缀截断点本身是真城镇才回退前缀。
// （101030102→101000000）。对照枫星 PredictReturnScrollTownOutdoor，但经典版
// 不能只用前缀——10103 带实际最近常是勇士之村 102000000。
bool PredictReturnScrollTownOutdoor(const char* fromMap, char* out, size_t outSz);

}  // namespace x::features::travel
