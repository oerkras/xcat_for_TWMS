#pragma once
// reach_cal — 触及包线在线自校准（横向站距）
//
// 为什么需要它：客户端命中判定是**攻击盒 × 怪体盒的矩形相交**（逆向确认，见
// docs/features/simple_combat/模块设计.md）。攻击盒来自 `GetMeleeAttackRange(afterimageUol,
// action)`，**随武器/动作变**；怪体盒来自 `Mob.GetBodyRect()`，**随怪种变**。
// 于是「站多远还能打到」是 (武器 + 怪种) 的函数，写死一个常数对谁都不对。
//
// 44 台客户机实测（`Dumps/runtime/_reach.py`）：36 台够样本的里 **7 台有真断崖**，位置散在
// |dx| 20 / 50 / 50 / 65 / 65 / 80 / 80；其余 29 台在各自观察范围内不掉。最硬的证据是
// **同一台物理机的不同角色**结论相反，硬件与网络已被控制住：
//   · 95C577CA51C72B0：36a48eee 断崖@50（台面 98.5%，14988 窗） vs 8ae91921 到 80 不掉（98.4%）
//   · 67A67A6FE1829AE：c23d8de9 断崖@65（99.4%）             vs 2919e145 到 80 不掉（98.2%）
//   · B9B29AE541C3AA4：e2e62cca 断崖@65（台面 87.9%）         vs 008d6f7a 到 100 不掉（93.9%）
//
// 做法：拿每个观察窗的判决（|dx| + 命中与否）在线估这条包线。
//
// ★★ 现状：**只观测、不接管**（reach_cal.cpp 的 kActuate=false）。估计值只进 combat.log 的
//    `reach_cal` 行，StationDx 恒返回 fallback。原因是拿 44 台历史日志逐窗回放（`_calsim.py`）
//    发现估计器会抖、会反向、且救不到最需要的那几台——详见 reach_cal.cpp 里 kActuate 的注释，
//    那里也写明了解除接管的前置条件。**别在满足那些条件之前把它打开。**
//
// ★ 只动站位，**不动出刀闸**。收紧出刀闸已两次实证失败（kills/min 119→109；门外刀等下一个
//   门内刀要 128ms 而平衡点仅 82~119ms，且 37%~64% 的锁整段进不了带）。详见模块设计文档。
//
// ★ 安全不对称：**没测到断崖时绝不收紧**（只可能放宽），只有真测到断崖才往里收。
//   否则冷启动或「这局没飞远过」会被误读成「射程很短」，把站位越缩越死。
//
// 线程：Feed / StationDx 都在 simple_combat 的 FSM 线程上调用；Read 可能被 UI/IPC 读，
// 故内部加锁。频率极低（≤10 次/秒），锁开销可忽略。

#include <cstdint>

namespace x::features::simple_combat::reach {

// 喂一个观察窗的判决。absDx 取该窗**首刀**的 |dx|（与离线口径一致：
// `_faceflip.windows` 也用 w[0]）。每个窗只喂一次。
void Feed(float absDx, bool hit);

// 当前建议的横向站位目标。样本不足或没测到断崖时返回 >= fallback 的值，绝不更小。
float StationDx(float fallback);

struct Snap {
    int    samples;    // 有效样本（已衰减）
    float  plateau;    // 台面命中率 %（|dx| 最近档）
    float  edge;       // 触及边界 px；未测到断崖时 = 观察到的最远有效档上沿
    bool   cliff;      // 是否**真**测到断崖（而非样本没到那么远）
    float  stationDx;  // 当前输出
    bool   ready;
};
Snap Read();

// 换图/换武器可显式清空；不调也行——内部有指数衰减，1~3 分钟自然跟上。
void Reset();

}  // namespace x::features::simple_combat::reach
