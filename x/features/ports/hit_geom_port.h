#pragma once
// hit_geom_port — Classic TWMS · 出刀前复刻 FindHit 相交谓词
//
// 官方命中真源（runtime IDB imagebase 0x7FFD60830000）：
//   MobPool.FindHitMobInRect  RVA 0xF89C60
//     → Mob.GetBodyRect(out Rect, continuous=true)  RVA 0xF281C0
//     → Unity Rect AABB ∩ 攻击盒
// 攻击盒来自 ActionManager._afterimageMap → MeleeAttackAfterImage.Range[action]
// （与 GetMeleeAttackRange 同源；XYWH，朝右时绕原点翻 X）。
//
// 本 port 只给出刀闸：不相交 / 取不到 → 不出 OnFuncKey。不改旋翼站距。
// GetBodyRect 是托管调用，必须 MainPump；失败禁止 worker 硬调。
// no_range（表未建）：只 Unknown 放行一刀（懒建 afterimage 需要挥一次）；其后按 Separate 压刀。
// 表仍空则 2s 后再放行一刀，避免 23:48 那种整场零 fire。泵失败 no_body 仍 Unknown 放行。
// 豁免（仍返回 Unknown，调用方放行）：A 槽不是普攻 5/52、或武器是弓/弩/飞镖/枪/杖。
// 近战盒闸只服务近战普攻；远程/A 槽技能走官方射击/技能盒，套 afterimage 会误压。

#include <cstdint>

namespace x::features::ports::hit_geom {

enum class Geom : uint8_t { Unknown = 0, Overlap = 1, Separate = 2 };

struct Snap {
    Geom geom = Geom::Unknown;
    int actionUsed = -1;  // 命中的 Range key；未相交 = -1
    int actionHint = -1;  // 调用方上次挥刀 action（仅日志）
    int rangeN = 0;
    int fkType = -1;      // A 槽 FuncType 缓存；未解析 -1
    int fkValue = -1;
    int weaponType = 0;   // MapleWeaponType；读失败 0
    float atkX = 0.f, atkY = 0.f, atkW = 0.f, atkH = 0.f;     // 世界 XYWH（命中那条或并集）
    float bodyX = 0.f, bodyY = 0.f, bodyW = 0.f, bodyH = 0.f;  // GetBodyRect
    const char* why = "init";
};

void Init();
void Shutdown();

// px/py = 玩家 AbsPos（更大 Y = 更高）。faceLeft = 引擎 ma bit0。
// actionHint < 0 时对当前武器 Range 并集判相交（下一刀动作未定）。
Snap QueryLockOverlap(void* mob, float px, float py, bool faceLeft, int actionHint);

}  // namespace x::features::ports::hit_geom
