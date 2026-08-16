#pragma once

// 禁挂台：挡 VecCtrl.CollisionDetect / CollisionDetectFloat 重挂。
// 锚点：VecCtrl_WorkUpdateActive 读 klass+0x208 / +0x218（IDA remount 2026-08-07）。
// 仅过滤 LocalUser 的 VecCtrl。
// 多源 OR：F6 飞天 / F5 Impact 贴怪 / 超级赶路贴门 / 吸怪寻簇可同时持有；任一方仍要则 BAN 保持。

namespace x::features::ports::fly_fh_ban {

enum class BanSource : unsigned {
    Fly = 1u << 0,           // F6 面板/快捷键武装
    CombatImpact = 1u << 1,  // F5 Impact 贴怪挂机期
    Travel = 1u << 2,        // 超级赶路 Impact 贴门冲量期（下穿台面）
    Gather = 1u << 3,        // 吸怪「先飞到最密堆」寻路期（选项开才用）
};

// source 开/关；mask 从 0→非 0 时 Ensure+卸台，非 0→0 时放行。
void SetSourceArmed(BanSource source, bool on);

// 兼容：等价 SetSourceArmed(BanSource::Fly, armed)。
void SetArmedBan(bool armed);

// 仅装 CollisionDetect/Float 钩，不抬 BAN、不卸台。
// 把「改虚表」从「0→开 detach」拆开，避免 F5 热开首刀与首次安装同拍崩窗。
bool WarmInstall();

bool IsBanActive();
bool IsInstalled();
unsigned ActiveMask();

void Shutdown();

}  // namespace x::features::ports::fly_fh_ban
