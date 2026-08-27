#pragma once
#include <Windows.h>
#include <cstdint>

namespace x {
namespace features {
namespace auto_enter {

// Login auto-enter: world (panel) → random non-full channel → char slot → confirm.
// UI-semantic calls only; shared main_thread_pump (no GA .text).
void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetDesired(bool on, int32_t worldId, const char* worldName, uint32_t charSlot);
bool IsDesired();

// Soft-relogin / 踢线回登录后：若自动进仍开着，从 Done/Failed 拉回 Idle 重跑选区选角。
// why=="soft_login" 时开快轨：Connected/点区/选频/离频/选角 settle 压到最短、
// **仅该轮**优先粘 sticky（不可用则候选池随机，不就近/不偏人少）；
// 频道 UI 已武装到 sticky 则跳过 SelectChannel 直 GoWorld。
// why=="char_ui"：Done/Failed 后现采到选角页仍在，只解锁重跑（不开 softFast、
// 不 CloseSession、不换频）。WaitWorldList 见选角则跳过分区/频道。
// worker 可调；下一拍 Tick 会进 WaitWorldList。
// 若选角页已在（avatars>0 且 loginPhase=2），跳过频道 resume/GoWorld，直进选角。
void RequestRestart(const char* why);

// 记下应粘回的频道：SelectChannel / 登录列表 ChannelId / WM+0x6C 同口径（不是 UI ch.N）。
// BIN 08-15：id=39 时玩家看到 頻道 40。软重连 PickSticky 用此 id 调 SelectChannel。
void NoteStickyChannel(int channelId1Based, const char* why);
// 当前 sticky（列表 id）；未设置返回 0。标题栏显示须 +1。
int StickyChannel1Based();

// 当前是否停在 Failed（软重进可据此早退，不必空等到 play-ready 超时）。
bool IsFailed();

// WaitCharSelect 相位超时次数（跨 RequestRestart 保留；进图 Done / 关自动进 清零）。
// 软路径：avatars>0 满 2 次先 CloseSession 再连；avatars=0 视为登录作废，禁止再拆。
int CharSelectTimeoutStreak();

// 最近一次选角页快照的角色数。avatars=0 + Notice =「已登出登入的帳號」。
int LastCharAvatarCount();
// 选角 UI 是否在（世界/频道页 avatarCount 也是 0，不能单凭头像数判登出）。
bool CharUiVisible();

// 选角页已稳定且名单空（新号未建角色）。自动进停手等玩家创建，禁止再 GoWorld / 软重连。
// BIN 22:42 6E96607D：avatars=0 超时 Failed → 8s char_ui → GoWorld 拆会话 → dismiss 弹窗循环。
bool IsWaitingCreateChar();

// 选角链路已收尾（Done）。软重进用来发现「Done 了但迟迟不 play-ready」的卡死，
// 避免空等到 reenter_timeout（dcaf08：Done@01:16:21 → 仍 playReady=0 直到 01:18:11 fail）。
bool IsDone();

// soft 泵采样：分区列表是否真有条目（或频道页已挂选中世界可续进）。
// 仅指针非空不算 ready（BIN 21:44：world/ch 壳在、WorldItems=0 → 卡大厅）。
struct SoftHallCtx {
    int ok = 0;              // 泵上采完
    int worldUi = 0;
    int channelUi = 0;
    int worldItems = 0;      // UILoginWorld.WorldItems.Count
    int selectedWorld = 0;   // 频道页 selectedWorld 非空
    int ready = 0;           // worldItems>0 || (channelUi && selectedWorld)
};
void SoftHallSampleOnPump(void* user);  // user = SoftHallCtx*

// softFast 卡在 WaitWorldList 且 WorldItems 一直空：超过 minAgeMs 则 true（软重连早 soft cycle）。
bool IsWorldItemsStarve(DWORD minAgeMs);

}  // namespace auto_enter
}  // namespace features
}  // namespace x
