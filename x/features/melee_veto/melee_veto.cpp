// 经典版 TWMS —— 近战不挥拳（melee_veto）
//
// 锚点（runtime IDB `Dumps/runtime/GameAssembly.dll.i64`，imagebase 0x7FF848C80000）：
//
// | RVA | 符号 | 托管参数 | 原生参数 |
// |---|---|---|---|
// | `0x105B0A0` | `UserLocal_TryDoingMeleeAttack` | `(skill, int, ref Nullable<int>, int, int, int, <class>)` = 7 | 9 |
// | `0x10705f0` | `UserLocal_TryDoingShootAttack` | `(skill, int, Nullable<int>, bool, int, uint)` = 6 | 8 |
//
// ★ 这两个形状**极易对调**：被拆掉的 pointblank_shoot 就是把 6 参形状套在 0x105B0A0 上，
//   于是 `methodInfo` 被塞进第 7 参的位置、真 methodInfo 丢失，把射击路径整体打歪
//   （体感「基本必挥弓」）。改这里之前先用 `Dumps/runtime/out/dump.cs` 按 RVA 复核形状。
//
// 射击函数的两个调用点决定了本模块的判决（xrefs 实测）：
//
// | 调用点 | 所在函数 | 计数器 |
// |---|---|---|
// | `0x7FF849CCA515` | `UserLocal_TryDoingMeleeAttack` 内部 | `nest` |
// | `0x7FF849CEC616` | `sub_7FF849CEB600` 普攻分发器的兜底 | `top` |
//
// ★★ 为什么必须先测量再拦截
//
// 弓上已经把「直接让近战返回 false」走到底并证伪：挥弓确实没了，但**怪完全不掉血**，
// 只有客户端飘伤害数字；远距目标（`d=(-1044,181)`）一样不掉血，所以不是贴身裁剪。
// 根因是弓的普攻伤害本来就由近战体内 `0x7FF849CCA515` 那一发射击产生并返回 true——
// 判负等于把伤害源本身掐掉（详见 ARCHER_SHOOT_VS_BONK_GATE_20260809.md §0″/§0‴）。
//
// 所以本模块**只拦投掷飞镖（`WeaponType.ThrowingGlove = 47`）**，白名单硬门。这是本模块
// 唯一的安全依据：弓/弩/短剑等根本进不了拦截路径，上面那种事故在结构上不可能重演。
//
// | 武器 | 行为 |
// |---|---|
// | 飞镖 47 | 普攻的近战那一发直接判负，零观测（`nest=0` 已由 2026-08-12 两轮 BIN 坐实） |
// | 其余 | **绝不拦**，照常转发，只顺手记 `nest` / `top` |
//
// 观测那条路现在只服务于「补上没人量过的武器」——弓的 `nest` 至今只有从失败实验反推的
// 推断值，拿弓勾一次就能拿到实测。判决（`safe`/`unsafe`）随之退化成纯记录，不影响行为。
//
// ⚠ 游戏更新后若飞镖的普攻改走近战体内，本模块**不会自动发现**（零观测的代价）。届时的
//   表现是普攻只飘数字不掉血；技能输出不受影响，因为技能路径从不进这个钩子。挂钩前的
//   12 字节 prologue 比对能挡住「函数搬家」，但挡不住「函数还在、语义变了」。
//
// 注意：模块内看不到「服务端认不认伤害」。`safe` 只保证伤害源不在近战体内，最终仲裁
// 仍然是 combat.log 里 `hit_probe` / `uihp_probe` 的怪物血量。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "melee_veto.h"

#include "../../runtime/anchor_lamps.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace x::features::melee_veto {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaTryDoingMeleeAttack = 0x105B0A0;
constexpr uint32_t kRvaTryDoingShootAttack = 0x10705f0;
constexpr uint32_t kRvaGetWeaponType = 0x142A660;

// ── 取框探针（一次性调研，`XCAT_MELEE_RECT_PROBE=1` 才挂）──────────────────────
//
// 「贴脸就挥拳」的判据不在分发器里：分发器无条件先调近战，近战**找不找得到怪**就是全部。
// 找怪用的框由 `sub_7FF849EA8290` 产出，两条出框路径都在可写内存里，没有一条编进代码：
//
//   arg4 == 55  → 直接搬 .rdata 的常量框 (-88, -6, 70, 56)，全模块仅此一处 xref
//   否则        → 拿 arg4 当 key 去托管集合里查
//
// 55 是实算出来的：IMM 0xD3E7CA30 + 种子 [0x7FF84F4D9F74]=0x2C183607 = 0x1_00000037 → 0x37。
// （按仓规逐处实读；这里按 0 读会把分支判反。它不是武器类型——枚举最大是 Gun=49。）
//
// 探针要回答的就一件事：飞镖普攻那一发走的是哪条、arg4 实际是几、框实际多大。
constexpr uint32_t kRvaGetAttackRect = 0x1239610;  // sub_7FF849EA8290
constexpr uint32_t kRvaConstRect = 0x4654800;      // xmmword_7FF84E29AF80
constexpr int kRectKindConstPath = 55;

// 近战 / 射击开头一致：push rbp / r15 / r14 / r13 / r12 / rsi / rdi / rbx = 12 字节。
constexpr uint8_t kProlog[12] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41,
                                 0x55, 0x41, 0x54, 0x56, 0x57, 0x53};
// 取框函数是另一套：push r14/rsi/rdi/rbx + sub rsp,0B8h，同样正好 12 字节且指令对齐。
constexpr uint8_t kPrologRect[12] = {0x41, 0x56, 0x56, 0x57, 0x53, 0x48,
                                     0x81, 0xEC, 0xB8, 0x00, 0x00, 0x00};
constexpr size_t kSteal = 12;

// 本功能只服务投掷飞镖的盗贼。`WeaponType.ThrowingGlove = 47`（`Dumps/cms_cw/dump.cs:79131`）
// 是**唯一**被授权拦截的武器：白名单而非黑名单，弓/弩/短剑等根本进不了拦截路径，
// §0″ 那种「拦了把伤害源一起掐掉」的事故在结构上就不可能重演。
constexpr int kWtThrowingGlove = 47;
constexpr int kBodyPartWeapon = 11;
constexpr int kInvTiEquip = 1;
constexpr size_t kFbCdEquipped = 0x28;
constexpr size_t kFbCdEquipped2 = 0x30;
// 缓存窗口直接决定「换武器后还有多久可能拿旧武器判拦截」。飞镖换弓那一瞬如果还按 47 拦，
// 就是白扔一发伤害，所以压到一次普攻间隔（~600ms）以内。
constexpr DWORD kWeaponCacheMs = 120;
constexpr DWORD kHeartMs = 5000;
// 判决只问一件事：**近战体内会不会自己转调射击**（nest）。拦截已由白名单决定，所以它现在
// 只是观测结论的一个标签，写进日志给人看，不参与任何分支。
//
// 「有结论的样本」= 近战真的干了活的那一发（返回 true，或体内转调过射击）。返回 false
// 又没 nest 的调用什么都不说明（人离怪远，近战本来就该失败），不能拿它凑数。
constexpr uint32_t kVerdictConclusive = 6;
// 兜底：刷了这么多次近战调用还一个有结论的都没有，说明近战压根不参与普攻。
constexpr uint32_t kVerdictMaxSamples = 120;

using FnMelee = uint8_t(__fastcall*)(void* self, void* skill, int32_t skillLevel,
                                     void* shootRangeRef, int32_t a5, int32_t a6, int32_t a7,
                                     void* a8, void* methodInfo);
using FnShoot = uint8_t(__fastcall*)(void* self, void* skill, int32_t skillLevel,
                                     uint64_t shootRange, int32_t isMortalBlow, int32_t timeKeyDown,
                                     uint32_t randMb, void* methodInfo);
using FnGetWeaponType = int(__fastcall*)(int itemId, void* methodInfo);
// 入口把参数搬得很干净：rsi=out、r14=arg2、rbx=arg3、edi=arg4；返回 rax=rsi。
using FnRect = void*(__fastcall*)(void* outRect, void* obj, void* a3, int32_t a4, int32_t a5);

enum class Verdict { Measuring, Safe, Unsafe };

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

// gWant = 功能开关；gHooksWanted = 钩子该不该在。两者**故意分开**。
//
// 早先是绑在一起的：关开关就卸钩、开开关再挂钩。代价是每次「开」都要等一次 PumpApply
// 往返，那段窗口里贴脸普攻照常挥拳——2026-08-12 15:17 的 BIN 就是这么漏过去 4 发的
// （战斗 15:17:54.975 开打，钩子 15:17:57.934 才挂上）。
//
// 现在钩子一旦挂上就留着，只有 Shutdown 才卸；关开关退化成一次原子写，下一发普攻立刻
// 生效、不可能失败。关着的时候 HookMelee 是纯转发，代价可以忽略。
std::atomic<bool> gWant{false};
std::atomic<bool> gHooksWanted{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
AbsHookState gMelee{};
AbsHookState gShoot{};
FnMelee gMeleeTramp = nullptr;
FnShoot gShootTramp = nullptr;
std::atomic<bool> gMeleeRefuse{false};
std::atomic<bool> gShootRefuse{false};
AbsHookState gRect{};
FnRect gRectTramp = nullptr;
std::atomic<bool> gRectRefuse{false};

// 标记文件存在就挂探针。用文件而不是环境变量：环境变量得把启动器和客户端整套重开才生效，
// 标记文件可以在游戏已经跑着的时候放下去，再把开关关一次开一次就挂上了。
bool RectProbeRequested() {
    const char* bin = x::runtime::GetBinDir();
    if (!bin || !*bin) return false;
    std::string p = bin;
    if (!p.empty() && p.back() != '\\' && p.back() != '/') p.push_back('\\');
    p += "state\\melee_rect_probe";
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 探针记账：只记「不同的」(kind, 框)，不按发计流水。
std::atomic<uint32_t> gRectCalls{0};

// 放宽后的观测：按 (kind, 是否处在普攻近战帧) 分槽，每个组合只打印一行。
// 要判断的就是「哪些 action id 是技能在用的」——只在普攻帧里出现过的 id 才可能可以归零，
// 帧外（技能等其它调用方）出现过的一律不能动。分槽去重是为了看全又不按发刷屏。
// 钩子跑在游戏线程上，里面只写原子，绝不写日志；由 worker 排空后打印。
constexpr int kRectKindSlots = 64;
struct RectSlot {
    // 0=未见 1=待打印 2=已打印 3=正在填（worker 必须跳过，别读到半张框）
    std::atomic<uint32_t> state{0};
    std::atomic<uint32_t> f[4]{};
    std::atomic<uint32_t> hits{0};
};
RectSlot gRectSlot[2][kRectKindSlots];  // [0]=非普攻帧  [1]=普攻帧内
std::atomic<int> gRectKindWide{-1};     // kind 落在槽位外时留个样本，别静默丢掉

// 普攻（skill==null）口径的计数器。技能攻击一律原样转发、不计数、不拦。
std::atomic<uint32_t> gMCall{0};    // 近战被调用
std::atomic<uint32_t> gMTrue{0};    // 近战返回 true（= 真挥了）
std::atomic<uint32_t> gMFalse{0};   // 近战返回 false（分发器会落到兜底射击）
std::atomic<uint32_t> gNest{0};     // 近战体内转调射击 —— 判决的关键
std::atomic<uint32_t> gTop{0};      // 分发器兜底射击
std::atomic<uint32_t> gConclusive{0};  // 有结论的样本：近战真的干了活（返回 true 或转调过射击）
std::atomic<uint32_t> gVetoHits{0};
std::atomic<int> gVerdict{static_cast<int>(Verdict::Measuring)};
std::atomic<int> gLastWt{0};
std::atomic<int> gLastJob{0};
std::atomic<int> gVerdictWt{0};   // 判决是在哪把武器上做的；换武器要重测

// 只在普攻的那一发近战里加深。技能路径不计，免得把技能的内部射击算成 nest。
thread_local int gNormalMeleeDepth = 0;

void ResetCounters() {
    gMCall.store(0, std::memory_order_relaxed);
    gMTrue.store(0, std::memory_order_relaxed);
    gMFalse.store(0, std::memory_order_relaxed);
    gNest.store(0, std::memory_order_relaxed);
    gTop.store(0, std::memory_order_relaxed);
    gConclusive.store(0, std::memory_order_relaxed);
    gVetoHits.store(0, std::memory_order_relaxed);
    gVerdict.store(static_cast<int>(Verdict::Measuring), std::memory_order_relaxed);
}

int ReadI32(void* obj, size_t off) {
    int v = 0;
    __try {
        v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

void* ArrayAtPtr(void* arr, int index) {
    if (!LooksLikeHeapPtr(arr) || index < 0) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    int len = 0;
    __try {
        len = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(arr) + offLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (index >= len) return nullptr;
    void* elem = nullptr;
    __try {
        elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + offData +
                                         static_cast<size_t>(index) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(elem) ? elem : nullptr;
}

void* ListAt(void* list, int index) {
    if (!LooksLikeHeapPtr(list) || index < 0) return nullptr;
    x::runtime::il2cpp_container::Ensure();
    x::runtime::il2cpp_container::RefineFromListInstance(list);
    const size_t offItems = x::runtime::il2cpp_container::OffListItems();
    const size_t offSize = x::runtime::il2cpp_container::OffListSize();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    void* items = nullptr;
    int size = 0;
    __try {
        items = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(list) + offItems);
        size = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(list) + offSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!LooksLikeHeapPtr(items) || index >= size) return nullptr;
    void* elem = nullptr;
    __try {
        elem = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(items) + offData +
                                         static_cast<size_t>(index) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(elem) ? elem : nullptr;
}

int WeaponTypeFromItemId(int itemId) {
    if (itemId < 1000000) return 0;
    const int t = (itemId / 10000) % 100;
    if (t >= 30 && t <= 49) return t;
    return 0;
}

int CallGetWeaponType(int itemId) {
    if (itemId <= 0) return 0;
    // 托管方法只许在主泵上；worker 调会点 Unity GC「Collecting from unknown thread」。
    if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
        return 0;
    auto fn = x::runtime::il2cpp::AtRva<FnGetWeaponType>(kRvaGetWeaponType);
    if (!fn) return 0;
    int wt = 0;
    __try {
        wt = fn(itemId, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wt = 0;
    }
    return (wt >= 30 && wt <= 49) ? wt : 0;
}

int SlotWeaponType(void* slot) {
    if (!LooksLikeHeapPtr(slot)) return 0;
    const size_t offId = x::ui::player::OffSlotItemId();
    if (!offId) return 0;
    const int id = ReadI32(slot, offId);
    if (id <= 0) return 0;
    const int fromId = WeaponTypeFromItemId(id);
    return fromId ? fromId : CallGetWeaponType(id);
}

int ReadEquippedWeaponType() {
    void* cd = x::ui::player::LocalCharacterData();
    if (LooksLikeHeapPtr(cd)) {
        const size_t eqOffs[2] = {kFbCdEquipped, kFbCdEquipped2};
        for (int ei = 0; ei < 2; ++ei) {
            void* arr = ReadPtr(cd, eqOffs[ei]);
            if (!LooksLikeHeapPtr(arr)) continue;
            const int wt = SlotWeaponType(ArrayAtPtr(arr, kBodyPartWeapon));
            if (wt) return wt;
        }
    }
    void* list = x::ui::player::GetItemSlotList(kInvTiEquip);
    if (!LooksLikeHeapPtr(list)) return 0;
    const int preferIdx[2] = {kBodyPartWeapon, kBodyPartWeapon + 1};
    for (int i = 0; i < 2; ++i) {
        const int wt = SlotWeaponType(ListAt(list, preferIdx[i]));
        if (wt) return wt;
    }
    return 0;
}

// 钩子跑在游戏主线程（= 泵线程）上，所以这里读装备是合规的；仍然做短缓存，
// 别让每一刀都去走一遍 equip 数组 + 托管 GetWeaponType。
int WeaponTypeCached() {
    static DWORD sLastMs = 0;
    static int sWt = 0;
    const DWORD now = GetTickCount();
    if (sLastMs && (now - sLastMs) < kWeaponCacheMs) return sWt;
    if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
        return sWt;
    int wt = 0;
    __try {
        wt = ReadEquippedWeaponType();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        wt = 0;
    }
    sLastMs = now ? now : 1;
    sWt = wt;
    gLastWt.store(wt, std::memory_order_relaxed);
    x::ui::player::Vitals v{};
    if (x::ui::player::Read(v) && v.ok) gLastJob.store(v.job, std::memory_order_relaxed);
    return wt;
}

// 判决只落一次；换武器会在 heart 里清掉重测。
//
// 只在**非白名单**武器上跑（白名单武器根本不执行近战，无从观测），结论纯记录、不改行为。
// 拿它去补弓的 `nest` 实测：日志里出现 `wt=45 ... nest>0` 就坐实了 §0‴′ 那条推断。
void MaybeSettleVerdict(int wt, bool conclusive) {
    if (gVerdict.load(std::memory_order_relaxed) != static_cast<int>(Verdict::Measuring)) return;
    if (conclusive) gConclusive.fetch_add(1, std::memory_order_relaxed);

    const uint32_t nConc = gConclusive.load(std::memory_order_relaxed);
    const uint32_t nCall = gMCall.load(std::memory_order_relaxed);
    if (nConc < kVerdictConclusive && nCall < kVerdictMaxSamples) return;

    const uint32_t nest = gNest.load(std::memory_order_relaxed);
    const Verdict v = nest > 0 ? Verdict::Unsafe : Verdict::Safe;
    gVerdict.store(static_cast<int>(v), std::memory_order_relaxed);
    gVerdictWt.store(wt, std::memory_order_relaxed);

    x::runtime::LogI("MeleeVeto",
                     "观测结论 wt=%d：%s（nest=%u，有结论样本 %u / 总调用 %u）。"
                     "本武器不在拦截白名单，行为不变。",
                     wt, v == Verdict::Unsafe ? "unsafe：伤害源在近战体内，这种武器拦不得" : "safe",
                     nest, nConc, nCall);
}

// 拦不拦只看开关 + 武器。判决不参与——飞镖的 `nest=0` 已由 2026-08-12 两轮 BIN 坐实
// （40 样本一次、6 样本一次，767 次拦截伤害全程正常），不必每次开启再花一发普攻确认。
//
// 这里读 gWant 是关键：钩子常驻之后，「关掉功能」就只剩这一次原子读，卸钩失不失败都
// 不影响正确性。
bool VetoNow(int wt) {
    return gWant.load(std::memory_order_relaxed) && wt == kWtThrowingGlove;
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool SigMatch(void* target, const uint8_t* sig, size_t n) {
    if (!target) return false;
    for (size_t i = 0; i < n; ++i) {
        if (reinterpret_cast<uint8_t*>(target)[i] != sig[i]) return false;
    }
    return true;
}

bool InstallAbs(AbsHookState& st, void* target, void* hook, const uint8_t* prolog) {
    if (st.active) return true;
    if (!target || !hook) return false;
    // 这道检查要用**调用方那个** prolog。写死 kProlog 的话，prologue 不同的目标会在这里被
    // 静默判掉（外层 TryArmOne 已经放行、这里 return false 又不打日志），查起来毫无线索。
    if (!SigMatch(target, prolog, kSteal)) return false;
    void* tramp =
        VirtualAlloc(nullptr, kSteal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(st.saved, target, kSteal);
    memcpy(tramp, target, kSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kSteal,
                reinterpret_cast<uint8_t*>(target) + kSteal);
    DWORD old = 0;
    if (!VirtualProtect(target, kSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hook);
    FlushInstructionCache(GetCurrentProcess(), target, kSteal);
    VirtualProtect(target, kSteal, old, &old);
    st.target = target;
    st.trampoline = tramp;
    st.stolen = kSteal;
    st.active = true;
    return true;
}

void RemoveAbs(AbsHookState& st) {
    if (!st.active || !st.target) return;
    DWORD old = 0;
    if (VirtualProtect(st.target, st.stolen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(st.target, st.saved, st.stolen);
        FlushInstructionCache(GetCurrentProcess(), st.target, st.stolen);
        VirtualProtect(st.target, st.stolen, old, &old);
    }
    // trampoline 故意不 VirtualFree：卸钩这一刻可能还有别的线程正停在里面，释放了就是
    // use-after-free。钩子只在 Shutdown 卸一次，整个进程最多漏掉两块 28 字节。
    st.trampoline = nullptr;
    st.target = nullptr;
    st.stolen = 0;
    st.active = false;
}

// 只测量，绝不改参数。nest / top 的区分全靠 gNormalMeleeDepth。
uint8_t __fastcall HookShoot(void* self, void* skill, int32_t skillLevel, uint64_t shootRange,
                             int32_t isMortalBlow, int32_t timeKeyDown, uint32_t randMb,
                             void* methodInfo) {
    if (gNormalMeleeDepth > 0)
        gNest.fetch_add(1, std::memory_order_relaxed);
    else if (!skill)
        gTop.fetch_add(1, std::memory_order_relaxed);
    const FnShoot o = gShootTramp;
    if (!o) return 0;
    __try {
        return o(self, skill, skillLevel, shootRange, isMortalBlow, timeKeyDown, randMb, methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 纯观测：转发后把这一发实际拿到的框记下来。全部调用方都记（另外两个函数也调它），
// 靠 gNormalMeleeDepth 分成「普攻帧内 / 帧外」两组，这样技能占用了哪些 kind 一目了然。
void* __fastcall HookRect(void* outRect, void* obj, void* a3, int32_t a4, int32_t a5) {
    const FnRect o = gRectTramp;
    if (!o) return outRect;
    void* r = nullptr;
    __try {
        r = o(outRect, obj, a3, a4, a5);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return outRect;
    }
    if (!outRect) return r;
    gRectCalls.fetch_add(1, std::memory_order_relaxed);

    const int ctx = gNormalMeleeDepth > 0 ? 1 : 0;
    if (a4 < 0 || a4 >= kRectKindSlots) {
        gRectKindWide.store(a4, std::memory_order_relaxed);
        return r;
    }
    RectSlot& s = gRectSlot[ctx][a4];
    s.hits.fetch_add(1, std::memory_order_relaxed);
    uint32_t unseen = 0;
    if (!s.state.compare_exchange_strong(unseen, 3, std::memory_order_acq_rel,
                                         std::memory_order_relaxed))
        return r;  // 这个组合记过了，只累 hits
    uint32_t f[4] = {0, 0, 0, 0};
    __try {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(outRect);
        f[0] = p[0];
        f[1] = p[1];
        f[2] = p[2];
        f[3] = p[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s.state.store(0, std::memory_order_release);  // 还回未见，下一发再试
        return r;
    }
    for (int i = 0; i < 4; ++i) s.f[i].store(f[i], std::memory_order_relaxed);
    s.state.store(1, std::memory_order_release);
    return r;
}

uint8_t __fastcall HookMelee(void* self, void* skill, int32_t skillLevel, void* shootRangeRef,
                             int32_t a5, int32_t a6, int32_t a7, void* a8, void* methodInfo) {
    const FnMelee o = gMeleeTramp;
    if (!o) return 0;
    // 技能攻击原样放行：本功能只管普攻贴脸那一发。
    if (skill) {
        __try {
            return o(self, skill, skillLevel, shootRangeRef, a5, a6, a7, a8, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // 开关关着：纯转发。钩子常驻不卸（见 gHooksWanted），这一步就是「关掉」的全部代价。
    //
    // 例外是挂了取框探针的时候：要观测的正是「自然挥拳」那一发，得先把开关关掉让近战真的
    // 跑起来，所以这条路也要维护 depth，否则 HookRect 认不出哪次取框属于普攻。
    if (!gWant.load(std::memory_order_relaxed)) {
        const bool probing = gRect.active;
        if (probing) ++gNormalMeleeDepth;
        uint8_t r = 0;
        __try {
            r = o(self, skill, skillLevel, shootRangeRef, a5, a6, a7, a8, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            r = 0;
        }
        if (probing) --gNormalMeleeDepth;
        return r;
    }

    const int wt = WeaponTypeCached();
    if (VetoNow(wt)) {
        gVetoHits.fetch_add(1, std::memory_order_relaxed);
        return 0;  // 分发器据此落到 0x7FF849CEC616 的兜底射击
    }

    // 拦不动的武器走这条：照常转发，只是顺手把 nest / top 量下来。
    // 弓的 `nest` 至今没有任何一条实测记录（§0‴′ 那张表是从失败实验反推的），拿弓勾一次
    // 这个开关就能补上——拦截是白名单硬门，观测期间不会动弓的任何一发普攻。
    gMCall.fetch_add(1, std::memory_order_relaxed);
    const uint32_t nestBefore = gNest.load(std::memory_order_relaxed);
    ++gNormalMeleeDepth;
    uint8_t r = 0;
    __try {
        r = o(self, skill, skillLevel, shootRangeRef, a5, a6, a7, a8, methodInfo);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 0;
    }
    --gNormalMeleeDepth;
    const bool nested = gNest.load(std::memory_order_relaxed) != nestBefore;
    if (r)
        gMTrue.fetch_add(1, std::memory_order_relaxed);
    else
        gMFalse.fetch_add(1, std::memory_order_relaxed);
    MaybeSettleVerdict(wt, r != 0 || nested);
    return r;
}

void ReportLamp(x::runtime::anchor_lamps::AnchorLampCode code, const char* detail) {
    x::runtime::anchor_lamps::Set("MeleeVeto", code, detail);
}

bool TryArmOne(AbsHookState& st, std::atomic<bool>& refuse, uint32_t rva, void* hook,
               const char* name, void** outTramp, const uint8_t* prolog = kProlog,
               bool giveUpOnInstallFail = false) {
    if (st.active || refuse.load(std::memory_order_relaxed)) return st.active;
    void* target = x::runtime::il2cpp::AtRva<void*>(rva);
    if (!SigMatch(target, prolog, kSteal)) {
        refuse.store(true, std::memory_order_relaxed);
        x::runtime::LogW("MeleeVeto", "%s refuse: prolog mismatch @%p rva=0x%X b0=%02X", name,
                         target, (unsigned)rva,
                         target ? reinterpret_cast<uint8_t*>(target)[0] : 0);
        return false;
    }
    if (!InstallAbs(st, target, hook, prolog)) {
        // 只补日志，不改重试语义：近战 / 射击原本就是「挂不上就下一轮再试」，那是本功能的
        // 既有行为，不该为了探针方便顺手改掉。只有探针自己 giveUp，免得它每秒重试刷日志。
        if (giveUpOnInstallFail) refuse.store(true, std::memory_order_relaxed);
        x::runtime::LogW("MeleeVeto", "%s install failed @%p rva=0x%X giveUp=%d", name, target,
                         (unsigned)rva, giveUpOnInstallFail ? 1 : 0);
        return false;
    }
    if (outTramp) *outTramp = st.trampoline;
    x::runtime::LogI("MeleeVeto", "%s arm=1 target=%p rva=0x%X", name, target, (unsigned)rva);
    return true;
}

// ── 零钩子取框链路 ────────────────────────────────────────────────────────────
//
//   ActionManager : Singleton<ActionManager>
//     └─ _afterimageMap : Dictionary<string, MeleeAttackAfterImage>   @0x40
//          └─ MeleeAttackAfterImage.Range : Dictionary<int, Rect>     @0x18
//               └─ [动作 id] → Rect(16B)   ← 近战找怪用的就是这个框
//
// 关键是不用自己去构造泛型实例化：`Singleton<ActionManager>` 就是 `ActionManager` 的父类，
// classParent 直接给出膨胀后的那个 klass，静态区随之可取。
//
// ★ 出货客户端的类名/字段名是 63 位十六进制哈希、命名空间为空，按友好名一律查不到。
//   `Dumps/cms_cw/dump.cs` 只是未混淆参照，仅可用来认结构；名字与偏移必须取自运行时
//   dump `Dumps/runtime/out/dump.cs`（2026-08-06）。两份的字段序还不一样：参照里
//   _afterimageMap 在 0x48，实机在 0x40（参照在它前面多一个字段），照抄会读到隔壁指针。
//
// 以下哈希取自运行时 dump：
//   L73846  ActionManager        TypeDefIndex 1653，父类 Singleton<ActionManager>
//   L1065361 Singleton<T>._instance  static @0x0，类型 Lazy<T>
//   L73512  MeleeAttackAfterImage TypeDefIndex 1634；L73516 Range: Dictionary<int,Rect> @0x18
constexpr char kHashActionManager[] =
    "c1a571fd586131c52f3a678bee953e76890f76d0729e198215a5239bc573442";
constexpr char kHashSingletonInstance[] =
    "b9143f0b097da8b3733c3286804b0dee7ab4ae8f39620354bc4817d33fe9a42";
constexpr size_t kOffActionMgrAfterImageMap = 0x40;
constexpr size_t kOffAfterImageRange = 0x18;

// Dictionary<K,V> 固定布局：buckets@0x10 entries@0x18 count@0x20
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
// Dictionary<int, Rect> 的 entry = hashCode(4) next(4) key(4) Rect(16) = 28。
// 与 sub_7FF84BFA7E60 反编译出的 *(_OWORD *)(entries + 28*i + 44) 对得上：44 = 数据区 0x20 + 12。
constexpr size_t kEntryStrideIntRect = 28;
constexpr size_t kEntryKeyOffIntRect = 8;
constexpr size_t kEntryValOffIntRect = 12;
// Dictionary<string, 引用> 的 entry = hashCode(4) next(4) key(ptr) value(ptr) = 24
constexpr size_t kEntryStrideStrRef = 24;
constexpr size_t kEntryKeyOffStrRef = 8;
constexpr size_t kEntryValOffStrRef = 16;

std::atomic<bool> gRangeDumped{false};

// il2cpp_bind 没绑 class_get_name，本地取一下就够，不去改共享运行时。
// 只用于诊断日志：父类/Lazy 的真实类名能一眼看出走错没走错。
using FnClassName = const char* (*)(void* klass);

const char* ClassNameOf(void* klass) {
    static FnClassName sFn = nullptr;
    static bool sTried = false;
    if (!sTried) {
        sTried = true;
        if (HMODULE ga = x::runtime::il2cpp::GameAssembly())
            sFn = reinterpret_cast<FnClassName>(GetProcAddress(ga, "il2cpp_class_get_name"));
    }
    if (!klass || !sFn) return "?";
    const char* s = nullptr;
    __try {
        s = sFn(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "!";
    }
    return s ? s : "?";
}

void ManagedStringUtf8(void* str, char* out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (!LooksLikeHeapPtr(str)) return;
    const auto& api = x::runtime::il2cpp::Get();
    if (!api.stringLength || !api.stringChars) return;
    int n = 0;
    const uint16_t* w = nullptr;
    __try {
        n = api.stringLength(str);
        w = reinterpret_cast<const uint16_t*>(api.stringChars(str));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (!w || n <= 0) return;
    if (static_cast<size_t>(n) >= cap) n = static_cast<int>(cap) - 1;
    __try {
        int j = 0;
        for (int i = 0; i < n; ++i) {
            const uint16_t c = w[i];
            out[j++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
        }
        out[j] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

// 必须在泵上跑：FindClass / RuntimeClassInit 会触发元数据解析。
//
// 每一步失败都写进 why：这条链有七步，只报一句「解析失败」的话根本分不清是导出缺了、
// 类找不到、还是单例没建成——19:42 那次就是这么白跑一轮的。
void* ResolveAfterImageMap(char* why, size_t cap) {
    if (why && cap) why[0] = '\0';
    auto note = [&](const char* s) {
        if (why && cap) _snprintf_s(why, cap, _TRUNCATE, "%s", s);
    };

    if (!x::runtime::il2cpp::Ensure()) return note("il2cpp 未就绪"), nullptr;
    const auto& api = x::runtime::il2cpp::Get();
    if (!api.classParent) return note("缺导出 class_get_parent"), nullptr;
    if (!api.classStaticData) return note("缺导出 class_get_static_field_data"), nullptr;
    if (!api.classGetFieldFromName) return note("缺导出 class_get_field_from_name"), nullptr;
    if (!api.fieldGetOffset) return note("缺导出 field_get_offset"), nullptr;
    if (!api.fieldGetType) return note("缺导出 field_get_type"), nullptr;
    if (!api.classFromType) return note("缺导出 class_from_type"), nullptr;

    void* amKlass = x::runtime::il2cpp::FindClass("", kHashActionManager);
    if (!amKlass) return note("FindClass 找不到 ActionManager（哈希名对不上，客户端可能已更新）"), nullptr;
    void* singleton = api.classParent(amKlass);  // Singleton<ActionManager>（已膨胀）
    if (!singleton) return note("ActionManager 取不到父类"), nullptr;
    x::runtime::il2cpp::RuntimeClassInit(singleton);

    void* fInstance = api.classGetFieldFromName(singleton, kHashSingletonInstance);
    if (!fInstance) {
        if (why && cap)
            _snprintf_s(why, cap, _TRUNCATE, "父类 %s 里没有 _instance 字段",
                        ClassNameOf(singleton));
        return nullptr;
    }
    void* statics = api.classStaticData(singleton);
    if (!statics) return note("父类静态区为空（cctor 还没跑）"), nullptr;
    const size_t offInst = api.fieldGetOffset(fInstance);
    void* lazy = x::runtime::il2cpp::ReadPtr(statics, offInst);
    if (!LooksLikeHeapPtr(lazy)) {
        if (why && cap)
            _snprintf_s(why, cap, _TRUNCATE, "父类 %s 的 _instance 为空 off=0x%zX",
                        ClassNameOf(singleton), offInst);
        return nullptr;
    }

    // Lazy<T> 的字段偏移在 dump 里全是 0（开放泛型没有固定布局），只能按名实解；
    // 且必须落在「膨胀后」的 Lazy<ActionManager> 上。对象头里的 klass 一定是实例化过的，
    // 比字段声明类型更可靠，取不到才退回声明类型。Lazy 来自 mscorlib，字段名没被混淆。
    void* lazyKlass = x::runtime::il2cpp::ReadPtr(lazy, 0);
    if (!LooksLikeHeapPtr(lazyKlass)) lazyKlass = api.classFromType(api.fieldGetType(fInstance));
    if (!lazyKlass) return note("_instance 的类型取不到 klass"), nullptr;
    void* fValue = api.classGetFieldFromName(lazyKlass, "_value");
    if (!fValue) {
        if (why && cap)
            _snprintf_s(why, cap, _TRUNCATE, "%s 里没有 _value 字段", ClassNameOf(lazyKlass));
        return nullptr;
    }
    void* mgr = x::runtime::il2cpp::ReadPtr(lazy, api.fieldGetOffset(fValue));
    if (!LooksLikeHeapPtr(mgr)) return note("Lazy._value 为空（单例还没被取用）"), nullptr;

    void* map = x::runtime::il2cpp::ReadPtr(mgr, kOffActionMgrAfterImageMap);
    if (!LooksLikeHeapPtr(map)) return note("ActionManager._afterimageMap 为空"), nullptr;
    return map;
}

// 纯只读：把每把武器的 Range 表整张印出来，看清有哪些动作 id 再决定动谁。
// 返回 false = 这轮还没取到，值得再试（单例/表都是懒建的）。
bool DumpAfterImageTables() {
    char why[192];
    void* map = ResolveAfterImageMap(why, sizeof(why));
    if (!map) {
        x::runtime::LogW("MeleeVeto", "afterimage 解析失败：%s", why[0] ? why : "未知");
        return false;
    }
    x::runtime::il2cpp_container::Ensure();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    void* entries = x::runtime::il2cpp::ReadPtr(map, kOffDictEntries);
    const int count = ReadI32(map, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count <= 0 || count > 4096) {
        x::runtime::LogW("MeleeVeto", "afterimage map=%p 还没建表 count=%d", map, count);
        return false;
    }
    x::runtime::LogI("MeleeVeto", "afterimage map=%p 武器数=%d", map, count);

    for (int i = 0; i < count && i < 64; ++i) {
        uint8_t* e = reinterpret_cast<uint8_t*>(entries) + offData +
                     static_cast<size_t>(i) * kEntryStrideStrRef;
        if (ReadI32(e, 0) < 0) continue;  // 空槽
        void* keyStr = x::runtime::il2cpp::ReadPtr(e, kEntryKeyOffStrRef);
        void* img = x::runtime::il2cpp::ReadPtr(e, kEntryValOffStrRef);
        char uol[96];
        ManagedStringUtf8(keyStr, uol, sizeof(uol));
        if (!LooksLikeHeapPtr(img)) {
            x::runtime::LogI("MeleeVeto", "afterimage[%d] uol=%s <null>", i, uol);
            continue;
        }
        void* range = x::runtime::il2cpp::ReadPtr(img, kOffAfterImageRange);
        void* rEnt = LooksLikeHeapPtr(range)
                         ? x::runtime::il2cpp::ReadPtr(range, kOffDictEntries)
                         : nullptr;
        const int rCount = LooksLikeHeapPtr(range) ? ReadI32(range, kOffDictCount) : 0;
        if (!LooksLikeHeapPtr(rEnt) || rCount <= 0 || rCount > 4096) {
            x::runtime::LogI("MeleeVeto", "afterimage[%d] uol=%s range 空 count=%d", i, uol,
                             rCount);
            continue;
        }
        char line[900];
        int w = 0;
        for (int k = 0; k < rCount && k < 32 && w < static_cast<int>(sizeof(line)) - 64; ++k) {
            uint8_t* re = reinterpret_cast<uint8_t*>(rEnt) + offData +
                          static_cast<size_t>(k) * kEntryStrideIntRect;
            if (ReadI32(re, 0) < 0) continue;
            const int key = ReadI32(re, kEntryKeyOffIntRect);
            float r[4] = {0, 0, 0, 0};
            __try {
                memcpy(r, re + kEntryValOffIntRect, sizeof(r));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            w += _snprintf_s(line + w, sizeof(line) - w, _TRUNCATE, "%s%d:(%.0f,%.0f,%.0f,%.0f)",
                             w ? " " : "", key, r[0], r[1], r[2], r[3]);
        }
        line[sizeof(line) - 1] = '\0';
        x::runtime::LogI("MeleeVeto", "afterimage[%d] uol=%s n=%d %s", i, uol, rCount, line);
    }
    return true;
}

void PumpDumpRange(void*) {
    if (DumpAfterImageTables()) gRangeDumped.store(true, std::memory_order_release);
}

void PumpApply(void*) {
    const bool want = gHooksWanted.load(std::memory_order_acquire);
    if (want) {
        if (!x::runtime::il2cpp::Ensure()) {
            ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "no-il2cpp");
            return;
        }
        // 先挂射击（测量端），再挂近战：反过来会有一小段窗口 nest 记不到。
        void* shootTramp = gShootTramp;
        TryArmOne(gShoot, gShootRefuse, kRvaTryDoingShootAttack,
                  reinterpret_cast<void*>(&HookShoot), "Shoot", &shootTramp);
        gShootTramp = reinterpret_cast<FnShoot>(shootTramp);
        void* meleeTramp = gMeleeTramp;
        TryArmOne(gMelee, gMeleeRefuse, kRvaTryDoingMeleeAttack,
                  reinterpret_cast<void*>(&HookMelee), "Melee", &meleeTramp);
        gMeleeTramp = reinterpret_cast<FnMelee>(meleeTramp);

        // 顺手把武器类型读出来（这里就在泵上），让心跳一开始就能报出当前武器。
        if (gMelee.active) (void)WeaponTypeCached();

        // 挂不上就每秒重试一次，日志只在真挂上那一刻打一行 —— 否则 need 会让 PumpApply
        // 每秒跑一趟，把 x.jsonl 刷爆（17:31 那批 1 秒 2 行就是这么来的）。
        if (!gRect.active && RectProbeRequested()) {
            void* rectTramp = gRectTramp;
            TryArmOne(gRect, gRectRefuse, kRvaGetAttackRect,
                      reinterpret_cast<void*>(&HookRect), "Rect", &rectTramp, kPrologRect,
                      /*giveUpOnInstallFail=*/true);
            gRectTramp = reinterpret_cast<FnRect>(rectTramp);
            if (gRect.active) {
                // 顺带把 .rdata 那个常量框读出来：既是基准值，也是 RVA 换算没算错的自证。
                float c[4] = {0, 0, 0, 0};
                __try {
                    const float* p = x::runtime::il2cpp::AtRva<const float*>(kRvaConstRect);
                    if (p) memcpy(c, p, sizeof(c));
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                x::runtime::LogI("MeleeVeto",
                                 "rect probe armed const=(%.1f, %.1f, %.1f, %.1f) "
                                 "（kind==%d 走这个常量，否则按 kind 查托管集合）",
                                 c[0], c[1], c[2], c[3], kRectKindConstPath);
            }
        }

        const bool ok = gMelee.active && gShoot.active;
        ReportLamp(ok ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                      : x::runtime::anchor_lamps::AnchorLampCode::Miss,
                   ok ? "armed" : "partial");
        x::runtime::LogI("MeleeVeto", "arm melee=%d shoot=%d wt=%d job=%d",
                         gMelee.active ? 1 : 0, gShoot.active ? 1 : 0,
                         gLastWt.load(std::memory_order_relaxed),
                         gLastJob.load(std::memory_order_relaxed));
    } else if (gMelee.active || gShoot.active || gRect.active) {
        // 先卸取框探针，再卸近战，最后卸射击：与挂载相反的次序，卸的过程里 nest / 取框
        // 都不会读到半拆的 tramp。
        RemoveAbs(gRect);
        gRectTramp = nullptr;
        RemoveAbs(gMelee);
        gMeleeTramp = nullptr;
        RemoveAbs(gShoot);
        gShootTramp = nullptr;
        x::runtime::LogI("MeleeVeto", "disarm mCall=%u mTrue=%u mFalse=%u nest=%u top=%u veto=%u",
                         gMCall.load(std::memory_order_relaxed),
                         gMTrue.load(std::memory_order_relaxed),
                         gMFalse.load(std::memory_order_relaxed),
                         gNest.load(std::memory_order_relaxed),
                         gTop.load(std::memory_order_relaxed),
                         gVetoHits.load(std::memory_order_relaxed));
        ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "off");
    }
}

// 勾上即自行放行 .text 补丁。关开关不撤环境变量：无限飞镖和近战不挥拳共用这根旗，
// 一方关掉若清掉，另一方会整段拒绝下钩（01:17 BIN：两边同时「须先设 XCAT_ALLOW_TEXT_PATCH=1」）。
bool EnsurePatchEnv() {
    char env[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", env, sizeof(env));
    if (n > 0 && env[0] == '1') return true;
    if (!SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", "1")) {
        x::runtime::LogW("MeleeVeto", "无法设置 XCAT_ALLOW_TEXT_PATCH=1 err=%lu", GetLastError());
        return false;
    }
    return true;
}

void RequestApply() {
    if (!x::runtime::main_thread::WaitUntilInstalled(0)) return;
    (void)x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 3000,
                                                x::runtime::main_thread::JobPrio::High);
}

const char* VerdictName(int v) {
    switch (static_cast<Verdict>(v)) {
        case Verdict::Safe:
            return "safe";
        case Verdict::Unsafe:
            return "unsafe";
        default:
            return "measuring";
    }
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("MeleeVeto", "worker start");
    DWORD lastHeart = 0;
    int lastNoticeWt = 0;
    DWORD lastRangeTry = 0;
    int rangeTries = 0;
    constexpr int kRangeTryMax = 40;  // 3 秒一次，约两分钟；建不成就不再纠缠
    while (!gStop.load(std::memory_order_acquire)) {
        const bool want = gWant.load(std::memory_order_acquire);
        const bool need =
            gHooksWanted.load(std::memory_order_acquire) &&
            ((!gMelee.active && !gMeleeRefuse.load(std::memory_order_relaxed)) ||
             (!gShoot.active && !gShootRefuse.load(std::memory_order_relaxed)) ||
             (!gRect.active && !gRectRefuse.load(std::memory_order_relaxed) &&
              RectProbeRequested()));
        if (need) RequestApply();
        const DWORD now = GetTickCount();
        // 印表单独投泵、自己控节奏：单例和 Range 都是懒建的，arm 那一瞬间往往还没建成，
        // 只试一次必然空手而归。故意不塞进 PumpApply —— 那条路每跑一趟都会打 arm 行。
        if (gRect.active && !gRangeDumped.load(std::memory_order_acquire) &&
            rangeTries < kRangeTryMax && (!lastRangeTry || now - lastRangeTry >= 3000)) {
            lastRangeTry = now ? now : 1;
            ++rangeTries;
            if (x::runtime::main_thread::WaitUntilInstalled(0))
                (void)x::runtime::main_thread::InvokeAndWait(&PumpDumpRange, nullptr, 3000);
        }
        if (want && (!lastHeart || now - lastHeart >= kHeartMs)) {
            lastHeart = now;
            // 换武器要重测：判决是「这把武器的普攻伤害走不走近战体内」，换把武器就不成立了。
            const int wt = gLastWt.load(std::memory_order_relaxed);
            const int vw = gVerdictWt.load(std::memory_order_relaxed);
            if (vw && wt && vw != wt) {
                x::runtime::LogI("MeleeVeto", "武器变更 %d→%d，判决作废重测", vw, wt);
                ResetCounters();
                gVerdictWt.store(0, std::memory_order_relaxed);
            }
            // 拿别的武器勾了这个开关，日志里得说清为什么普攻照旧。
            if (wt && wt != kWtThrowingGlove && wt != lastNoticeWt) {
                lastNoticeWt = wt;
                x::runtime::LogI("MeleeVeto",
                                 "wt=%d 不在白名单（只拦飞镖 %d），转为纯观测：普攻照常执行，"
                                 "只记 nest / top。", wt, kWtThrowingGlove);
            }
            // 这里只读原子计数，绝不去碰托管对象（worker 非泵线程）。
            x::runtime::LogI("MeleeVeto",
                             "heart melee=%d shoot=%d mode=%s verdict=%s wt=%d job=%d "
                             "mCall=%u conc=%u mTrue=%u mFalse=%u nest=%u top=%u veto=%u",
                             gMelee.active ? 1 : 0, gShoot.active ? 1 : 0,
                             wt == kWtThrowingGlove ? "veto" : "observe",
                             VerdictName(gVerdict.load(std::memory_order_relaxed)), wt,
                             gLastJob.load(std::memory_order_relaxed),
                             gMCall.load(std::memory_order_relaxed),
                             gConclusive.load(std::memory_order_relaxed),
                             gMTrue.load(std::memory_order_relaxed),
                             gMFalse.load(std::memory_order_relaxed),
                             gNest.load(std::memory_order_relaxed),
                             gTop.load(std::memory_order_relaxed),
                             gVetoHits.load(std::memory_order_relaxed));
        }
        // 探针要在**开关关着**时用（得让近战真跑起来才看得到自然挥拳），所以不能挂在 heart
        // 里面。每个 (kind, 帧) 组合只打一行，不按发计流水。
        for (int ctx = 0; ctx < 2; ++ctx) {
            for (int k = 0; k < kRectKindSlots; ++k) {
                RectSlot& s = gRectSlot[ctx][k];
                if (s.state.load(std::memory_order_acquire) != 1) continue;
                float f[4]{};
                for (int i = 0; i < 4; ++i) {
                    const uint32_t bits = s.f[i].load(std::memory_order_relaxed);
                    memcpy(&f[i], &bits, sizeof(bits));
                }
                x::runtime::LogI("MeleeVeto",
                                 "rect kind=%d 帧=%s src=%s rect=(%.1f, %.1f, %.1f, %.1f) "
                                 "hits=%u calls=%u wt=%d",
                                 k, ctx ? "普攻" : "非普攻",
                                 k == kRectKindConstPath ? "rdata-const" : "managed-lookup",
                                 f[0], f[1], f[2], f[3],
                                 s.hits.load(std::memory_order_relaxed),
                                 gRectCalls.load(std::memory_order_relaxed),
                                 gLastWt.load(std::memory_order_relaxed));
                s.state.store(2, std::memory_order_release);
            }
        }
        {
            static int lastWide = -1;
            const int wide = gRectKindWide.load(std::memory_order_relaxed);
            if (wide >= 0 && wide != lastWide) {
                lastWide = wide;
                x::runtime::LogI("MeleeVeto", "rect kind=%d 落在槽位外（>=%d），只计不记框", wide,
                                 kRectKindSlots);
            }
        }
        Sleep(want ? 1000 : 2000);
    }
    x::runtime::LogI("MeleeVeto", "worker exit");
    return 0;
}

}  // namespace

void Init() {
    gStop.store(false, std::memory_order_release);
    gHooksWanted.store(false, std::memory_order_release);
    gRectRefuse.store(false, std::memory_order_relaxed);
    gMeleeRefuse.store(false, std::memory_order_relaxed);
    gShootRefuse.store(false, std::memory_order_relaxed);
    ResetCounters();
    ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "init");
}

void Shutdown() {
    StopWorker();
    gWant.store(false, std::memory_order_release);
    gHooksWanted.store(false, std::memory_order_release);
    if (gMelee.active || gShoot.active || gRect.active) {
        if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
            x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 2000);
        else {
            RemoveAbs(gMelee);
            gMeleeTramp = nullptr;
            RemoveAbs(gShoot);
            gShootTramp = nullptr;
        }
    }
}

void StartWorker() {
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        x::runtime::LogW("MeleeVeto", "CreateThread failed");
        return;
    }
    HANDLE prev = nullptr;
    if (!gWorker.compare_exchange_strong(prev, th, std::memory_order_acq_rel)) CloseHandle(th);
}

void StopWorker() {
    gStop.store(true, std::memory_order_release);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

void SetEnabled(bool on) {
    if (on && !EnsurePatchEnv()) {
        gWant.store(false, std::memory_order_release);
        return;
    }
    const bool was = gWant.exchange(on, std::memory_order_acq_rel);
    if (was == on) return;

    // 开关每次翻转都记一笔。2026-08-12 15:15 那次断线后开关被人写成 0，日志里只有
    // PumpApply 的 `disarm`（结果），查不出是哪一侧下发的，白折腾了一轮。
    x::runtime::LogI("MeleeVeto", "switch %d→%d hooks=%d/%d", was ? 1 : 0, on ? 1 : 0,
                     gMelee.active ? 1 : 0, gShoot.active ? 1 : 0);

    if (!on) {
        // 钩子留着，只灭开关：VetoNow 下一发就放行，不走泵、不会失败。
        return;
    }
    gMeleeRefuse.store(false, std::memory_order_relaxed);
    gShootRefuse.store(false, std::memory_order_relaxed);
    ResetCounters();
    gVerdictWt.store(0, std::memory_order_relaxed);
    gHooksWanted.store(true, std::memory_order_release);
    RequestApply();
}

bool IsEnabled() { return gWant.load(std::memory_order_acquire); }

}  // namespace x::features::melee_veto
