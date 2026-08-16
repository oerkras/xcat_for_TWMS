#pragma once
// Classic TWMS travel port — PortalManager + WorldManager mapId + 进门。
// 产品主路径：StickUp（贴门 + unity_kbd ↑；禁直调 CheckMovePortal）。
// fill+Doing 已废；远处贴门旋翼滑翔，站稳后再进门。
// 调试旁路：Up / CheckMove / DirectEnter（贴门+CheckMove，易断线）/ Rpc。禁止 AbsPos/Transform 硬写坐标。

#include <cstdint>
#include <string>
#include <vector>

namespace x::features::ports::travel {

struct PortalInfo {
    std::string id;    // seed:map/pn 或运行时索引名
    std::string name;  // pn
    bool        activate = true;
    bool        vis = true;
    bool        fm = false;
    std::string destMap;  // 9 位 mapId；未知空
    float       x = 0.f;
    float       y = 0.f;
    int         pt = 0;
    int         toMapId = 0;
    // 触发区（FindMovePortal 几何）；由 PortalRect 或 H/VImpact 推导
    bool  rectValid = false;
    float rectL = 0.f;
    float rectT = 0.f;
    float rectR = 0.f;
    float rectB = 0.f;
};

enum class FireMode {
    Up = 0,             // 当前位置 unity_kbd ↑（调试；不硬写坐标）
    CheckMove = 1,      // 当前位置 WM.CheckMovePortal（调试；不硬写坐标）
    Rpc = 2,            // OutPacket.Create(114) + EncodeStr(+可选 fieldKey) + Send
    StickUp = 3,        // Impact 贴门 + unity_kbd ↑（产品；禁 CheckMove）
    DirectEnter = 4,    // Impact 贴门 + CheckMovePortal（易断线，调试保留）
};

bool EnsureBound();

// 急切绑定 CheckMove / Rpc 等进门 API（无硬写坐标）。
void Init();

// 9 位 mapId。出生图 0 → "000000000"。无 MapData → 空串（不是把 0 当没进图）。
std::string CurrentMapKey();
// 当前 Field id。无 MapData → -1；出生图 → 0。
int CurrentMapId();

bool EnumPortals(std::string& outMapKey, std::vector<PortalInfo>& out);
bool FindPortalByName(const std::string& portalName, PortalInfo& out);

void SetFireMode(FireMode mode);
FireMode GetFireMode();
const char* FireModeName(FireMode mode);

// MethodInfo swap NetworkManager.Send — 抓 C→S；命中 0x72/枚举114 打 hex。
void SetCaptureEnabled(bool on);
bool IsCaptureEnabled();

// 按当前 FireMode 开火。
// warpFirst：产品模式=先 Impact 贴门（或已在门内跳过）；Rpc 时 false=允许远处手组包。
// 不含 AbsPos 硬写；fill+Doing 不再使用。
bool FirePortalByName(const std::string& portalName, std::string& outResult);
bool FirePortalByName(const std::string& portalName, bool warpFirst, std::string& outResult);

// 贴到站立点（假门）：与超级赶路同一套 Cruise→Station→hold 落地，不按 ↑、不进门。
// portal.x/y + 可选触发框；成功时 outResult="STOOD" / "ALREADY"。
bool StickToStand(const PortalInfo& portal, std::string& outResult);

// 贴门抬升 px（AbsPos 更大 Y=更高）。调试 TAB「超级赶路」下发；末段/台下恢复/发门带空悬停共用。
void SetPortalAimLiftY(uint32_t liftPx);

}  // namespace x::features::ports::travel
