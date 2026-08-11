// TWMS Classic — attack_accel（攻击加速）
//
// 启用后周期：
//   - LocalUser+0x118=-1（跳过动作等待；字段哈希 a55e80b9… 防漂移）
//   - CutLayerDelays（砍动作层 delay）
// 出刀频率由 simpleCombatAttackIntervalMs（面板「间隔」，默认 123，下限 1）控制。
// 禁止 GA .text E9。
//
// 2026-08-04 撤销「Prepare 绝对攻速」写入：SecondaryStat+0x1BC/0x1C4 经 IDA 实证
// 是 nSlow_/tSlow_（减速 debuff），不是攻速槽。Prepare 内仍 `mov eax,[r14+1BCh]`
//（remount 2026-08-06：RVA 0xFE06A0 @ imagebase 0x7ff848c80000 → 0x7FF849C60963），
// nSlow_ 非 0 即顶掉 GetSpeed()。两字段现仅作只读诊断。
//
// 2026-08-04 接入真攻速槽 nBooster_@0xBC（IDA 运行时 dump，imagebase 0x7FFB83A80000）：
//   StatDetailAggregator.GetAttackSpeed(RVA 0xE78EB0) `mov rax,[rdi+8]`(=input.SecondaryStat)
//     → `mov ebx,[rax+0BCh]` → 作 weaponBooster 传入 GetAttackSpeedDegree(0x7FFB850112B0)
//   GetAttackSpeedDegree 去混淆后 = clamp(weaponDegree - (skill==4001334 ? 2:0)
//                                          + weaponBooster + partyBooster, 2, 10)
//     （种子实读：8A2E44F8→4001334 RogueDoubleStabDagger；8A2E44FC→2；8A2E4500→10）
//   战斗调用点 0x7FFB84A62960 尾部：攻击延迟 = 基础延迟 × (degree + 10) / 16
//     （degree=6 → 16/16=1.0，自洽点确证「degree 越小越快」）
// 故 nBooster_ 是**加数**，要加速必须写负值；-8 可让任何武器夹到 degree=2（×0.75）。
// 与 nSlow_ 的关键区别：Prepare 全 251 条指令只出现 1BCh/1C8h，**没有** 0BCh —— 本字段
// 不进动作层，只进攻击延迟，故不会重演视觉层错乱。
// tBooster_@0xC4 由 CheckByTime(tCur)/GetRemainTime(id,tCur) 按**游戏钟**判到期
//（GetRemainTime RVA 0xD65FA0 内 `[rcx+0BCh]`→`[rcx+0C0h]`→`[rcx+0C4h]` 三连读，
//  尾部 MBA 化简即 tBooster_ - tCur），必须用 skill_port::GetGameUpdateTimeMs 续期。
//
// 2026-08-04 booster 拆成独立开关（gBoosterDesired），不再挂在 gDesired 上。依据：
//   0.1.36(仅清忙锁) 出刀间隔中位 63ms（n=845/1073），0.1.37(清忙锁+booster) 64ms(n=396)
//   —— booster 净收益 0ms。同版内 accel 关(85ms,n=20) → 开(64ms) 的提速全部来自清忙锁。
//   清忙锁把引擎那道闸整个拆了，闸后的 (degree+10)/16 再短也落不到地；节奏实由面板
//   interval=50ms 与状态机决定（间隔直方图质量压在 50-99ms）。
//   booster 真正的价值是**替掉**清忙锁：它是引擎合法通道，不改动作忙标志。按 85ms 基线推
//   算单开 -8 约 68ms（×0.8）。要量它就必须能在 accel=0 时单独打开 —— 同一开关做不到，
//   因为 attackAccel 还顺带下发 animBusyOverride=0 / immediateUp，会混进变量。
//
// 实验·跳过 Prepare：改 LocalUser 虚表槽（SetAttackAction @RVA 0x10FA090 虚调 Prepare），
// 不碰 GA .text。关开关时 hook 仍在，走 orig 透传。
// remount 2026-08-06：Prepare/哈希/字段名哈希已对 dump.cs.restored.C + 运行时 IDB。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_accel.h"

#include "../../runtime/dbg_log_file.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../ports/attack_input_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../../ui/player_vitals.h"
#include "xcat_payload_control.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace x {
namespace features {
namespace attack_accel {
namespace {

// 默认/兜底偏移（dump.cs.runtime + LocalUser_Prepare [r14+1BCh] 实锤；种子解出阈 0）
constexpr size_t kOffActionBusyHint = 0x118;
constexpr size_t kOffActionLayerAHint = 0x120;
constexpr size_t kOffActionLayerBHint = 0x128;
constexpr size_t kOffLayerDelay = 0x14;
// ActionLayer：Prepare 把已缩放 delay 写入 int[]（Il2CppArray），再拷当前帧到 +0x14。
// IDA User_PrepareActionLayer：`mov [layer+20h], arr` / `[arr+idx*4+20h]`；Slot14 每 tick ≈−30。
constexpr size_t kOffLayerDelayArr = 0x20;
// sum(delay') → 闸门 ms（多发限频用）。
// 硬事实：Slot14 每 tick delay≈−30。墙钟换算若按 ~50Hz → sum×2/3；但帧率锁/卡顿下
// Update 可能更慢 → 真实动作更长。多发策略「只能多不能少」：闸门取上界
// **ms = sum**（等价假定 ≤~30Hz 逻辑节拍），绝不短于 sum×2/3。
constexpr DWORD DelaySumToMs(int sum) {
    if (sum <= 0) return 0;
    return static_cast<DWORD>(sum);
}
// WM.MyUser / SecondaryStat → x::ui::player SSOT（hash 防漂）
// nSlow_ / tSlow_ —— 只读诊断，本模块不再写（见文件头 2026-08-04 说明）。
constexpr size_t kOffSlowHint = 0x1BC;
constexpr size_t kOffSlowExpireHint = 0x1C4;
// nBooster_ / rBooster_ / tBooster_ —— 真攻速槽，accel 开启时接管（见文件头）。
constexpr size_t kOffBoostHint = 0xBC;
constexpr size_t kOffBoostReasonHint = 0xC0;
constexpr size_t kOffBoostExpireHint = 0xC4;
// A 系 CTS Speed：nSpeed_@0x84 / tSpeed_@0x8C（GetActionSpeed 读 +80/+84；CheckByTime 不扫）。
// SecondaryStat 基速 nSpeed@+0x80（Forced/装备 SetFrom 默认约 100，常被鞋顶到近 140）。
constexpr size_t kOffSpeedBase = 0x80;
constexpr size_t kOffSpeedNHint = 0x84;
constexpr size_t kOffSpeedTHint = 0x8C;
constexpr int kActionSpeedClampLo = 70;
constexpr int kActionSpeedClampHi = 140;
// TempStats[] @ +0x450；PartyBooster = index 4；TempStatBase.Value @ +0x18。
constexpr size_t kOffTempStatsHint = 0x450;
constexpr size_t kIl2CppArrayLenOff = 0x18;
constexpr size_t kIl2CppArrayDataOff = 0x20;
constexpr size_t kTempStatValueOff = 0x18;
constexpr int kPartyBoosterIndex = 4;
constexpr int kDashSpeedIndex = 1;
// LocalUser+0x15C = 武器攻速 degree（战斗点 0x7FFB84A62C52 `mov edi,[rdi+15Ch]`）。只读。
constexpr size_t kOffLuWeaponDegree = 0x15C;
// 引擎夹 [2,10]；默认 -8 保证任何武器都落到 2（最快，延迟 ×0.75）。
// 合法 booster 只写 -1/-2；Party 滑条可调，见 xcat::kAttackAccelPartyBoosterValue*。
// nBooster_ 仍写死 -8（用户入口已关）。
constexpr int kBoosterValue = -8;
constexpr int kPartyBoosterValueDefault = -8;
// CalcWeaponAttackSpeedTier（RVA 0x15940F0 @ imagebase 0x7ff848c80000）：
//   lo = dword_7FF84F4E956C ^ 0xE95BBBB4  → 种子 0xE95BBBB6 解出 2（独占 xref）
// 破限：写种子使 lo=滑条值（默认 -10）；不改 Party / nBooster_。
// delay=(deg+10)/16；deg=-10 → ×0。
constexpr uint32_t kRvaDegreeClampLoSeed = 0x686956Cu;
constexpr uint32_t kDegreeClampLoXorImm = 0xE95BBBB4u;
constexpr uint32_t kDegreeClampLoSeedPristine = 0xE95BBBB6u;  // → 2
constexpr int kDegreeClampLoDefault = -10;
constexpr DWORD kDegreeFloorSeedRetryMs = 2000;

uint32_t SeedForDegreeClampLo(int lo) {
    return static_cast<uint32_t>(lo) ^ kDegreeClampLoXorImm;
}

int DegreeClampLoFromSeed(uint32_t seed) {
    return static_cast<int>(seed ^ kDegreeClampLoXorImm);
}
// nSpeed_=+40 + 基速 100 → GetActionSpeed≈140（Prepare clamp 上限）。
constexpr int kActionSpeedNValue = 40;
// tBooster_ 按游戏钟续到 now+60s，每拍重写；过期会被 CheckByTime→Reset 清掉。
constexpr int kBoosterHoldMs = 60000;
constexpr int kActionSpeedHoldMs = 60000;
constexpr DWORD kRefreshMs = 16;
constexpr DWORD kRebindMs = 2000;
constexpr DWORD kLogChangeMs = 1000;  // 字段变化：最短间隔（防抖）
constexpr DWORD kLogIdleMs = 30000;   // 稳态心跳
constexpr DWORD kLandGraceMs = 400;
constexpr DWORD kSkipPrepareLandGraceMs = 1000;

// LocalUser(TDI:1560) 覆写 Prepare @ 0xFE06A0；基类 User @ 0x1253CA0
// UserLocal(TDI:1577) : LocalUser — 无再覆写；虚表槽仍在实例 klass 上。
// dump Slot:32 · 哈希 f71637b0…（默认参 action=6,speed=100,bool=false）· remount 2026-08-06
constexpr uint32_t kRvaUserPrepare = 0xFE06A0;
constexpr uint32_t kRvaFbPrepare = 0x1253CA0;
constexpr char kHashPrepareActionLayer[] =
    "f71637b0493ae259de6c988fe81a1f3f12ae34a470fa02f641acfd67289f14c";
constexpr char kHashLocalUser[] =
    "b8c9aedb2c800fa8ec9515b0f728235725989303f6bb609bafebeee4a902078";
// 声明 Prepare 的类（= LocalUser）；虚表补丁仍打在最末级 UserLocal 上
constexpr const char* kHashLocalUserDecl = kHashLocalUser;
constexpr int kPrepareVtableSlot = 32;
constexpr size_t kVirtInvokeStride = 16;
constexpr size_t kHintVtableBase = 0x138;
constexpr size_t kVtableScanLo = 0x80;
constexpr size_t kVtableScanHi = 0xC00;
constexpr DWORD kSkipPrepareInstallRetryMs = 2000;

// 字段防漂移（Il2CppDumper 哈希名 → field_get_offset；失败回退 Hint）
// remount 2026-08-06：类/字段哈希全换；偏移仍 0x118/0x120/0x128 / SS 0xBC/0xC0/0xC4/0x1BC/0x1C4
constexpr char kHashSecondaryStat[] =
    "fda0a837975e9b385db9604d6689232d1f1783dcfafa16403a92309b5604df3";
constexpr char kHashSlow[] =
    "d5bf793df45a8f8ea38a2f408c8c5edb7c2122b92b34916cc914dede9354408";
constexpr char kHashSlowExpire[] =
    "cea518cf2cf239b37d86d7261b4ddeaadfb9595779b1caff529800831d86af7";
constexpr char kHashBoost[] =
    "c7a681d25ac765845bc7ee482fe7bfe78338e70e65456d3ee4302481dc4abe8";
constexpr char kHashBoostReason[] =
    "ed17ce43efc6ed6c54ca965ef7b462c9f3a9ce27b3f5dde626373366abff984";
constexpr char kHashBoostExpire[] =
    "abec342a6ade99d50a338ccb55acb7199a39ba031ec4626cb5da74d84ceb9dc";
constexpr char kHashActionBusy[] =
    "a55e80b967beb6b3a4ae5af0e93a1e09293c74f5fe852a6f014dc917b55d926";
constexpr char kHashActionLayerA[] =
    "a8269a5fd552d9c18702bb17235ed49e9eb56e51b8f8967aea25944d48ee963";
constexpr char kHashActionLayerB[] =
    "df44aea45566e5964a25045760965cbc2b54f42485f08b8fdc5752897abe6e8";

using FnPrepareActionLayer = void (*)(void* self, int32_t action, int32_t speed, uint8_t flag,
                                      const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<bool> gDesired{false};
std::atomic<bool> gBoosterDesired{false};
std::atomic<bool> gActionSpeedDesired{false};
std::atomic<bool> gPartyBoosterDesired{false};
std::atomic<int> gPartyBoosterValue{kPartyBoosterValueDefault};
std::atomic<bool> gBreakDegreeFloorDesired{false};
std::atomic<bool> gBreakDegreeFloorApplied{false};
std::atomic<int> gBreakDegreeFloorLo{kDegreeClampLoDefault};
std::atomic<int> gBreakDegreeFloorAppliedLo{2};  // 诊断用：当前种子解出的 lo
std::atomic<bool> gCutLayerDesired{false};
std::atomic<bool> gSkipPrepareDesired{false};
std::atomic<bool> gSkipPrepareArmed{false};
std::atomic<bool> gSkipPrepareNeedGrace{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
std::atomic<uint32_t> gSkipPrepareHits{0};
std::atomic<uint32_t> gSkipPreparePassHits{0};
std::atomic<bool> gSkipPrepareInstalled{false};
std::atomic<FnPrepareActionLayer> gOrigPrepare{nullptr};
void* gSkipPrepareKlass = nullptr;
void** gSkipPrepareSlot = nullptr;
MethodInfoHead* gSkipPrepareMi = nullptr;
size_t gSkipPrepareSlotOff = 0;
char gSkipPreparePath[48]{};
DWORD gLastSkipPrepareInstallTry = 0;
HANDLE gLog = INVALID_HANDLE_VALUE;

// 运行时解析后的字段偏移（默认 = Hint）
size_t gOffActionBusy = kOffActionBusyHint;
size_t gOffActionLayerA = kOffActionLayerAHint;
size_t gOffActionLayerB = kOffActionLayerBHint;
size_t gOffSlow = kOffSlowHint;
size_t gOffSlowExpire = kOffSlowExpireHint;
size_t gOffBoost = kOffBoostHint;
size_t gOffBoostReason = kOffBoostReasonHint;
size_t gOffBoostExpire = kOffBoostExpireHint;
size_t gOffSpeedN = kOffSpeedNHint;
size_t gOffSpeedT = kOffSpeedTHint;
size_t gOffTempStats = kOffTempStatsHint;
std::atomic<bool> gFieldOffResolved{false};
char gFieldOffPath[80]{};

// booster 接管状态（仅 worker 线程访问）
void* gBoostSs = nullptr;    // 我们写过的那个 SecondaryStat
int gBoostBaseN = 0;         // 接管前引擎的值，关闭时奉还
int gBoostBaseR = 0;
int gBoostBaseT = 0;
int gBoostLastN = 0;         // 我们上次写下的值，用于识别「引擎又改了」
int gBoostLastR = 0;
int gBoostLastT = 0;
bool gBoostHeld = false;
uint32_t gBoostRebaseCnt = 0;  // 重登基线次数：持续上涨 = 引擎每帧在跟我们抢这个字段
DWORD gBoostRebaseLogAt = 0;   // 上条 baseline 日志时刻（16ms 一拍，不节流会刷爆）

// A 系 nSpeed_ 接管
void* gActSpSs = nullptr;
int gActSpBaseN = 0;
int gActSpBaseT = 0;
int gActSpLastN = 0;
int gActSpLastT = 0;
bool gActSpHeld = false;
uint32_t gActSpRebaseCnt = 0;
DWORD gActSpRebaseLogAt = 0;

// PartyBooster TempStats[4].Value 接管
void* gPartySs = nullptr;
void* gPartyElem = nullptr;
int gPartyBaseV = 0;
int gPartyLastV = 0;
bool gPartyHeld = false;
uint32_t gPartyRebaseCnt = 0;
DWORD gPartyRebaseLogAt = 0;
uint32_t gPartyMissLogAt = 0;

// B 系 degree 下限种子（CalcWeaponAttackSpeedTier 独占 .data）
bool gDegreeFloorSeedHeld = false;
uint32_t gDegreeFloorSeedOrig = kDegreeClampLoSeedPristine;
uintptr_t gDegreeFloorSeedBase = 0;
DWORD gDegreeFloorSeedTryAt = 0;
DWORD gDegreeFloorSeedLogAt = 0;

void WriteLogHandle(HANDLE h, const char* buf, int n) {
    if (h == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, nullptr);
}

void Log(const char* fmt, ...) {
    char body[720];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[800];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond,
                     body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteLogHandle(gLog, buf, n);
    x::runtime::LogI("AtkAccel", "%s", body);
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &self) ||
        !self)
        return L".";
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L".";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return s.substr(0, slash);
}

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"attack_accel.log");
}

// 在 klass(+parents) 上按字段哈希取 offset；失败返回 0。
size_t FieldOffsetByHash(void* klass, const char* nameHash) {
    if (!klass || !nameHash || !x::runtime::il2cpp::Ensure()) return 0;
    const auto& e = x::runtime::il2cpp::Get();
    for (void* k = klass; k; ) {
        if (e.classGetFieldFromName && e.fieldGetOffset) {
            void* field = nullptr;
            __try {
                field = e.classGetFieldFromName(k, nameHash);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                field = nullptr;
            }
            if (field) {
                size_t off = 0;
                __try {
                    off = e.fieldGetOffset(field);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    off = 0;
                }
                if (off) return off;
            }
        }
        if (e.classGetFields && e.fieldGetName && e.fieldGetOffset) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* field = e.classGetFields(k, &iter);
                    if (!field) break;
                    const char* nm = nullptr;
                    __try {
                        nm = e.fieldGetName(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        nm = nullptr;
                    }
                    if (!nm || std::strcmp(nm, nameHash) != 0) continue;
                    size_t off = 0;
                    __try {
                        off = e.fieldGetOffset(field);
                    } __except (EXCEPTION_EXECUTE_HANDLER) {
                        off = 0;
                    }
                    if (off) return off;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (!e.classParent) break;
        __try {
            k = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
    }
    return 0;
}

size_t PickOff(size_t resolved, size_t hint, bool* usedHash) {
    if (resolved) {
        if (usedHash) *usedHash = true;
        return resolved;
    }
    return hint;
}

void EnsureFieldOffsets() {
    if (gFieldOffResolved.load(std::memory_order_acquire)) return;
    if (!x::runtime::il2cpp::Ensure()) return;

    bool busyH = false, layerAH = false, layerBH = false;
    bool slowH = false, slowExpH = false, boostH = false, boostRH = false, boostExpH = false;
    void* ul = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* ssKlass = x::runtime::il2cpp::FindClass("", kHashSecondaryStat);
    // IL2CPP 已起但类表未齐：下 tick 再试，勿锁死 hint。
    if (!ul && !ssKlass) return;

    if (ul) {
        gOffActionBusy =
            PickOff(FieldOffsetByHash(ul, kHashActionBusy), kOffActionBusyHint, &busyH);
        gOffActionLayerA =
            PickOff(FieldOffsetByHash(ul, kHashActionLayerA), kOffActionLayerAHint, &layerAH);
        gOffActionLayerB =
            PickOff(FieldOffsetByHash(ul, kHashActionLayerB), kOffActionLayerBHint, &layerBH);
    }

    if (ssKlass) {
        gOffSlow = PickOff(FieldOffsetByHash(ssKlass, kHashSlow), kOffSlowHint, &slowH);
        gOffSlowExpire =
            PickOff(FieldOffsetByHash(ssKlass, kHashSlowExpire), kOffSlowExpireHint, &slowExpH);
        gOffBoost = PickOff(FieldOffsetByHash(ssKlass, kHashBoost), kOffBoostHint, &boostH);
        gOffBoostReason =
            PickOff(FieldOffsetByHash(ssKlass, kHashBoostReason), kOffBoostReasonHint, &boostRH);
        gOffBoostExpire =
            PickOff(FieldOffsetByHash(ssKlass, kHashBoostExpire), kOffBoostExpireHint, &boostExpH);
    }

    const int hits = (busyH ? 1 : 0) + (layerAH ? 1 : 0) + (layerBH ? 1 : 0) + (slowH ? 1 : 0) +
                     (slowExpH ? 1 : 0) + (boostH ? 1 : 0) + (boostRH ? 1 : 0) + (boostExpH ? 1 : 0);
    constexpr int kExpect = 8;
    snprintf(gFieldOffPath, sizeof(gFieldOffPath), "%s hits=%d/%d",
             hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback"), hits, kExpect);
    // 有一边类到了就落盘；另一边仍 hint，可接受（偏移本轮已静态复核）。
    gFieldOffResolved.store(true, std::memory_order_release);
    Log("field offsets path=%s busy=0x%zX layer=0x%zX/0x%zX slow(ro)=0x%zX/0x%zX "
        "boost=0x%zX/0x%zX/0x%zX myUser=0x%zX",
        gFieldOffPath, gOffActionBusy, gOffActionLayerA, gOffActionLayerB, gOffSlow, gOffSlowExpire,
        gOffBoost, gOffBoostReason, gOffBoostExpire, x::ui::player::OffWmMyUser());
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32(void* obj, size_t off, int v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ProbeSsAlive(void* ss) {
    if (!ss) return false;
    __try {
        if (!*reinterpret_cast<void**>(ss)) return false;
        (void)*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ss) + gOffSlow);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsKnownPrepareRva(uintptr_t rva) {
    return rva == kRvaUserPrepare || rva == kRvaFbPrepare;
}

uint32_t PtrRva(void* p) {
    if (!p) return 0;
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (!base) return 0;
    const auto a = reinterpret_cast<uintptr_t>(p);
    if (a < base) return 0;
    const uint64_t d = static_cast<uint64_t>(a - base);
    return d > 0x7FFFFFFFull ? 0u : static_cast<uint32_t>(d);
}

void ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode code, const char* detail) {
    x::runtime::anchor_lamps::Set("SkipPrep", code, detail);
}

// dump 默认参 action=6 为待机 Idle；武装后也必须透传，否则呼吸/落地皮会坏。
constexpr int32_t kPrepareActionIdle = 6;

void Hook_PrepareActionLayer(void* self, int32_t action, int32_t speed, uint8_t flag,
                             const void* methodInfo) {
    FnPrepareActionLayer orig = gOrigPrepare.load(std::memory_order_acquire);
    // Idle 永远透传；未武装 / 未装钩 / 无 orig → 透传。
    const bool wantSkip = orig && action != kPrepareActionIdle &&
                          gSkipPrepareDesired.load(std::memory_order_acquire) &&
                          gSkipPrepareArmed.load(std::memory_order_acquire) &&
                          gSkipPrepareInstalled.load(std::memory_order_acquire);
    if (!wantSkip) {
        gSkipPreparePassHits.fetch_add(1, std::memory_order_relaxed);
        if (orig) orig(self, action, speed, flag, methodInfo);
        return;
    }
    gSkipPrepareHits.fetch_add(1, std::memory_order_relaxed);
    // 跳过攻击 Prepare：不建攻击层。自然解锁靠 Slot14 扣完 layer 才写 +0x118=-1；
    // 无层则忙锁永久卡在 actionIdx（BIN：只开 skip 出刀后卡住；开 accel 清忙锁则恢复）。
    // 立刻清忙锁，并透传 Idle Prepare 重建呼吸层（action==6 不会再进本分支）。
    WriteI32(self, gOffActionBusy, -1);
    orig(self, kPrepareActionIdle, 100, 0, methodInfo);
}

bool PatchGaU32(uint32_t* p, uint32_t want) {
    if (!p) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(uint32_t), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        *p = want;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(p, sizeof(uint32_t), old, &old);
    return ok;
}

void RestoreDegreeFloorSeed() {
    if (!gDegreeFloorSeedHeld) {
        gBreakDegreeFloorApplied.store(false, std::memory_order_release);
        gBreakDegreeFloorAppliedLo.store(2, std::memory_order_release);
        return;
    }
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (base && base == gDegreeFloorSeedBase) {
        auto* p = reinterpret_cast<uint32_t*>(base + kRvaDegreeClampLoSeed);
        if (!PatchGaU32(p, gDegreeFloorSeedOrig)) {
            Log("breakFloor restore fail p=%p orig=0x%X", p, gDegreeFloorSeedOrig);
        } else {
            Log("breakFloor restore seed=0x%X (lo=%d)", gDegreeFloorSeedOrig,
                DegreeClampLoFromSeed(gDegreeFloorSeedOrig));
        }
    } else {
        Log("breakFloor drop restore (base %p -> %p)", (void*)gDegreeFloorSeedBase,
            (void*)base);
    }
    gDegreeFloorSeedHeld = false;
    gDegreeFloorSeedBase = 0;
    gBreakDegreeFloorApplied.store(false, std::memory_order_release);
    gBreakDegreeFloorAppliedLo.store(2, std::memory_order_release);
}

// 开：写独占种子使 clamp lo=滑条值；关：原值奉还。不碰 PartyBooster / nBooster_。
void SyncDegreeFloorSeed(bool want) {
    const DWORD now = GetTickCount();
    if (!want) {
        RestoreDegreeFloorSeed();
        return;
    }
    const int wantLo = static_cast<int>(
        xcat::ClampAttackAccelBreakDegreeFloorLo(gBreakDegreeFloorLo.load(std::memory_order_acquire)));
    const uint32_t wantSeed = SeedForDegreeClampLo(wantLo);
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (!base) {
        if (!gDegreeFloorSeedLogAt || now - gDegreeFloorSeedLogAt >= kDegreeFloorSeedRetryMs) {
            gDegreeFloorSeedLogAt = now;
            Log("breakFloor pending — GaBase null");
        }
        return;
    }
    if (gDegreeFloorSeedHeld && gDegreeFloorSeedBase != base) {
        Log("breakFloor GaBase rebound %p -> %p — reapply", (void*)gDegreeFloorSeedBase,
            (void*)base);
        gDegreeFloorSeedHeld = false;
        gBreakDegreeFloorApplied.store(false, std::memory_order_release);
    }
    auto* p = reinterpret_cast<uint32_t*>(base + kRvaDegreeClampLoSeed);
    uint32_t cur = 0;
    __try {
        cur = *p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!gDegreeFloorSeedLogAt || now - gDegreeFloorSeedLogAt >= kDegreeFloorSeedRetryMs) {
            gDegreeFloorSeedLogAt = now;
            Log("breakFloor read AV p=%p", p);
        }
        return;
    }
    if (gDegreeFloorSeedHeld && cur == wantSeed) {
        gBreakDegreeFloorApplied.store(true, std::memory_order_release);
        gBreakDegreeFloorAppliedLo.store(wantLo, std::memory_order_release);
        return;
    }
    // 滑条变了：允许立刻改写（不走 fail 冷却）。
    const bool retarget = gDegreeFloorSeedHeld && cur != wantSeed;
    if (!retarget && gDegreeFloorSeedTryAt && now - gDegreeFloorSeedTryAt < kDegreeFloorSeedRetryMs &&
        !gDegreeFloorSeedHeld) {
        return;
    }
    gDegreeFloorSeedTryAt = now;
    if (!gDegreeFloorSeedHeld) {
        if (DegreeClampLoFromSeed(cur) == 2) {
            gDegreeFloorSeedOrig = cur;
        } else {
            Log("breakFloor unexpected seed=0x%X (xorLo=%d) — still patch; orig=pristine", cur,
                DegreeClampLoFromSeed(cur));
            gDegreeFloorSeedOrig = kDegreeClampLoSeedPristine;
        }
    }
    if (!PatchGaU32(p, wantSeed)) {
        Log("breakFloor patch fail p=%p cur=0x%X wantLo=%d", p, cur, wantLo);
        return;
    }
    gDegreeFloorSeedHeld = true;
    gDegreeFloorSeedBase = base;
    gBreakDegreeFloorApplied.store(true, std::memory_order_release);
    gBreakDegreeFloorAppliedLo.store(wantLo, std::memory_order_release);
    Log("breakFloor apply seed 0x%X -> 0x%X (lo 2->%d) rva=0x%X", gDegreeFloorSeedOrig, wantSeed,
        wantLo, (unsigned)kRvaDegreeClampLoSeed);
}

bool PatchVtableMethodPtr(void** slot, void* hook, void** outOrig) {
    if (!slot || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) {
        if (orig == hook) {
            FnPrepareActionLayer prev = gOrigPrepare.load(std::memory_order_acquire);
            if (prev) {
                *outOrig = reinterpret_cast<void*>(prev);
                return true;
            }
        }
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        *slot = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return ok;
}

void RestoreVtableMethodPtr(void** slot, void* orig) {
    if (!slot || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    __try {
        *slot = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) {
        if (orig == hook) {
            FnPrepareActionLayer prev = gOrigPrepare.load(std::memory_order_acquire);
            if (prev) {
                *outOrig = reinterpret_cast<void*>(prev);
                return true;
            }
        }
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        if (mi->virtualMethodPointer == orig) mi->virtualMethodPointer = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return ok;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    __try {
        void* cur = mi->methodPointer;
        mi->methodPointer = orig;
        if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

// 防漂移：FindMethodResolved(hash→RVA/kind)；同形非唯一 → 校验 IsKnownPrepareRva
MethodInfoHead* ResolvePrepareMiOn(void* klass, const char** outPath) {
    if (outPath) *outPath = "miss";
    if (!klass) return nullptr;

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::ResolvePath;
    using x::runtime::il2cpp_method::TypeKind;
    // dump: void(int=6,int=100,bool=false)；同形在 UL/LU 上不唯一 → unique=false + RVA 过滤
    constexpr MethodShape kPrep{3, TypeKind::Void, false, true,
                                {TypeKind::I32, TypeKind::I32, TypeKind::Bool}};

    auto accept = [&](void* method, ResolvePath path, const char* tag) -> MethodInfoHead* {
        if (!method) return nullptr;
        auto* mi = reinterpret_cast<MethodInfoHead*>(method);
        const uint32_t got = PtrRva(mi->methodPointer);
        if (got && !IsKnownPrepareRva(got)) {
            if (path == ResolvePath::Hash) {
                Log("SkipPrepare hash MI rva=0x%X unexpected, try rva/kind", got);
            } else if (path == ResolvePath::Kind) {
                return nullptr;  // kind 首击可能漂到同形非 Prepare
            } else {
                Log("SkipPrepare path=%s rva=0x%X unexpected", tag, got);
            }
            return nullptr;
        }
        if (outPath) {
            if (path == ResolvePath::Hash) *outPath = got ? "hash" : "hash(nobase)";
            else if (path == ResolvePath::Kind) *outPath = "kind";
            else if (path == ResolvePath::Rva) *outPath = tag;
            else *outPath = tag;
        }
        if (path == ResolvePath::Kind) {
            Log("SkipPrepare ResolveMi kind hit tag=%s got=0x%X", tag, got);
        }
        return mi;
    };

    {
        const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
            klass, kRvaUserPrepare, kPrep, nullptr, kHashPrepareActionLayer);
        if (MethodInfoHead* mi = accept(mr.method, mr.path, "rva-ul")) {
            static bool sHitsLogged = false;
            if (!sHitsLogged) {
                sHitsLogged = true;
                Log("methods path=%s hits=1/1 Prepare",
                    mr.path == ResolvePath::Hash
                        ? "meta"
                        : (mr.path == ResolvePath::Miss ? "fallback" : "meta-partial"));
            }
            return mi;
        }
    }
    {
        // hash 已试过且可能脏；仅走 RVA/kind
        const auto mr =
            x::runtime::il2cpp_method::FindMethodCached(klass, kRvaUserPrepare, kPrep);
        if (MethodInfoHead* mi = accept(mr.method, mr.path, "rva-ul")) return mi;
    }
    {
        const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, kRvaFbPrepare, kPrep);
        if (MethodInfoHead* mi = accept(mr.method, mr.path, "rva-fb")) return mi;
    }

    if (void* raw = x::runtime::il2cpp_method::FindMethodByRva(klass, kRvaUserPrepare, true)) {
        if (outPath) *outPath = "rva-raw-ul";
        return reinterpret_cast<MethodInfoHead*>(raw);
    }
    if (void* raw = x::runtime::il2cpp_method::FindMethodByRva(klass, kRvaFbPrepare, true)) {
        if (outPath) *outPath = "rva-raw-fb";
        return reinterpret_cast<MethodInfoHead*>(raw);
    }
    return nullptr;
}

MethodInfoHead* ResolvePrepareMi(void* klass, const char** outPath) {
    if (MethodInfoHead* mi = ResolvePrepareMiOn(klass, outPath)) return mi;
    // Prepare 真体在 LocalUser；若 UL 元数据/父链异常，直查父类哈希
    void* lu = x::runtime::il2cpp::FindClass("", kHashLocalUser);
    if (lu && lu != klass) {
        if (outPath) *outPath = "miss-lu";
        if (MethodInfoHead* mi = ResolvePrepareMiOn(lu, outPath)) {
            if (outPath && *outPath && std::strncmp(*outPath, "miss", 4) != 0) {
                // 保留具体路径，前缀标 lu-
            }
            return mi;
        }
    }
    return nullptr;
}

// 在 klass 上扫 VirtualInvokeData{methodPtr, method}，不硬编码 +0x338
// mi 可空：此时用 wantPtrHint（通常 GaBase+PrepareRVA）扫槽。
bool FindPrepareVtableSlot(void* klass, MethodInfoHead* mi, void* wantPtrHint, void*** outSlot,
                           size_t* outOff, const char** outPath) {
    if (!klass || !outSlot || !outOff) return false;
    void* wantPtr = wantPtrHint;
    if (!wantPtr && mi) {
        __try {
            wantPtr = mi->methodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    if (!wantPtr) return false;

    struct Hit {
        void** slot = nullptr;
        size_t off = 0;
        int score = 0;  // 2=双匹配, 1=仅 MI, 1=仅 ptr
    };
    Hit hits[8]{};
    int n = 0;

    auto push = [&](size_t off, int score) {
        void** slot =
            reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off);
        for (int i = 0; i < n; ++i) {
            if (hits[i].slot == slot) {
                if (score > hits[i].score) hits[i].score = score;
                return;
            }
        }
        if (n >= (int)(sizeof(hits) / sizeof(hits[0]))) return;
        hits[n++] = Hit{slot, off, score};
    };

    for (size_t off = kVtableScanLo; off + kVirtInvokeStride <= kVtableScanHi; off += 8) {
        void* p0 = nullptr;
        void* p1 = nullptr;
        __try {
            p0 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off);
            p1 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (p1 == mi && p0 == wantPtr) {
            push(off, 2);
        } else if (p1 == mi) {
            push(off, 1);
        } else if (p0 == wantPtr && x::runtime::il2cpp::LooksLikeHeapPtr(p1)) {
            push(off, 1);
        }
    }

    if (n == 0) {
        // 末级 hint：Slot32 × 16 + 本包 vtable 基；校验 methodPtr RVA
        const size_t hintOff = kHintVtableBase + (size_t)kPrepareVtableSlot * kVirtInvokeStride;
        void* p0 = nullptr;
        void* p1 = nullptr;
        __try {
            p0 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + hintOff);
            p1 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + hintOff + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        const uint32_t rva = PtrRva(p0);
        // mi 为空时必须靠 RVA；有 mi 时允许 MI 指针对上即过
        if (!IsKnownPrepareRva(rva) && (!mi || p1 != mi)) return false;
        *outSlot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + hintOff);
        *outOff = hintOff;
        if (outPath) *outPath = "hint-slot32";
        return true;
    }

    // 取最高分；同分要求唯一
    int best = -1;
    int bestScore = -1;
    int bestCount = 0;
    for (int i = 0; i < n; ++i) {
        if (hits[i].score > bestScore) {
            bestScore = hits[i].score;
            best = i;
            bestCount = 1;
        } else if (hits[i].score == bestScore) {
            ++bestCount;
        }
    }
    if (best < 0 || bestCount != 1) {
        Log("SkipPrepare vtable scan ambiguous hits=%d bestScore=%d", n, bestScore);
        return false;
    }
    *outSlot = hits[best].slot;
    *outOff = hits[best].off;
    if (outPath) {
        *outPath = (bestScore >= 2) ? "scan-pair" : "scan-soft";
    }
    return true;
}

void UninstallSkipPrepareHook() {
    if (!gSkipPrepareInstalled.load(std::memory_order_acquire)) return;
    // 先卸武装，主线程 hook 走透传；再还原虚表 / MethodInfo。
    gSkipPrepareArmed.store(false, std::memory_order_release);
    FnPrepareActionLayer origFn = gOrigPrepare.load(std::memory_order_acquire);
    void* orig = reinterpret_cast<void*>(origFn);
    if (gSkipPrepareSlot && orig) RestoreVtableMethodPtr(gSkipPrepareSlot, orig);
    if (gSkipPrepareMi && orig) RestoreMethodInfo(gSkipPrepareMi, orig);
    gSkipPrepareInstalled.store(false, std::memory_order_release);
    gSkipPrepareSlot = nullptr;
    gSkipPrepareMi = nullptr;
    gSkipPrepareKlass = nullptr;
    gOrigPrepare.store(nullptr, std::memory_order_release);
    gSkipPrepareSlotOff = 0;
    gSkipPreparePath[0] = '\0';
    ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "off");
    Log("SkipPrepare uninstall");
}

bool EnsureSkipPrepareHook() {
    if (gSkipPrepareInstalled.load(std::memory_order_acquire)) {
        if (gSkipPrepareSlot) {
            void* cur = nullptr;
            __try {
                cur = *gSkipPrepareSlot;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                cur = nullptr;
            }
            if (cur != reinterpret_cast<void*>(&Hook_PrepareActionLayer) &&
                gOrigPrepare.load(std::memory_order_acquire)) {
                void* ignored = nullptr;
                if (PatchVtableMethodPtr(gSkipPrepareSlot,
                                         reinterpret_cast<void*>(&Hook_PrepareActionLayer),
                                         &ignored)) {
                    Log("SkipPrepare re-pin slot off=0x%zX", gSkipPrepareSlotOff);
                }
            }
        }
        return true;
    }
    const DWORD now = GetTickCount();
    if (gLastSkipPrepareInstallTry && now - gLastSkipPrepareInstallTry < kSkipPrepareInstallRetryMs)
        return false;
    gLastSkipPrepareInstallTry = now;

    if (!x::runtime::il2cpp::Ensure()) {
        ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "no-il2cpp");
        return false;
    }
    // 虚表补丁用最末级 MyUser 类（d344…）；MI 优先在声明类 LocalUser（d9ad…）上解析
    void* vtableKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    void* declKlass = x::runtime::il2cpp::FindClass("", kHashLocalUserDecl);
    void* klass = vtableKlass ? vtableKlass : declKlass;
    if (!klass) {
        Log("SkipPrepare install pending — UserLocal klass null");
        ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "no-UL");
        return false;
    }

    const char* miPath = "miss";
    MethodInfoHead* mi = nullptr;
    if (declKlass) mi = ResolvePrepareMi(declKlass, &miPath);
    if ((!mi || !mi->methodPointer) && vtableKlass && vtableKlass != declKlass) {
        mi = ResolvePrepareMi(vtableKlass, &miPath);
    }
    if ((!mi || !mi->methodPointer) && klass != declKlass && klass != vtableKlass) {
        mi = ResolvePrepareMi(klass, &miPath);
    }
    if (!vtableKlass) vtableKlass = klass;

    void* wantPtr = nullptr;
    if (mi) {
        __try {
            wantPtr = mi->methodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wantPtr = nullptr;
        }
    }
    uint32_t miRva = PtrRva(wantPtr);
    if (wantPtr && miRva && !IsKnownPrepareRva(miRva)) {
        Log("SkipPrepare install abort — MI rva=0x%X not Prepare path=%s", miRva, miPath);
        ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Miss, "bad-rva");
        return false;
    }
    if (!wantPtr) {
        wantPtr = x::runtime::il2cpp::AtRva<void*>(kRvaUserPrepare);
        miPath = "ga-rva";
        miRva = kRvaUserPrepare;
        if (!wantPtr) {
            Log("SkipPrepare install fail — Prepare MethodInfo miss path=%s wantRva=0x%X/0x%X",
                miPath, (unsigned)kRvaUserPrepare, (unsigned)kRvaFbPrepare);
            ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Miss, "no-mi");
            return false;
        }
        Log("SkipPrepare MI miss → vtable via GaBase+0x%X", (unsigned)kRvaUserPrepare);
    }

    const char* slotPath = "miss";
    void** slot = nullptr;
    size_t slotOff = 0;
    if (!FindPrepareVtableSlot(vtableKlass, mi, wantPtr, &slot, &slotOff, &slotPath)) {
        Log("SkipPrepare install fail — vtable slot not found mi=%s", miPath);
        ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Miss, "no-slot");
        return false;
    }

    void* orig = nullptr;
    if (!PatchVtableMethodPtr(slot, reinterpret_cast<void*>(&Hook_PrepareActionLayer), &orig)) {
        Log("SkipPrepare install fail — vtable patch off=0x%zX", slotOff);
        ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Miss, "patch");
        return false;
    }
    // 校验 orig 仍是 Prepare（防扫槽扫偏）
    const uint32_t origRva = PtrRva(orig);
    if (origRva && !IsKnownPrepareRva(origRva)) {
        RestoreVtableMethodPtr(slot, orig);
        Log("SkipPrepare install abort — orig rva=0x%X after patch off=0x%zX", origRva, slotOff);
        ReportSkipPrepLamp(x::runtime::anchor_lamps::AnchorLampCode::Miss, "bad-orig");
        return false;
    }

    gOrigPrepare.store(reinterpret_cast<FnPrepareActionLayer>(orig), std::memory_order_release);
    gSkipPrepareSlot = slot;
    gSkipPrepareKlass = vtableKlass;
    gSkipPrepareMi = mi;
    gSkipPrepareSlotOff = slotOff;

    if (mi) {
        void* miOrig = nullptr;
        if (!PatchMethodInfo(mi, reinterpret_cast<void*>(&Hook_PrepareActionLayer), &miOrig)) {
            Log("SkipPrepare MI patch skip (vtable ok) path=%s/%s", miPath, slotPath);
        }
    } else {
        Log("SkipPrepare MI patch skipped (no MI) path=%s/%s", miPath, slotPath);
    }

    snprintf(gSkipPreparePath, sizeof(gSkipPreparePath), "%s+%s", miPath, slotPath);
    gSkipPrepareInstalled.store(true, std::memory_order_release);

    const bool degraded = (std::strcmp(slotPath, "hint-slot32") == 0) ||
                          (std::strcmp(slotPath, "scan-soft") == 0) ||
                          (std::strncmp(miPath, "kind", 4) == 0) ||
                          (std::strncmp(miPath, "rva-raw", 7) == 0) ||
                          (std::strcmp(miPath, "ga-rva") == 0);
    ReportSkipPrepLamp(degraded ? x::runtime::anchor_lamps::AnchorLampCode::Degraded
                                : x::runtime::anchor_lamps::AnchorLampCode::Ok,
                       gSkipPreparePath);
    Log("SkipPrepare install ok klass=%p off=0x%zX rva=0x%X path=%s", vtableKlass, slotOff,
        (unsigned)(origRva ? origRva : miRva), gSkipPreparePath);
    return true;
}

bool LooksLikeHeapPtr(void* p) {
    const auto u = reinterpret_cast<uintptr_t>(p);
    return u > 0x10000ull && u < 0x00007FFFFFFFFFFFull;
}

// 实验：把当前动作层倒计时砍到 0，逼 Slot14 尽快推帧（不改 +0x118 / ActionSpeed）。
int CutLayerDelays(void* lu) {
    if (!lu) return 0;
    int n = 0;
    const size_t offs[] = {gOffActionLayerA, gOffActionLayerB};
    for (size_t off : offs) {
        void* layer = ReadPtr(lu, off);
        if (!LooksLikeHeapPtr(layer)) continue;
        const int rem = ReadI32(layer, kOffLayerDelay);
        if (rem > 0) {
            WriteI32(layer, kOffLayerDelay, 0);
            ++n;
        }
    }
    return n;
}

void* ResolveLocalUser() {
    if (x::runtime::managed_main::IsLoginFrozen()) return nullptr;
    x::features::ports::player_combat::CombatCtx ctx{};
    if (!x::features::ports::player_combat::QueryCombatCtx(ctx) || !ctx.ok) return nullptr;
    return ctx.localUser;
}

void* ResolveSecondaryStat() {
    if (!x::features::ports::world::IsPlayReady()) return nullptr;
    void* ss = x::ui::player::LocalSecondaryStat();
    return ProbeSsAlive(ss) ? ss : nullptr;
}

void ResetBoosterState() {
    gBoostSs = nullptr;
    gBoostBaseN = gBoostBaseR = gBoostBaseT = 0;
    gBoostLastN = gBoostLastR = gBoostLastT = 0;
    gBoostHeld = false;
    gBoostRebaseCnt = 0;
    gBoostRebaseLogAt = 0;
}

void ResetActionSpeedState() {
    gActSpSs = nullptr;
    gActSpBaseN = gActSpBaseT = 0;
    gActSpLastN = gActSpLastT = 0;
    gActSpHeld = false;
    gActSpRebaseCnt = 0;
    gActSpRebaseLogAt = 0;
}

void ResetPartyBoosterState() {
    gPartySs = nullptr;
    gPartyElem = nullptr;
    gPartyBaseV = gPartyLastV = 0;
    gPartyHeld = false;
    gPartyRebaseCnt = 0;
    gPartyRebaseLogAt = 0;
}

// TempStats[index].Value；失败返回 false（数组/槽空）。
bool ReadTempStatValue(void* ss, int index, int& outVal) {
    if (!ss || index < 0) return false;
    void* arr = ReadPtr(ss, gOffTempStats);
    if (!LooksLikeHeapPtr(arr)) return false;
    const int len = ReadI32(arr, kIl2CppArrayLenOff);
    if (index >= len) return false;
    void* elem = ReadPtr(arr, kIl2CppArrayDataOff + (size_t)index * sizeof(void*));
    if (!LooksLikeHeapPtr(elem)) return false;
    outVal = ReadI32(elem, kTempStatValueOff);
    return true;
}

bool ResolveTempStatElem(void* ss, int index, void** outElem) {
    if (!ss || !outElem || index < 0) return false;
    *outElem = nullptr;
    void* arr = ReadPtr(ss, gOffTempStats);
    if (!LooksLikeHeapPtr(arr)) return false;
    const int len = ReadI32(arr, kIl2CppArrayLenOff);
    if (index >= len) return false;
    void* elem = ReadPtr(arr, kIl2CppArrayDataOff + (size_t)index * sizeof(void*));
    if (!LooksLikeHeapPtr(elem)) return false;
    *outElem = elem;
    return true;
}

// 接管 nBooster_ 并按游戏钟续 tBooster_。ss 须已 ProbeSsAlive。返回本拍是否真的写了。
bool ApplyBooster(void* ss) {
    if (!ss) return false;
    // 没有游戏钟就不写：写不出能过 CheckByTime 的到期值，反而会被当场判过期清掉。
    const int gt = x::features::ports::skill::GetGameUpdateTimeMs();
    if (gt <= 0) return false;

    const int n = ReadI32(ss, gOffBoost);
    const int r = ReadI32(ss, gOffBoostReason);
    const int t = ReadI32(ss, gOffBoostExpire);
    // 非我方留下的值（首次接管 / 引擎清了 / 玩家自己嗑了加速药）→ 重登基线供关闭时奉还。
    if (!gBoostHeld || ss != gBoostSs || n != gBoostLastN || r != gBoostLastR ||
        t != gBoostLastT) {
        gBoostBaseN = n;
        gBoostBaseR = r;
        gBoostBaseT = t;
        ++gBoostRebaseCnt;
        const DWORD now = GetTickCount();
        if (!gBoostRebaseLogAt || now - gBoostRebaseLogAt >= 1000) {
            gBoostRebaseLogAt = now;
            Log("booster baseline ss=%p n=%d r=%d t=%d gt=%d held=%d rebase=%u", ss, n, r, t, gt,
                gBoostHeld ? 1 : 0, gBoostRebaseCnt);
        }
    }

    const int wantT = gt + kBoosterHoldMs;
    WriteI32(ss, gOffBoost, kBoosterValue);
    WriteI32(ss, gOffBoostExpire, wantT);
    // rBooster_ 不动：它是发起技能 ID，伪造会在 buff 列表里多出一个假图标。
    gBoostSs = ss;
    gBoostLastN = kBoosterValue;
    gBoostLastR = r;
    gBoostLastT = wantT;
    gBoostHeld = true;
    return true;
}

// 归还 booster。ssNow 为当前有效的 SecondaryStat；与接管时不是同一个（换图/换角色）就只丢状态不写。
void BoosterOff(void* ssNow) {
    if (!gBoostHeld) return;
    if (ssNow && ssNow == gBoostSs && ProbeSsAlive(ssNow)) {
        // 基线 n==0 表示接管前本就没 booster → 整组归零；否则原样奉还，由引擎自然判到期。
        const int n = gBoostBaseN;
        WriteI32(ssNow, gOffBoost, n);
        WriteI32(ssNow, gOffBoostReason, n ? gBoostBaseR : 0);
        WriteI32(ssNow, gOffBoostExpire, n ? gBoostBaseT : 0);
        Log("booster restore ss=%p n=%d r=%d t=%d", ssNow, n, n ? gBoostBaseR : 0,
            n ? gBoostBaseT : 0);
    } else {
        Log("booster drop (ss %p -> %p)", gBoostSs, ssNow);
    }
    ResetBoosterState();
}

// 对齐 GetSpeed / GetActionSpeed（未 clamp）：(+80 if +84>=0 else 0) + Max(+84, Dash)。
int ComputeActionSpeedRaw(void* ss, int* outN80 = nullptr, int* outDash = nullptr) {
    if (!ss) return 100;
    const int n80 = ReadI32(ss, kOffSpeedBase);
    const int n84 = ReadI32(ss, gOffSpeedN);
    int dash = 0;
    (void)ReadTempStatValue(ss, kDashSpeedIndex, dash);
    if (outN80) *outN80 = n80;
    if (outDash) *outDash = dash;
    const int basePart = (n84 >= 0) ? n80 : 0;
    const int add = (n84 > dash) ? n84 : dash;
    return basePart + add;
}

int ClampActionSpeed(int raw) {
    if (raw < kActionSpeedClampLo) return kActionSpeedClampLo;
    if (raw > kActionSpeedClampHi) return kActionSpeedClampHi;
    return raw;
}

// A 系：写 nSpeed_=+40，并按游戏钟续 tSpeed_（即使 CheckByTime 不扫，也方便对照 Decode）。
// 注意：Prepare 再 clamp [70,140]。若 nSpeed@+80 已近 140，加 +40 后仍被夹死 → 体感无变化（≠写失败）。
bool ApplyActionSpeed(void* ss) {
    if (!ss) return false;
    const int gt = x::features::ports::skill::GetGameUpdateTimeMs();
    if (gt <= 0) return false;

    const int n = ReadI32(ss, gOffSpeedN);
    const int t = ReadI32(ss, gOffSpeedT);
    if (!gActSpHeld || ss != gActSpSs || n != gActSpLastN || t != gActSpLastT) {
        gActSpBaseN = n;
        gActSpBaseT = t;
        ++gActSpRebaseCnt;
        int n80 = 0, dash = 0;
        const int rawBefore = ComputeActionSpeedRaw(ss, &n80, &dash);
        const int aSpBefore = ClampActionSpeed(rawBefore);
        const DWORD now = GetTickCount();
        if (!gActSpRebaseLogAt || now - gActSpRebaseLogAt >= 1000) {
            gActSpRebaseLogAt = now;
            Log("actionSpeed baseline ss=%p n80=%d nSpeed_=%d dash=%d raw=%d aSp=%d t=%d gt=%d "
                "held=%d rebase=%u",
                ss, n80, n, dash, rawBefore, aSpBefore, t, gt, gActSpHeld ? 1 : 0, gActSpRebaseCnt);
            if (aSpBefore >= kActionSpeedClampHi) {
                Log("actionSpeed noop warn: Prepare already at clamp aSp=%d (n80=%d) — A系无法再加速；"
                    "体感请用 PartyBooster/nBooster_（B系 degree）",
                    aSpBefore, n80);
            }
        }
    }

    const int wantT = gt + kActionSpeedHoldMs;
    WriteI32(ss, gOffSpeedN, kActionSpeedNValue);
    WriteI32(ss, gOffSpeedT, wantT);
    gActSpSs = ss;
    gActSpLastN = kActionSpeedNValue;
    gActSpLastT = wantT;
    gActSpHeld = true;
    return true;
}

void ActionSpeedOff(void* ssNow) {
    if (!gActSpHeld) return;
    if (ssNow && ssNow == gActSpSs && ProbeSsAlive(ssNow)) {
        WriteI32(ssNow, gOffSpeedN, gActSpBaseN);
        WriteI32(ssNow, gOffSpeedT, gActSpBaseN ? gActSpBaseT : 0);
        Log("actionSpeed restore ss=%p nSpeed_=%d t=%d", ssNow, gActSpBaseN,
            gActSpBaseN ? gActSpBaseT : 0);
    } else {
        Log("actionSpeed drop (ss %p -> %p)", gActSpSs, ssNow);
    }
    ResetActionSpeedState();
}

bool ApplyPartyBooster(void* ss) {
    if (!ss) return false;
    void* elem = nullptr;
    if (!ResolveTempStatElem(ss, kPartyBoosterIndex, &elem) || !elem) {
        const DWORD now = GetTickCount();
        if (!gPartyMissLogAt || now - gPartyMissLogAt >= 2000) {
            gPartyMissLogAt = now;
            Log("partyBooster miss TempStats[%d] ss=%p", kPartyBoosterIndex, ss);
        }
        return false;
    }
    const int want = gPartyBoosterValue.load(std::memory_order_acquire);
    const int v = ReadI32(elem, kTempStatValueOff);
    if (!gPartyHeld || ss != gPartySs || elem != gPartyElem || v != gPartyLastV) {
        gPartyBaseV = v;
        ++gPartyRebaseCnt;
        const DWORD now = GetTickCount();
        if (!gPartyRebaseLogAt || now - gPartyRebaseLogAt >= 1000) {
            gPartyRebaseLogAt = now;
            Log("partyBooster baseline ss=%p elem=%p v=%d want=%d held=%d rebase=%u", ss, elem, v,
                want, gPartyHeld ? 1 : 0, gPartyRebaseCnt);
        }
    }
    WriteI32(elem, kTempStatValueOff, want);
    gPartySs = ss;
    gPartyElem = elem;
    gPartyLastV = want;
    gPartyHeld = true;
    return true;
}

void PartyBoosterOff(void* ssNow) {
    if (!gPartyHeld) return;
    if (ssNow && ssNow == gPartySs && ProbeSsAlive(ssNow) && gPartyElem) {
        void* elem = nullptr;
        if (ResolveTempStatElem(ssNow, kPartyBoosterIndex, &elem) && elem == gPartyElem) {
            WriteI32(elem, kTempStatValueOff, gPartyBaseV);
            Log("partyBooster restore ss=%p v=%d", ssNow, gPartyBaseV);
        } else {
            Log("partyBooster drop elem moved (ss %p)", ssNow);
        }
    } else {
        Log("partyBooster drop (ss %p -> %p)", gPartySs, ssNow);
    }
    ResetPartyBoosterState();
}

DWORD WINAPI Worker(LPVOID) {
    OpenLog();
    EnsureFieldOffsets();
    Log("worker start (clearBusy | nBooster_=%d | nSpeed_=%d | partyV=%d; landGrace=%ums; "
        "skipPrepLandGrace=%ums)",
        kBoosterValue, kActionSpeedNValue, gPartyBoosterValue.load(std::memory_order_relaxed),
        (unsigned)kLandGraceMs, (unsigned)kSkipPrepareLandGraceMs);
    DWORD lastRebind = 0;
    DWORD lastLog = 0;
    DWORD landGraceUntil = 0;  // PlayReady/换皮后宽限，禁止写忙锁 / 禁止武装 SkipPrepare
    void* lu = nullptr;
    void* ss = nullptr;
    void* lastMyUser = nullptr;
    bool wasPlayReady = false;
    // 稳态行签名：变化才打；无变化走 kLogIdleMs 心跳。
    int prevHoldSig = INT_MIN;
    int prevOnSig = INT_MIN;
    int prevSkipSig = INT_MIN;

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const bool accelOn = gDesired.load(std::memory_order_acquire);
        const bool boostOn = gBoosterDesired.load(std::memory_order_acquire);
        const bool actSpOn = gActionSpeedDesired.load(std::memory_order_acquire);
        const bool partyOn = gPartyBoosterDesired.load(std::memory_order_acquire);
        const bool cutOn = gCutLayerDesired.load(std::memory_order_acquire);
        const bool skipPrepOn = gSkipPrepareDesired.load(std::memory_order_acquire);
        const bool breakFloorOn = gBreakDegreeFloorDesired.load(std::memory_order_acquire);
        // GA .data 种子：与 PlayReady / Party 无关，每拍同步（开写 / 关还原）。
        SyncDegreeFloorSeed(breakFloorOn);
        const bool anyWrite = accelOn || boostOn || actSpOn || partyOn || cutOn;
        // 跳过 Prepare 也要跟落地窗，不能只在「加速/砍层」开着时才追踪 PlayReady。
        const bool needLand = anyWrite || skipPrepOn;
        const DWORD now = GetTickCount();

        if (skipPrepOn) {
            EnsureFieldOffsets();
            EnsureSkipPrepareHook();
        } else {
            gSkipPrepareArmed.store(false, std::memory_order_release);
        }

        if (needLand) {
            EnsureFieldOffsets();
            if (!x::features::ports::world::IsPlayReady()) {
                if (wasPlayReady) {
                    wasPlayReady = false;
                    landGraceUntil = 0;
                    lastMyUser = nullptr;
                    gSkipPrepareArmed.store(false, std::memory_order_release);
                    Log("play not ready — pause writes");
                } else {
                    gSkipPrepareArmed.store(false, std::memory_order_release);
                }
                // 离场前尽量把数值槽还回去；ss 已失效则只丢状态。
                BoosterOff(ss);
                ActionSpeedOff(ss);
                PartyBoosterOff(ss);
                lu = nullptr;
                ss = nullptr;
                Sleep(kRefreshMs);
                continue;
            }
            if (!wasPlayReady) {
                wasPlayReady = true;
                const DWORD grace =
                    skipPrepOn ? (std::max)(kLandGraceMs, kSkipPrepareLandGraceMs) : kLandGraceMs;
                landGraceUntil = now + grace;
                gSkipPrepareArmed.store(false, std::memory_order_release);
                Log("play ready — land grace %ums (skipPrep=%d)", (unsigned)grace,
                    skipPrepOn ? 1 : 0);
            }

            void* wm = x::features::ports::world::PeekWorldManager();
            void* myUser = x::ui::player::LocalMyUser();
            if (myUser != lastMyUser) {
                if (lastMyUser || myUser) {
                    const DWORD grace = skipPrepOn
                                            ? (std::max)(kLandGraceMs, kSkipPrepareLandGraceMs)
                                            : kLandGraceMs;
                    landGraceUntil = now + grace;
                    gSkipPrepareArmed.store(false, std::memory_order_release);
                    Log("MyUser %p -> %p — land grace %ums", lastMyUser, myUser, (unsigned)grace);
                }
                lastMyUser = myUser;
                lu = nullptr;
                ss = nullptr;
            }
            const bool luStale = lu && (!myUser || lu != myUser);
            const bool needSs = boostOn || actSpOn || partyOn;
            if (!lu || (needSs && !ss) || luStale || now - lastRebind >= kRebindMs) {
                if (luStale) Log("LocalUser stale %p -> wm.MyUser=%p, rebind", lu, myUser);
                if (luStale && !myUser) {
                    lu = nullptr;
                } else {
                    lu = ResolveLocalUser();
                }
                if (needSs || !ss) ss = ResolveSecondaryStat();
                lastRebind = now;
            } else if (ss && !ProbeSsAlive(ss)) {
                ss = nullptr;
            }

            // 数值槽不吃落地宽限（宽限是为忙锁/SkipPrepare 可视写入设的）。
            if (boostOn) {
                if (ss) ApplyBooster(ss);
            } else {
                BoosterOff(ss);
            }
            if (actSpOn) {
                if (ss) ApplyActionSpeed(ss);
            } else {
                ActionSpeedOff(ss);
            }
            if (partyOn) {
                if (ss) ApplyPartyBooster(ss);
            } else {
                PartyBoosterOff(ss);
            }

            if (gSkipPrepareNeedGrace.exchange(false, std::memory_order_acq_rel)) {
                landGraceUntil =
                    now + (std::max)(kLandGraceMs, kSkipPrepareLandGraceMs);
                gSkipPrepareArmed.store(false, std::memory_order_release);
                Log("SkipPrepare rising-edge — land grace %ums",
                    (unsigned)(std::max)(kLandGraceMs, kSkipPrepareLandGraceMs));
            }

            const bool inGrace = landGraceUntil && static_cast<int>(now - landGraceUntil) < 0;
            const bool fireHeld = x::features::ports::attack::IsFireSuppressed();
            const bool skipInstalled = gSkipPrepareInstalled.load(std::memory_order_acquire);
            // SkipPrepare 武装：已装钩 + PlayReady + 过落地窗 + 有皮；suppress 时卸武装透传。
            const bool skipArmed = skipPrepOn && skipInstalled && !inGrace && !!myUser && !!lu &&
                                   !fireHeld;
            gSkipPrepareArmed.store(skipArmed, std::memory_order_release);

            if (inGrace || !lu || fireHeld) {
                const int holdSig = (inGrace ? 1 : 0) | ((fireHeld ? 1 : 0) << 1) |
                                    ((lu ? 1 : 0) << 2) | ((skipArmed ? 1 : 0) << 3) |
                                    ((accelOn ? 1 : 0) << 4) | ((boostOn ? 1 : 0) << 5) |
                                    ((cutOn ? 1 : 0) << 6) | ((actSpOn ? 1 : 0) << 7) |
                                    ((partyOn ? 1 : 0) << 8);
                const bool changed = holdSig != prevHoldSig;
                const bool due =
                    !lastLog || (changed && now - lastLog >= kLogChangeMs) ||
                    (!changed && now - lastLog >= kLogIdleMs);
                if (due) {
                    lastLog = now;
                    prevHoldSig = holdSig;
                    Log("hold grace=%d suppress=%d lu=%p skipArmed=%d accel=%d boost=%d "
                        "actSp=%d party=%d cut=%d",
                        inGrace ? 1 : 0, fireHeld ? 1 : 0, lu, skipArmed ? 1 : 0,
                        accelOn ? 1 : 0, boostOn ? 1 : 0, actSpOn ? 1 : 0, partyOn ? 1 : 0,
                        cutOn ? 1 : 0);
                }
                Sleep(kRefreshMs);
                continue;
            }

            // SkipPrepare 单独开时也必须清忙锁：无攻击层则 Slot14 永不写 +0x118=-1。
            // SetAttackAction 可能在 Prepare 返回后仍写入 actionIdx，故 worker 周期兜底。
            if (accelOn || skipArmed) {
                WriteI32(lu, gOffActionBusy, -1);
            }

            int cutN = 0;
            if (cutOn && !skipPrepOn) cutN = CutLayerDelays(lu);

            if (accelOn || boostOn || actSpOn || partyOn || cutOn || skipArmed || breakFloorOn) {
                const int busy = ReadI32(lu, gOffActionBusy);
                const int slow = ss ? ReadI32(ss, gOffSlow) : 0;
                const int tSlow = ss ? ReadI32(ss, gOffSlowExpire) : 0;
                const int boost = ss ? ReadI32(ss, gOffBoost) : 0;
                const int tBoost = ss ? ReadI32(ss, gOffBoostExpire) : 0;
                const int nSp = ss ? ReadI32(ss, gOffSpeedN) : 0;
                int n80 = 0;
                const int aSp = ss ? ClampActionSpeed(ComputeActionSpeedRaw(ss, &n80)) : 100;
                int partyV = 0;
                const bool partyOk = ss && ReadTempStatValue(ss, kPartyBoosterIndex, partyV);
                const int wpn = ReadI32(lu, kOffLuWeaponDegree);
                const bool floorBroken =
                    gBreakDegreeFloorApplied.load(std::memory_order_acquire);
                const int degLo =
                    floorBroken ? gBreakDegreeFloorAppliedLo.load(std::memory_order_acquire) : 2;
                const int deg =
                    (std::min)(10, (std::max)(degLo, wpn + boost + (partyOk ? partyV : 0)));
                const int onSig =
                    busy + 3 * slow + 5 * tSlow + 7 * boost + 11 * tBoost + 13 * wpn + 17 * deg +
                    19 * cutN + 23 * nSp + 29 * partyV + 31 * n80 + 37 * aSp +
                    (gBoostHeld ? 41 : 0) + (gActSpHeld ? 43 : 0) + (gPartyHeld ? 47 : 0) +
                    (accelOn ? 53 : 0) + (boostOn ? 59 : 0) + (actSpOn ? 61 : 0) +
                    (partyOn ? 67 : 0) + ((cutOn && !skipPrepOn) ? 71 : 0) +
                    (skipArmed ? 73 : 0) + (gBoostRebaseCnt * 79) + (gActSpRebaseCnt * 83) +
                    (gPartyRebaseCnt * 89) + (floorBroken ? 97 : 0);
                const bool changed = onSig != prevOnSig;
                const bool due =
                    !lastLog || (changed && now - lastLog >= kLogChangeMs) ||
                    (!changed && now - lastLog >= kLogIdleMs);
                if (due) {
                    lastLog = now;
                    prevOnSig = onSig;
                    Log("on lu=%p busy=%d ss=%p slow=%d tSlow=%d boost=%d tBoost=%d n80=%d nSp=%d "
                        "aSp=%d party=%d/%d heldB=%d heldS=%d heldP=%d wpn=%d deg=%d x%.2f "
                        "accel=%d bst=%d actSp=%d partyOn=%d cut=%d cutN=%d skipArmed=%d "
                        "brkFloor=%d skipHits=%u pass=%u",
                        lu, busy, ss, slow, tSlow, boost, tBoost, n80, nSp, aSp, partyOk ? 1 : 0,
                        partyV, gBoostHeld ? 1 : 0, gActSpHeld ? 1 : 0, gPartyHeld ? 1 : 0, wpn, deg,
                        (deg + 10) / 16.0, accelOn ? 1 : 0, boostOn ? 1 : 0, actSpOn ? 1 : 0,
                        partyOn ? 1 : 0, (cutOn && !skipPrepOn) ? 1 : 0, cutN, skipArmed ? 1 : 0,
                        floorBroken ? 1 : 0,
                        gSkipPrepareHits.load(std::memory_order_relaxed),
                        gSkipPreparePassHits.load(std::memory_order_relaxed));
                }
            } else {
                const int skipSig = (skipArmed ? 1 : 0);
                const bool changed = skipSig != prevSkipSig;
                const bool due =
                    !lastLog || (changed && now - lastLog >= kLogChangeMs) ||
                    (!changed && now - lastLog >= kLogIdleMs);
                if (due) {
                    lastLog = now;
                    prevSkipSig = skipSig;
                    Log("SkipPrepare armed=%d hits=%u pass=%u lu=%p", skipArmed ? 1 : 0,
                        gSkipPrepareHits.load(std::memory_order_relaxed),
                        gSkipPreparePassHits.load(std::memory_order_relaxed), lu);
                }
            }
            Sleep(kRefreshMs);
        } else {
            gSkipPrepareArmed.store(false, std::memory_order_release);
            BoosterOff(ss);
            ActionSpeedOff(ss);
            PartyBoosterOff(ss);
            lu = nullptr;
            ss = nullptr;
            lastMyUser = nullptr;
            wasPlayReady = false;
            landGraceUntil = 0;
            Sleep(50);
        }
    }

    gSkipPrepareArmed.store(false, std::memory_order_release);
    BoosterOff(ss);
    ActionSpeedOff(ss);
    PartyBoosterOff(ss);
    RestoreDegreeFloorSeed();
    UninstallSkipPrepareHook();
    Log("worker stop");
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    Log("Init");
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) gWorkerThread.store(th, std::memory_order_release);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

void SetDesired(bool on) {
    const bool was = gDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on) Log("SetDesired on=%d", on ? 1 : 0);
}

bool IsDesired() { return gDesired.load(std::memory_order_acquire); }

void SetBoosterDesired(bool on) {
    if (!xcat::kAttackAccelBoosterUserEnabled) on = false;
    const bool was = gBoosterDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on) Log("SetBoosterDesired on=%d (nBooster_=%d)", on ? 1 : 0, kBoosterValue);
}

bool IsBoosterDesired() { return gBoosterDesired.load(std::memory_order_acquire); }

void SetCutLayerDesired(bool on) {
    const bool was = gCutLayerDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on) Log("SetCutLayerDesired on=%d (experimental)", on ? 1 : 0);
}

bool IsCutLayerDesired() { return gCutLayerDesired.load(std::memory_order_acquire); }

void SetSkipPrepareDesired(bool on) {
    const bool was = gSkipPrepareDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on) {
        Log("SetSkipPrepareDesired on=%d (experimental anti-drift)", on ? 1 : 0);
        if (on && !was) {
            gSkipPrepareNeedGrace.store(true, std::memory_order_release);
            gSkipPrepareArmed.store(false, std::memory_order_release);
        }
        if (!on) gSkipPrepareArmed.store(false, std::memory_order_release);
    }
}

bool IsSkipPrepareDesired() { return gSkipPrepareDesired.load(std::memory_order_acquire); }

void SetActionSpeedDesired(bool on) {
    const bool was = gActionSpeedDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on) Log("SetActionSpeedDesired on=%d (nSpeed_=%d)", on ? 1 : 0, kActionSpeedNValue);
}

bool IsActionSpeedDesired() { return gActionSpeedDesired.load(std::memory_order_acquire); }

void SetPartyBoosterDesired(bool on) {
    const bool was = gPartyBoosterDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on)
        Log("SetPartyBoosterDesired on=%d (TempStats[%d].Value=%d)", on ? 1 : 0, kPartyBoosterIndex,
            gPartyBoosterValue.load(std::memory_order_relaxed));
}

bool IsPartyBoosterDesired() { return gPartyBoosterDesired.load(std::memory_order_acquire); }

void SetPartyBoosterValue(int v) {
    const int clamped = static_cast<int>(xcat::ClampAttackAccelPartyBoosterValue(v));
    const int was = gPartyBoosterValue.exchange(clamped, std::memory_order_acq_rel);
    if (was != clamped)
        Log("SetPartyBoosterValue %d -> %d", was, clamped);
}

int PartyBoosterValue() { return gPartyBoosterValue.load(std::memory_order_acquire); }

void SetBreakDegreeFloorDesired(bool on) {
    const bool was = gBreakDegreeFloorDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on)
        Log("SetBreakDegreeFloorDesired on=%d (CalcWeaponAttackSpeedTier lo→%d)", on ? 1 : 0,
            gBreakDegreeFloorLo.load(std::memory_order_relaxed));
}

bool IsBreakDegreeFloorDesired() {
    return gBreakDegreeFloorDesired.load(std::memory_order_acquire);
}

void SetBreakDegreeFloorLo(int lo) {
    const int clamped = static_cast<int>(xcat::ClampAttackAccelBreakDegreeFloorLo(lo));
    const int was = gBreakDegreeFloorLo.exchange(clamped, std::memory_order_acq_rel);
    if (was != clamped) Log("SetBreakDegreeFloorLo %d -> %d", was, clamped);
}

int BreakDegreeFloorLo() { return gBreakDegreeFloorLo.load(std::memory_order_acquire); }

bool QueryActionBusy(void* localUser, int& outBusy) {
    if (!localUser) return false;
    // 幂等（gFieldOffResolved 早退）。worker 启动时已解析过，这里只兜住「worker 尚未起来」。
    EnsureFieldOffsets();
    if (!gFieldOffResolved.load(std::memory_order_acquire)) return false;
    // 不复用 ReadI32：它把读故障也返回 0，而 0 在本语义下 = 忙，会变成永久禁止出刀。
    int v = 0;
    __try {
        v = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(localUser) + gOffActionBusy);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    outBusy = v;
    return true;
}

bool QueryActionIndex(void* localUser, int& outActionIdx) {
    int busy = -1;
    if (!QueryActionBusy(localUser, busy) || busy < 0) return false;
    outActionIdx = busy;
    return true;
}

// 对照 Dumps/cms_cw/dump.cs · Msc.Game.Object.Avatar.ActionType（与 busy 实测 5/6/7/16/17 吻合）。
bool IsRangedShootAction(int actionIdx) {
    if (actionIdx >= 22 && actionIdx <= 27) return true;  // Shoot1..ShooTf
    if (actionIdx == 48) return true;                     // Shoot6
    return false;
}

bool IsMeleeWeaponAction(int actionIdx) {
    if (actionIdx >= 5 && actionIdx <= 21) return true;  // SwingO1..StabTf
    if (actionIdx == 32) return true;                    // ProneStab
    return false;
}

bool ClearActionBusy(void* localUser) {
    if (!localUser) return false;
    EnsureFieldOffsets();
    if (!gFieldOffResolved.load(std::memory_order_acquire)) return false;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(localUser) + gOffActionBusy) = -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool QueryActionLayerDelayRemain(void* localUser, int& outRemain) {
    if (!localUser) return false;
    EnsureFieldOffsets();
    if (!gFieldOffResolved.load(std::memory_order_acquire)) return false;
    int best = 0;
    bool any = false;
    const size_t offs[] = {gOffActionLayerA, gOffActionLayerB};
    for (size_t off : offs) {
        void* layer = nullptr;
        __try {
            layer = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(localUser) + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (!LooksLikeHeapPtr(layer)) continue;
        int rem = 0;
        __try {
            rem = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(layer) + kOffLayerDelay);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (rem > best) best = rem;
        any = true;
    }
    if (!any) return false;
    outRemain = best;
    return true;
}

// ActionLayer+0x20 → Il2Cpp int[]：Prepare 写入的 delay'（已 ×100/ActionSpeed）。
bool SumLayerDelayArr(void* layer, int& outSum) {
    outSum = 0;
    if (!LooksLikeHeapPtr(layer)) return false;
    void* arr = nullptr;
    __try {
        arr = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(layer) + kOffLayerDelayArr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!LooksLikeHeapPtr(arr)) return false;
    const size_t offLen = x::runtime::il2cpp_container::OffArrayMaxLength();
    const size_t offData = x::runtime::il2cpp_container::OffArrayData();
    int n = 0;
    __try {
        n = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(arr) + offLen);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (n <= 0 || n > 256) return false;  // 动作帧数上限护栏
    int sum = 0;
    __try {
        const int* data = reinterpret_cast<const int*>(reinterpret_cast<uint8_t*>(arr) + offData);
        for (int i = 0; i < n; ++i) {
            const int d = data[i];
            if (d > 0 && d < 100000) sum += d;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (sum <= 0) return false;
    outSum = sum;
    return true;
}

bool QueryActionLayerDelaySum(void* localUser, int& outSum) {
    if (!localUser) return false;
    EnsureFieldOffsets();
    if (!gFieldOffResolved.load(std::memory_order_acquire)) return false;
    int best = 0;
    bool any = false;
    const size_t offs[] = {gOffActionLayerA, gOffActionLayerB};
    for (size_t off : offs) {
        void* layer = nullptr;
        __try {
            layer = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(localUser) + off);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        int sum = 0;
        if (!SumLayerDelayArr(layer, sum)) continue;
        if (sum > best) best = sum;
        any = true;
    }
    if (!any) return false;
    outSum = best;
    return true;
}

bool QueryActionLayerAnimMs(void* localUser, DWORD& outMs) {
    int sum = 0;
    if (!QueryActionLayerDelaySum(localUser, sum)) return false;
    const DWORD ms = DelaySumToMs(sum);
    if (ms < 40u || ms > 5000u) return false;
    outMs = ms;
    return true;
}

DWORD DelayUnitsToAnimMs(int delaySum) { return DelaySumToMs(delaySum); }

int ScaleDelayByActionSpeed(int baseSum, int actionSpeed) {
    if (baseSum <= 0) return 0;
    int sp = actionSpeed;
    if (sp < 70) sp = 70;
    if (sp > 140) sp = 140;
    return static_cast<int>((static_cast<int64_t>(baseSum) * 100) / sp);
}

int UnscaleDelayByActionSpeed(int scaledSum, int actionSpeed) {
    if (scaledSum <= 0) return 0;
    int sp = actionSpeed;
    if (sp < 70) sp = 70;
    if (sp > 140) sp = 140;
    return static_cast<int>((static_cast<int64_t>(scaledSum) * sp) / 100);
}

int AnimMsToBaseSum(DWORD animMs, int actionSpeed) {
    // 与 DelaySumToMs 互逆：闸门 ms≡scaled delay 上界 → scaled≈animMs，再反解 base。
    if (animMs < 40u || animMs > 5000u) return 0;
    const int scaled = static_cast<int>(animMs);
    if (scaled <= 0) return 0;
    return UnscaleDelayByActionSpeed(scaled, actionSpeed);
}

bool QueryActionSpeed(void* localUser, int& outSpeed) {
    (void)localUser;
    EnsureFieldOffsets();
    void* ss = ResolveSecondaryStat();
    if (!ss) {
        outSpeed = 100;
        return true;  // 无 SS 时按名义 100，避免挡离线估算
    }
    int speed = 100;
    __try {
        // Prepare：nSlow_@1BC != 0 则绝对覆盖 GetActionSpeed。
        const int ov = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ss) + gOffSlow);
        if (ov != 0) {
            speed = ov;
        } else {
            speed = ComputeActionSpeedRaw(ss);
            if (speed == 0) speed = 100;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outSpeed = 100;
        return false;
    }
    outSpeed = ClampActionSpeed(speed);
    return true;
}

namespace {

std::once_flag gOfflineActionOnce;
std::unordered_map<std::string, int> gActionBasePos;  // action → delay_sum_pos
std::unordered_map<int, std::string> gSkillAction;     // skillId → action name
bool gOfflineActionOk = false;

void LoadOfflineActionTablesOnce() {
    std::call_once(gOfflineActionOnce, [] {
        std::string base = x::runtime::GetBinDir() ? x::runtime::GetBinDir() : "";
        if (!base.empty() && base.back() != '\\' && base.back() != '/') base += '\\';
        const std::string actPath = base + "dataservice\\action_delay_base.tsv";
        const std::string skPath = base + "dataservice\\skill_action.tsv";
        {
            std::ifstream f(actPath, std::ios::binary);
            if (!f) {
                x::runtime::LogW("AttackAccel", "action_delay_base.tsv missing path=%s",
                                 actPath.c_str());
            } else {
                std::string line;
                size_t n = 0;
                while (std::getline(f, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty() || line[0] == '#') continue;
                    // action \t frames \t delay_sum_pos \t delay_sum_all
                    const size_t t0 = line.find('\t');
                    if (t0 == std::string::npos) continue;
                    const size_t t1 = line.find('\t', t0 + 1);
                    if (t1 == std::string::npos) continue;
                    const size_t t2 = line.find('\t', t1 + 1);
                    if (t2 == std::string::npos) continue;
                    const std::string act = line.substr(0, t0);
                    const int spos = atoi(line.c_str() + t1 + 1);
                    if (act.empty() || spos <= 0) continue;
                    gActionBasePos[act] = spos;
                    ++n;
                }
                x::runtime::LogI("AttackAccel", "action_delay_base loaded n=%zu path=%s", n,
                                 actPath.c_str());
            }
        }
        {
            std::ifstream f(skPath, std::ios::binary);
            if (!f) {
                x::runtime::LogW("AttackAccel", "skill_action.tsv missing path=%s", skPath.c_str());
            } else {
                std::string line;
                size_t n = 0;
                while (std::getline(f, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty() || line[0] == '#') continue;
                    const size_t t0 = line.find('\t');
                    if (t0 == std::string::npos) continue;
                    const int sid = atoi(line.c_str());
                    std::string act = line.substr(t0 + 1);
                    const size_t t1 = act.find('\t');
                    if (t1 != std::string::npos) act.resize(t1);
                    if (sid <= 0 || act.empty()) continue;
                    gSkillAction[sid] = act;
                    ++n;
                }
                x::runtime::LogI("AttackAccel", "skill_action loaded n=%zu path=%s", n,
                                 skPath.c_str());
            }
        }
        gOfflineActionOk = !gActionBasePos.empty();
    });
}

bool AnimMsFromBaseSum(void* localUser, int baseSum, DWORD& outMs) {
    if (baseSum <= 0) return false;
    int speed = 100;
    (void)QueryActionSpeed(localUser, speed);
    const int scaled = ScaleDelayByActionSpeed(baseSum, speed);
    const DWORD ms = DelaySumToMs(scaled);
    if (ms < 40u || ms > 5000u) return false;
    outMs = ms;
    return true;
}

}  // namespace

bool LookupOfflineSkillBaseSum(int skillId, int& outBaseSum);
bool LookupOfflineSkillAnimMs(void* localUser, int skillId, DWORD& outMs);
bool LookupOfflineActionAnimMs(void* localUser, const char* actionName, DWORD& outMs);

bool LookupOfflineSkillAnimMs(void* localUser, int skillId, DWORD& outMs) {
    if (skillId <= 0) return false;
    int baseSum = 0;
    if (!LookupOfflineSkillBaseSum(skillId, baseSum)) return false;
    return AnimMsFromBaseSum(localUser, baseSum, outMs);
}

bool LookupOfflineSkillBaseSum(int skillId, int& outBaseSum) {
    if (skillId <= 0) return false;
    LoadOfflineActionTablesOnce();
    if (!gOfflineActionOk) return false;
    const auto sit = gSkillAction.find(skillId);
    if (sit == gSkillAction.end()) return false;
    const auto ait = gActionBasePos.find(sit->second);
    if (ait == gActionBasePos.end()) return false;
    outBaseSum = ait->second;
    return true;
}

bool LookupOfflineActionAnimMs(void* localUser, const char* actionName, DWORD& outMs) {
    if (!actionName || !actionName[0]) return false;
    LoadOfflineActionTablesOnce();
    if (!gOfflineActionOk) return false;
    const auto it = gActionBasePos.find(actionName);
    if (it == gActionBasePos.end()) return false;
    return AnimMsFromBaseSum(localUser, it->second, outMs);
}

bool QueryAttackSpeedDegree(void* localUser, int& outDegree) {
    if (!localUser) return false;
    EnsureFieldOffsets();
    int wpn = 6;  // 无武器默认档（P0b / 战斗点自洽）
    __try {
        wpn = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(localUser) + kOffLuWeaponDegree);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    int boost = 0;
    int party = 0;
    if (gFieldOffResolved.load(std::memory_order_acquire)) {
        void* ss = ResolveSecondaryStat();
        if (ss) {
            __try {
                boost = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ss) + gOffBoost);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                boost = 0;
            }
            (void)ReadTempStatValue(ss, kPartyBoosterIndex, party);
        }
    }
    int deg = wpn + boost + party;
    const int degLo = gBreakDegreeFloorApplied.load(std::memory_order_acquire)
                          ? gBreakDegreeFloorAppliedLo.load(std::memory_order_acquire)
                          : 2;
    if (deg < degLo) deg = degLo;
    if (deg > 10) deg = 10;
    outDegree = deg;
    return true;
}

DWORD EstimateDamageDelayScaleMs(void* localUser, DWORD baseMsAtDegree6) {
    const DWORD base = baseMsAtDegree6 ? baseMsAtDegree6 : 120u;
    int deg = 6;
    if (!QueryAttackSpeedDegree(localUser, deg)) return base;
    // degree=6 → ×16/16；degree=2 → ×12/16；degree=10 → ×20/16（相对缩放，非官方绝对 ms）
    const DWORD ms = static_cast<DWORD>((static_cast<uint64_t>(base) * (deg + 10)) / 16u);
    return ms < 40u ? 40u : ms;
}

}  // namespace attack_accel
}  // namespace features
}  // namespace x
