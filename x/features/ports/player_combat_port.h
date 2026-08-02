#pragma once
// player_combat_port — Classic TWMS LocalUser 战斗上下文（只读坐标）
//
// 解析纪律对齐 skill_port / titlebar：WM.MyUser 优先，FindAll + GO 名 MyUser。
// 禁止 INLINE HOOK。

#include <cstdint>

namespace x::features::ports::player_combat {

struct CombatCtx {
    bool ok = false;
    void* localUser = nullptr;
    float x = 0.f;
    float y = 0.f;
};

bool EnsureBound();
// 完整上下文：要有效坐标（打怪选怪/贴怪）。
bool QueryCombatCtx(CombatCtx& out);
// 只要 LocalUser 指针（点飞绝对落点不依赖旧 Ap；换图落地 Ap 暂坏时仍可 Doing）。
bool QueryLocalUser(void** outLu);

}  // namespace x::features::ports::player_combat
