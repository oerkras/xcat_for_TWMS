#pragma once
// fly — Classic TWMS 鼠标飞：鼠标世界点 → 旋翼 setpoint（heli_rotor 闭环）→ Attr=2
// 武装期：fly_fh_ban 禁挂台；ApplyImpact 必须放行（旁路已拆除）。
// 旋翼所有权：F6 是**抢占**方（heli::Owner::Fly），压过 F5 打怪与自动赶路。

namespace x::features::fly {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

bool IsArmed();
// 卸武装会先停旋翼，再可选 SetImpactNext(0,0) 刹残速，最后放 fhBan。
// 试修回退：XCAT_FLY_DISARM_ZERO=0 或 DLL 旁 fly_disarm_zero.off（见 fly.cpp）。
void SetArmed(bool on);

// 外部暂停（补给/测谎等）：不改 armed 开关，仅抑制 hop；解除后恢复。
void SetExternalPause(bool on);
bool IsExternallyPaused();

// 0=Impact NockBack  1=Impact SetImpactNext
// ⚠️ 换旋翼后对**飞行**已不起作用（冲量路由由 A 层内部决定）。保留只为面板/存档兼容。
void SetMode(unsigned mode);
unsigned GetMode();

// 目标点刷新间隔（历史名 HopCd）。旧实现里它是发冲量的节奏；现在冲量节奏由旋翼自控，
// 这里只管「多久重算一次鼠标对应的世界点」（STW 要走主线程 job，不能每拍做）。
void SetHopCdMs(unsigned ms);
unsigned GetHopCdMs();

// F6 手动飞的速度倍率（百分比，100 = 基准 1.0X）。与 F5 打怪那份**各存各的**：
// 旧 F6 开环等效约 1600 px/s，而 Cruise 基准只有 620，共用一个旋钮必有一方别扭。
// 边界与「哪些量跟随倍率、哪些不跟」见 heli_rotor.h 的 SetSpeedScale 注释。
void SetSpeedPct(unsigned pct);
unsigned SpeedPct();

}  // namespace x::features::fly
