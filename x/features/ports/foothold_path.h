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
// FH=Prev/Next 线段链；X 钳在「整条 Walk 链」安全带（仅链条端点内缩，接合处不缩）。
bool SnapStandAt(float x, float y, float* outX, float* outY, uint32_t* outFhId = nullptr);

// 钉死在指定 FH 线段上：X = 本段 ∩ 链条安全带。接合处可站，链条端点不可贴边落下。
bool SnapOnFh(uint32_t fhId, float x, float* outX, float* outY);

// x 是否落在该 FH 安全站立带内（与 SnapOnFh 同一套链条规则）。
bool IsXSafeOnFh(uint32_t fhId, float x);

// FH / 点附近站立台的 zMass（连通域键）。图未就绪或未命中台返回 false。
bool ZMassOfFh(uint32_t fhId, int32_t* outZMass);
bool ZMassAt(float x, float y, int32_t* outZMass, uint32_t* outFhId = nullptr);

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
