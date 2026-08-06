// TWMS Classic — attack_accel（攻击加速）
//
// 启用后周期：
//   - LocalUser+0x118=-1（跳过动作等待；字段哈希 b4e36b7c… 防漂移）
//   - CutLayerDelays（砍动作层 delay）
// 出刀频率由 simpleCombatAttackIntervalMs（面板「间隔」，默认 50，下限 1）控制。
// 禁止 GA .text E9。
//
// 2026-08-04 撤销「Prepare 绝对攻速」写入：SecondaryStat+0x1BC/0x1C4 经 IDA 实证
// 是 nSlow_/tSlow_（减速 debuff），不是攻速槽。Prepare(RVA 0xFDE830) 在
// 0x7FFB84A5EAF3 处 `mov eax,[r14+1BCh]` + `cmovnz ebp,eax`，nSlow_ 非 0 即顶掉
// GetSpeed()，把 140 烘进每一个动作层；且 tSlow_ 当时按 GetTickCount 写（应为
// WorldManager.GetUpdateTime 游戏钟），CheckByTime 永不判到期 → 整局视觉层错乱。
// 两个偏移现仅作只读诊断，用于确认本模块不再污染该字段。
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
// 实验·跳过 Prepare：改 LocalUser 虚表槽（SetAttackAction @RVA 0xFDAF10 虚调 Prepare），
// 不碰 GA .text。关开关时 hook 仍在，走 orig 透传。
// remount 2026-08-04：Prepare RVA/哈希已对 dump.cs.runtime；字段偏移经 IDA+dump 复核。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_accel.h"

#include "../../runtime/dbg_log_file.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../ports/attack_input_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../../ui/player_vitals.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace x {
namespace features {
namespace attack_accel {
namespace {

// 默认/兜底偏移（dump.cs.runtime + LocalUser_Prepare [r14+1BCh] 实锤；种子解出阈 0）
constexpr size_t kOffActionBusyHint = 0x118;
constexpr size_t kOffActionLayerAHint = 0x120;
constexpr size_t kOffActionLayerBHint = 0x128;
constexpr size_t kOffLayerDelay = 0x14;
// WM.MyUser / SecondaryStat → x::ui::player SSOT（hash 防漂）
// nSlow_ / tSlow_ —— 只读诊断，本模块不再写（见文件头 2026-08-04 说明）。
constexpr size_t kOffSlowHint = 0x1BC;
constexpr size_t kOffSlowExpireHint = 0x1C4;
// nBooster_ / rBooster_ / tBooster_ —— 真攻速槽，accel 开启时接管（见文件头）。
constexpr size_t kOffBoostHint = 0xBC;
constexpr size_t kOffBoostReasonHint = 0xC0;
constexpr size_t kOffBoostExpireHint = 0xC4;
// LocalUser+0x15C = 武器攻速 degree（战斗点 0x7FFB84A62C52 `mov edi,[rdi+15Ch]`）。只读。
constexpr size_t kOffLuWeaponDegree = 0x15C;
// 引擎夹 [2,10]；-8 保证任何武器都落到 2（最快，延迟 ×0.75）。
// 合法 booster 只写 -1/-2，本值超出合法域 —— 若客户端把 nBooster_ 原样上报即为指纹。
constexpr int kBoosterValue = -8;
// tBooster_ 按游戏钟续到 now+60s，每拍重写；过期会被 CheckByTime→Reset 清掉。
constexpr int kBoosterHoldMs = 60000;
constexpr DWORD kRefreshMs = 16;
constexpr DWORD kRebindMs = 2000;
constexpr DWORD kLogChangeMs = 1000;  // 字段变化：最短间隔（防抖）
constexpr DWORD kLogIdleMs = 30000;   // 稳态心跳
constexpr DWORD kLandGraceMs = 400;
constexpr DWORD kSkipPrepareLandGraceMs = 1000;

// LocalUser(TDI:1560) 覆写 Prepare @ 0xFDE830；基类 User @ 0x1251E30
// UserLocal(TDI:1577) : LocalUser — 无再覆写；虚表槽仍在实例 klass 上。
// dump.cs Slot:32 · 哈希 e7e1a3b6…（默认参 action=6,speed=100,bool=false）· remount 2026-08-04
constexpr uint32_t kRvaUserPrepare = 0xFDE830;
constexpr uint32_t kRvaFbPrepare = 0x1251E30;
constexpr char kHashPrepareActionLayer[] =
    "e7e1a3b65a50e20dd79a2a9c50da9f84dd4b109213cd4dd9c18b6ab4156c349";
constexpr char kHashLocalUser[] =
    "d9ad004bbff1a41ca96697c8e44ed3175dae9846fb772898fd54ec65040348b";
// 声明 Prepare 的类（= LocalUser）；虚表补丁仍打在最末级 UserLocal 上
constexpr const char* kHashLocalUserDecl = kHashLocalUser;
constexpr int kPrepareVtableSlot = 32;
constexpr size_t kVirtInvokeStride = 16;
constexpr size_t kHintVtableBase = 0x138;
constexpr size_t kVtableScanLo = 0x80;
constexpr size_t kVtableScanHi = 0xC00;
constexpr DWORD kSkipPrepareInstallRetryMs = 2000;

// 字段防漂移（Il2CppDumper 哈希名 → field_get_offset；失败回退 Hint）
constexpr char kHashSecondaryStat[] =
    "b66e6c1639331514fade7a757dd74e7e70d7d903c49252b516d09778ecc46d6";
constexpr char kHashSlow[] =
    "aea6fd45d31598f1936c772d806a1bb83d166edcd16bd80747b4f092a4a0c61";
constexpr char kHashSlowExpire[] =
    "f9f6337974f65d92abf601516fa537c53456c6963d72ea782abcabd0df08949";
constexpr char kHashBoost[] =
    "edb890433d34daf1efb63896f22c31ae4a777e21a8f181c6e821f14e5e69a10";
constexpr char kHashBoostReason[] =
    "e5bcb6dd51713bba4f713b8bfcc56029dab11c1fad119b854a7d87c235bcdf9";
constexpr char kHashBoostExpire[] =
    "eaa8842a44b8a90ca5fda2ab1579ec9cfa634a817e7697ec83fada533f60979";
constexpr char kHashActionBusy[] =
    "b4e36b7c44c0cb3becd5a2a5db50ce5825d0b8c71552777fa1a82a2ca5a8c34";
constexpr char kHashActionLayerA[] =
    "af6dd6f6c5235a728464f7016187d51df2d43b906b10e7decae7e05530966ba";
constexpr char kHashActionLayerB[] =
    "e091ef23e01ee04968579fef353fadbf5a932d48c536b32caf9db717207e0d4";

using FnPrepareActionLayer = void (*)(void* self, int32_t action, int32_t speed, uint8_t flag,
                                      const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<bool> gDesired{false};
std::atomic<bool> gBoosterDesired{false};
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

DWORD WINAPI Worker(LPVOID) {
    OpenLog();
    EnsureFieldOffsets();
    Log("worker start (clearBusy | nBooster_=%d/hold=%ums 独立开关; landGrace=%ums; "
        "skipPrepLandGrace=%ums)",
        kBoosterValue, (unsigned)kBoosterHoldMs, (unsigned)kLandGraceMs,
        (unsigned)kSkipPrepareLandGraceMs);
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
        const bool cutOn = gCutLayerDesired.load(std::memory_order_acquire);
        const bool skipPrepOn = gSkipPrepareDesired.load(std::memory_order_acquire);
        const bool anyWrite = accelOn || boostOn || cutOn;
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
                // 离场前尽量把 booster 还回去；ss 已失效则只丢状态（残值最多 60s 后自然到期）。
                BoosterOff(ss);
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
            const bool needSs = boostOn;
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

            // booster 是纯数值字段，不吃落地宽限 —— 宽限是为忙锁/SkipPrepare 那类可视写入设的。
            // 开关仍开着但 ss 暂时没解析出来时保持接管：此刻 BoosterOff 会把基线丢成我们自己
            // 写的 -8，等 ss 回来再登记就成了假基线，关闭时「还」不回去。
            if (boostOn) {
                if (ss) ApplyBooster(ss);
            } else {
                BoosterOff(ss);
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
                                    ((cutOn ? 1 : 0) << 6);
                const bool changed = holdSig != prevHoldSig;
                const bool due =
                    !lastLog || (changed && now - lastLog >= kLogChangeMs) ||
                    (!changed && now - lastLog >= kLogIdleMs);
                if (due) {
                    lastLog = now;
                    prevHoldSig = holdSig;
                    Log("hold grace=%d suppress=%d lu=%p skipArmed=%d accel=%d boost=%d cut=%d",
                        inGrace ? 1 : 0, fireHeld ? 1 : 0, lu, skipArmed ? 1 : 0,
                        accelOn ? 1 : 0, boostOn ? 1 : 0, cutOn ? 1 : 0);
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

            if (accelOn || boostOn || cutOn || skipArmed) {
                const int busy = ReadI32(lu, gOffActionBusy);
                const int slow = ss ? ReadI32(ss, gOffSlow) : 0;
                const int tSlow = ss ? ReadI32(ss, gOffSlowExpire) : 0;
                const int boost = ss ? ReadI32(ss, gOffBoost) : 0;
                const int tBoost = ss ? ReadI32(ss, gOffBoostExpire) : 0;
                const int wpn = ReadI32(lu, kOffLuWeaponDegree);
                const int deg = (std::min)(10, (std::max)(2, wpn + boost));
                // hits/pass 不进签名：否则出刀期仍会每秒刷；事件路径另有 Log。
                const int onSig =
                    busy + 3 * slow + 5 * tSlow + 7 * boost + 11 * tBoost + 13 * wpn + 17 * deg +
                    19 * cutN + (gBoostHeld ? 23 : 0) + (accelOn ? 29 : 0) + (boostOn ? 31 : 0) +
                    ((cutOn && !skipPrepOn) ? 37 : 0) + (skipArmed ? 41 : 0) +
                    (gBoostRebaseCnt * 43);
                const bool changed = onSig != prevOnSig;
                const bool due =
                    !lastLog || (changed && now - lastLog >= kLogChangeMs) ||
                    (!changed && now - lastLog >= kLogIdleMs);
                if (due) {
                    lastLog = now;
                    prevOnSig = onSig;
                    Log("on lu=%p busy=%d ss=%p slow=%d tSlow=%d boost=%d tBoost=%d held=%d "
                        "wpn=%d deg=%d x%.2f rebase=%u accel=%d bst=%d cut=%d cutN=%d "
                        "skipArmed=%d skipHits=%u pass=%u",
                        lu, busy, ss, slow, tSlow, boost, tBoost, gBoostHeld ? 1 : 0, wpn, deg,
                        (deg + 10) / 16.0, gBoostRebaseCnt, accelOn ? 1 : 0, boostOn ? 1 : 0,
                        (cutOn && !skipPrepOn) ? 1 : 0, cutN, skipArmed ? 1 : 0,
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

}  // namespace attack_accel
}  // namespace features
}  // namespace x
