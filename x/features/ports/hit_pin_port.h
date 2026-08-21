#pragma once
// hit_pin_port — Classic TWMS · 打中换怪时指定 FindHitMobInRect 的 wishMobId
//
// 真源：Dumps/runtime/out/dump.cs MobPool.c2f16606… = FindHitMobInRect
//   RVA 0xF89C60（runtime IDB imagebase 0x7FFD60880000）
// 默认关：wishOid=0 时钩子原样转发。开启时原函数仍按攻击盒扫描（不改 wishMobId），
// 再把列表滤成锁 oid；盒内没有则清空（空刀），不把邻居塞进环。

#include <cstdint>

namespace x::features::ports::hit_pin {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

// 面板「每只怪打几刀」打开时置 true：首次开启才下 .text 钩（之后留着，关模式只清 wish）。
void SetWanted(bool on);
bool IsArmed();

// map_attack 等旁路：只要 true 就装同一 FindHit 钩。关只停回调，不摘 E9（与 SetWanted 相同）。
void SetAuxWanted(bool on);

// FindHit 入参旁路。默认 nullptr。P2 map_attack 改 Rect（不抬 maxCount）。
// 调用线程 = TryDoing 主泵。before 在原函数之前，after 在原函数之后、PinListToOid 之前。
using BeforeFindHitFn = void (*)(void* rect, int32_t* maxCount, int32_t startIndex);
using AfterFindHitFn = void (*)(void* rect, void* mobsRef, int32_t n, int32_t maxCount,
                                int32_t startIndex);
void SetBeforeFindHit(BeforeFindHitFn fn);
void SetAfterFindHit(AfterFindHitFn fn);

// 锁目标 oid；0 = 不改官方列表。战斗 worker 每 tick 同步。
void SetWishOid(int32_t oid);
int32_t WishOid();

}  // namespace x::features::ports::hit_pin
