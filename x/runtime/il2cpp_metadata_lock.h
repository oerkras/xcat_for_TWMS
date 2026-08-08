// Classic TWMS — il2cpp 全局元数据锁：读状态 + 泄漏归还。
#pragma once

#include <cstdint>

namespace x::runtime::il2cpp_metadata_lock {

struct State {
    bool read = false;       // 三个全局都读到了
    bool plausible = false;  // 读数通过合理性校验（RVA 没漂）
    uint32_t word = 0;       // 0=空闲 1=已持有 2=已持有且有等待者
    uint64_t ownerTid = 0;   // 0 = 无人持有
    uint32_t recursion = 0;  // 重入计数
};

State Read();

// 从 il2cpp 内部被 SEH 异常「弹」出来之后调用。若锁此刻仍挂在本线程名下，说明
// 展开时跳过了 il2cpp 的解锁代码——把锁归还并唤醒等待者，返回 true。
// 没漏、或读数不可信时什么都不做并返回 false。
bool ReleaseIfOwnedByCurrentThread(const char* where);

// 看门狗兜底：主泵已经僵死，且这把锁自始至终被同一个「非主泵」线程占着。正常的元数据
// 查找是微秒级，持续十几秒只可能是泄漏（某个 __except 把 il2cpp 内部的异常吞了）。
// 仅当当前持有者仍等于 ownerTid 时才动手，把永久黑屏降级成一次卡顿。
bool ForceReleaseIfOwnedBy(uint32_t ownerTid, const char* why);

// ——— 定点抢修（比上面那条「主泵僵死」兜底快一个数量级）———
// il2cpp_fault_probe 在 VEH 里一看到「本线程持着锁却抛了异常」就调 Note 挂号。若那次异常
// 被正常处理（il2cpp 自己解锁，或某个 __except 里的归还屏障生效），锁会在微秒级还回来，
// 挂号自然作废；反之过了宽限期锁还挂在同一个 tid 名下，就是实锤泄漏。
//
// 2026-08-09 06:16 实测这条链：tid 35744 在同一毫秒里连抛 4 次，recursion 0→1→2→3，
// 每次 __except 吞掉就漏一层，11 秒后主泵僵死、全屏黑。有了它，黑屏窗口从十几秒收到一秒
// 出头。Note 只做两个原子写，可以安全地在 VEH 里调。
void NoteExceptionWhileOwned(uint32_t tid);
bool RepairAfterExceptionIfStillHeld(uint32_t graceMs);

}  // namespace x::runtime::il2cpp_metadata_lock
