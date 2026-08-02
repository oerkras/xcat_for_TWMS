#pragma once
// attack_rpc_port — Classic TWMS 攻包伪造探针（Create 50 + Encode + Network_SendOutPacket）
//
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

void SetSkillId(int skillId);  // NormalAttack 固定 0；调用会被忽略
int GetSkillId();              // 恒为 0

// 主线程组包发送（managed_main）。需 IsEnabled()。Create(50)+skill=0。
bool TryFireNormal(FireResult* out = nullptr);
bool TryFireMelee(FireResult* out = nullptr);  // 兼容旧名 → TryFireNormal

// 间隔门控；enabled 时调 TryFireNormal。
void Tick();

}  // namespace x::features::ports::attack_rpc
