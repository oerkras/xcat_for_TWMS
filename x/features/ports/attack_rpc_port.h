#pragma once
// attack_rpc_port — Classic TWMS 攻包伪造探针（Create opcode + Encode + Network_SendOutPacket）
//
// opcode 不手选。近战普攻（A 空 / BasicAction Attack）与 A 槽近战攻击技（opcode 50 + skillId）。
// A 槽魔法攻击技（attackCount≥1，如魔力爪）组 opcode 52。飞镖/弓/弩/枪（51）、杖普攻 NA、蓄力技：不组包。
// 双飞斩 / BUFF / 宏：不组包。
// 空绑回退普攻在 attack_input_port（合成 5/52），此处不再发明一条。
// 真源：docs/features/attack_rpc/P0c_攻包BODY布局.md
// 默认关；禁止 GA .text E9；禁止 Session.Send 旁路。
// 不替代 attack_input_port（OnFuncKey 正路）。

#include <Windows.h>
#include <cstdint>

namespace x::features::ports::attack_rpc {

struct FireResult {
    bool ok = false;
    int mobs = 0;
    int bodyHint = 0;  // 粗算 BODY 字节（不含 6B 头）
    int opcode = 0;      // 50/51/52，由装备决定（仅普攻组包）
    int weaponType = 0;  // 当前装备 MapleWeaponType
    int skillId = 0;     // 普攻 0；A 槽近战技为技能 id
    int fkType = -1;     // FuncType；读失败 -1
    int fkValue = -1;
    const char* err = "";
};

void Init();
void Shutdown();
bool EnsureBound();
bool Ready();

// 默认 false。未开时 TryFire* 直接拒绝。
void SetEnabled(bool on);
bool IsEnabled();

void SetMaxMobs(int n);  // clamp 1..15；默认 1
int GetMaxMobs();

void SetIntervalMs(DWORD ms);  // 默认 500
DWORD GetIntervalMs();

void SetDamage(int dmg);  // 每 hit Encode4；默认 1
int GetDamage();

void SetSkillId(int skillId);  // 忽略：skillId 由 A 槽决定
int GetSkillId();              // 最近一次组包写入的 skillId（普攻为 0）

// 主线程组包发送（managed_main）。需 IsEnabled()。近战普攻、A 槽近战攻击技、或 A 槽魔法攻击技。
bool TryFireNormal(FireResult* out = nullptr);
bool TryFireMelee(FireResult* out = nullptr);  // 兼容旧名 → TryFireNormal

// 实验直调（仅 attack_rpc TAB / ATTACK_RPC）：手搓 BODY。
// 多发普攻不得调用——2026-08-04 BIN 证实踢号；多发 NA 只走 OnFuncKey。
bool TryFireNormalDirect(FireResult* out = nullptr);

// 面板按钮 oneshot：不依赖 gEnabled；命中环最多 1 只；真 Ap XY；近距门同 TryFireNormal。
// 贴脸 ~50px 即可，不强制 MobCtrlState>0（Passive 贴脸 BIN 已打死）。
bool TryFireOneshot(FireResult* out = nullptr);

// 打怪实验：SendOutPacket。hits[0] 必为 lockOid（须在 snap、活着、且在攻击盒内）。
// 命中环只填这一只锁（nHit=1）。不看 gEnabled（与「攻包伪造探针」Tick 独立）；不走 Session.Send。
// oid<=0 直接拒绝。过远（自组攻包攻击盒 |dx|/|dy|）/ SendOut false / 非近战且非 A 槽魔法技：拒发。
// 调用方不得退回 OnFuncKey。
bool TryFireLockOid(int32_t oid, FireResult* out = nullptr);

// 钉锁过远尺：出刀自组攻包自己的攻击盒（AbsPos 半宽/半高；0=该轴不限）。不与站桩面前盒共用。
void SetLockFrontBox(uint32_t dx, uint32_t dy);

// 换图：丢掉 2s Rebind 缓存。InterStage 时 opcode HashSet 会被清空。
void InvalidateAfterMapChange();

// 间隔门控；enabled 时调 TryFireNormal。
void Tick();

// 清零进程内成功计数，之后可再勾选。距上一刀不足 2.5s 会排队到闲置够了再清。
bool ResetSessionCap(const char* why = nullptr);

// auto_stop 后递增；面板读 stop_seq 勾灭。worker 把 gen 变化写成会话文件。
uint32_t PeekStopGen();

}  // namespace x::features::ports::attack_rpc
