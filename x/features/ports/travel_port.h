#pragma once
// Classic TWMS travel port — PortalManager + WorldManager mapId + 进门。
// 产品主路径：DirectEnter / StickUp —— 冲量贴门 + 掠过触发区即 CheckMove/↑。
// fill+Doing 已废；远处贴门分段冲量，进框当拍开火（不要求刹停）。
// 调试旁路：Up / CheckMove（当前位置触发）/ Rpc（手组包）。禁止 AbsPos/Transform 硬写坐标。

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
    Up = 0,             // 当前位置 VK_UP（调试；不硬写坐标）
    CheckMove = 1,      // 当前位置 WM.CheckMovePortal（调试；不硬写坐标）
    Rpc = 2,            // OutPacket.Create(114) + EncodeStr(+可选 fieldKey) + Send
    StickUp = 3,        // Impact 贴门 + VK_UP（旧名 TeleportStick）
    DirectEnter = 4,    // Impact 贴门 + CheckMovePortal（默认）
};

bool EnsureBound();

// 急切绑定 CheckMove / Rpc 等进门 API（无硬写坐标）。
void Init();

std::string CurrentMapKey();
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

}  // namespace x::features::ports::travel
