#pragma once
// Classic TWMS travel port — PortalManager + WorldManager mapId + 进门。
// 产品主路径：TeleportStick（瞬移贴门+↑）/ DirectEnter（瞬移+WM.CheckMovePortal）。
// 调试旁路：Up / CheckMove（AbsPos 硬钉）/ Rpc（手组包）。

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
    Up = 0,             // AbsPos + VK_UP（调试）
    CheckMove = 1,      // AbsPos + WorldManager.CheckMovePortal（调试）
    Rpc = 2,            // OutPacket.Create(114) + EncodeStr(+可选 fieldKey) + Send
    TeleportStick = 3,  // teleport_port 贴门 + VK_UP（对照枫星滑翔贴门）
    DirectEnter = 4,    // teleport_port 贴门 + CheckMovePortal（直调进门，默认）
};

bool EnsureBound();

// 急切绑定 Unity Transform.set_position 等（MISS 灯 TravelPos）。
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

// 按当前 FireMode 开火。warpFirst=false 时 Rpc 模式可测「远处发包」。
bool FirePortalByName(const std::string& portalName, std::string& outResult);
bool FirePortalByName(const std::string& portalName, bool warpFirst, std::string& outResult);

}  // namespace x::features::ports::travel
