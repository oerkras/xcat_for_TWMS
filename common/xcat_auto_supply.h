#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace xcat {

// Classic TWMS 自动补给：PC GUI 写 user.ini [auto_supply]，payload 热读；运行态写 [auto_supply_status]。
// 对照枫星 auto_supply 配置契约；实现为 Il2Cpp 开店/买卖（非 Lua）。
// 触发：装备栏达阈值 → 就近可卖店开店 → 先卖装/其他 → 可选补货（店内有则买、无则跳过）→ 回挂机图。
// 寻店不关心货架内容；补给品类由用户自选（通常选挂机图附近店有的）。
// 去店：优先回家卷軸 2030000（同名备用 2030059），无卷则 travel；shopMapName 非空则覆盖自动寻店。
// 飞镖 Charge 字段保留兼容；经典版执行侧暂 stub（UIShop Charge CF 平坦化未钉入口）。

constexpr uint32_t kAutoSupplyMagic   = 0x50555341u;  // 'ASUP'
constexpr uint32_t kAutoSupplyVersion = 6u;

// 回家卷軸（智能回最近主城）。离线 catalog：2030000 / 2030059 同名「回家卷軸」。
// 用卷时主码失败会再试备用码；补买仍买主码（杂货店货架常见 2030000）。
constexpr const char* kAutoSupplyDefaultReturnScrollCode = "2030000";
constexpr const char* kAutoSupplyAltReturnScrollCode     = "2030059";
constexpr const char* kAutoSupplyDefaultPotionCode       = "2000000";

// 可选补红/补蓝 UI 默认名（精确中文）与经典 CODE；红/蓝默认不勾选。
// 补蓝默认「藍色藥水」(2000003)，补到 300、默认关（维港杂货常见；非神社黑輪）。
// 自定义默认「回家卷軸」(2030000)，补到 100、默认关；简体「回家卷轴」同 CODE。
// 饲料默认勾选、补到 100；执行优先美味飼料→寵物食品（店内无则跳过）。
// 寻店不按这些 CODE 选型；用户应勾选挂机附近店有卖的品类。
// 离线 catalog 名优先来自 LOCAL *StringDataBaked；内置仅兜底默认项
//（避免「紅色藥水」与宠物卷轴等同名时选错 CODE）。
constexpr const char* kAutoSupplyDefaultRefillHpName      = "紅色藥水";
constexpr const char* kAutoSupplyDefaultRefillMpName      = "藍色藥水";
constexpr const char* kAutoSupplyDefaultRefillCustomName  = "回家卷軸";
constexpr const char* kAutoSupplyDefaultRefillFeedName    = "美味飼料";
constexpr const char* kAutoSupplyDefaultRefillFeedAltName = "寵物食品";
constexpr const char* kAutoSupplyDefaultRefillHpCode      = "2000000";
constexpr const char* kAutoSupplyDefaultRefillMpCode      = "2000003";
constexpr const char* kAutoSupplyDefaultRefillCustomCode  = kAutoSupplyDefaultReturnScrollCode;
constexpr const char* kAutoSupplyDefaultRefillFeedCode    = "2120008";
constexpr const char* kAutoSupplyDefaultRefillFeedAltCode = "2120000";
constexpr int32_t     kAutoSupplyDefaultRefillMpBuyTo       = 300;
constexpr int32_t     kAutoSupplyDefaultRefillCustomBuyTo   = 100;

constexpr int kAutoSupplyMaxBuyRules  = 8;
constexpr int kAutoSupplyMaxSellRules = 16;
constexpr int kAutoSupplyCodeLen      = 24;
constexpr int kAutoSupplyNameLen      = 64;
constexpr int kAutoSupplyNameKeyLen   = 40;

constexpr uint32_t kAutoSupplyManualNone       = 0u;
constexpr uint32_t kAutoSupplyManualProbe      = 1u;  // 只探测当前商店 UI，不执行买卖
constexpr uint32_t kAutoSupplyManualRun        = 2u;  // 执行一次 ShopUiOnly 买入最小闭环
constexpr uint32_t kAutoSupplyManualSell       = 3u;  // 执行一次 ShopUiOnly 卖出最小闭环
constexpr uint32_t kAutoSupplyManualTrip       = 4u;  // 完整一趟：回城→开店→先卖后买
constexpr uint32_t kAutoSupplyManualReturnFarm = 5u;  // 只回挂机图（不卖不买）
constexpr uint32_t kAutoSupplyManualStop       = 6u;  // 停止动作：关开关+中止行程/超级赶路/开店

constexpr uint32_t kAutoSupplyOpenRequireAlreadyOpen = 0u;
constexpr uint32_t kAutoSupplyOpenInteractNpc        = 1u;  // 预留：寻路到 NPC 后交互打开
constexpr uint32_t kAutoSupplyOpenScriptedProbe      = 2u;  // 预留：运行时验证后再开放

constexpr uint32_t kAutoSupplyBagEquip       = 1u << 0;
constexpr uint32_t kAutoSupplyBagConsumption = 1u << 1;
constexpr uint32_t kAutoSupplyBagInstall     = 1u << 2;
constexpr uint32_t kAutoSupplyBagEtc         = 1u << 3;
constexpr uint32_t kAutoSupplyBagCash        = 1u << 4;

constexpr uint32_t kAutoSupplySellDenylist   = 0u;  // 默认保守：只卖不在保留规则内的目标栏位
constexpr uint32_t kAutoSupplySellAllowlist  = 1u;  // 只卖明确命中的物品

constexpr uint32_t kAutoSupplyStateIdle          = 0u;
constexpr uint32_t kAutoSupplyStateDisabled      = 1u;
constexpr uint32_t kAutoSupplyStateProbeShopUi   = 2u;
constexpr uint32_t kAutoSupplyStateShopUiReady   = 3u;
constexpr uint32_t kAutoSupplyStateShopUiMissing = 4u;
constexpr uint32_t kAutoSupplyStateBuying        = 5u;
constexpr uint32_t kAutoSupplyStateBuyDone       = 6u;
constexpr uint32_t kAutoSupplyStateBuySkipped    = 7u;
constexpr uint32_t kAutoSupplyStateSelling       = 8u;
constexpr uint32_t kAutoSupplyStateSellDone      = 9u;
constexpr uint32_t kAutoSupplyStateSellSkipped   = 10u;
constexpr uint32_t kAutoSupplyStateGoingTown     = 20u;  // 回城卷 / 多跳赶路中
constexpr uint32_t kAutoSupplyStateOpeningShop   = 21u;
constexpr uint32_t kAutoSupplyStateTripTrading   = 22u;  // 本趟先卖后买
constexpr uint32_t kAutoSupplyStateReturning     = 23u;
constexpr uint32_t kAutoSupplyStateTripDone      = 24u;
constexpr uint32_t kAutoSupplyStateError         = 100u;

#pragma pack(push, 1)
struct AutoSupplyBuyRule {
    uint32_t enabled = 0;
    char     itemCode[kAutoSupplyCodeLen] = {};  // 执行主键
    int32_t  triggerBelow = 0;
    int32_t  buyTo = 0;
};

struct AutoSupplySellRule {
    uint32_t enabled = 0;
    uint32_t bagMask = kAutoSupplyBagEtc;
    uint32_t mode = kAutoSupplySellAllowlist;
    char     itemCode[kAutoSupplyCodeLen] = {};
    char     nameKey[kAutoSupplyNameKeyLen] = {};  // UI 辅助；执行优先 itemCode
};

struct AutoSupplyConfig {
    uint32_t magic = kAutoSupplyMagic;
    uint32_t version = kAutoSupplyVersion;
    uint32_t enabled = 0;
    uint32_t manualSeq = 0;
    uint32_t manualKind = kAutoSupplyManualNone;

    char shopNpcCode[kAutoSupplyCodeLen] = {};
    char shopMapName[kAutoSupplyNameLen] = {};
    char returnMapName[kAutoSupplyNameLen] = {};

    // 运行时强制：补给必暂停战斗、必回挂机图；字段保留兼容旧 ini，GUI 不再暴露。
    uint32_t pauseCombat = 1;
    uint32_t returnAfterSupply = 1;
    uint32_t openShopMode = kAutoSupplyOpenInteractNpc;

    // autoBuyEnabled / buyRules：已废弃，读写兼容旧 ini，Normalize 强制为 0。
    uint32_t autoBuyEnabled = 0;
    uint32_t autoSellOnBagFullEnabled = 0;   // 装备栏达阈值自动卖（进城后顺便卖其他）
    // 仅装备栏已用件数：>= X 触发；0 = 满装才卖。其他栏不参与触发计数。
    // 字段名历史兼容（曾表示 ETC 空位）。
    int32_t  sellFreeSlotsAtOrBelow = 0;

    uint32_t buyRuleCount = 0;  // 废弃
    AutoSupplyBuyRule buyRules[kAutoSupplyMaxBuyRules]{};

    uint32_t sellRuleCount = 0;
    AutoSupplySellRule sellRules[kAutoSupplyMaxSellRules]{};

    // 可选补红/补蓝/补自定义：用户填中文名（精确全匹配），GUI 失焦后离线表反查 CODE 缓存。
    // 饲料：执行固定美味飼料→寵物食品；refillFeedName/Code 仅作主选缓存，Normalize 强制收敛。
    // 执行仍走 CODE 买店；识别/配置侧禁止用 CODE 当用户输入（红/蓝/自定义）。
    uint32_t refillHpEnabled = 0;
    uint32_t refillMpEnabled = 0;
    uint32_t refillCustomEnabled = 0;  // 默认关：自定义消耗品（默认名回家卷軸）
    uint32_t refillFeedEnabled = 1;  // 默认开：补美味飼料
    char     refillHpName[kAutoSupplyNameLen] = {};
    char     refillMpName[kAutoSupplyNameLen] = {};
    char     refillCustomName[kAutoSupplyNameLen] = {};
    char     refillFeedName[kAutoSupplyNameLen] = {};
    char     refillHpCode[kAutoSupplyCodeLen] = {};
    char     refillMpCode[kAutoSupplyCodeLen] = {};
    char     refillCustomCode[kAutoSupplyCodeLen] = {};
    char     refillFeedCode[kAutoSupplyCodeLen] = {};
    int32_t  refillHpBuyTo = 0;
    int32_t  refillMpBuyTo = 0;
    int32_t  refillCustomBuyTo = kAutoSupplyDefaultRefillCustomBuyTo;
    int32_t  refillFeedBuyTo = 100;

    // 卖装行程开店后：消耗栏可充值手里剑自动 Charge；钱不够跳过该件。默认关。
    uint32_t rechargeStarsEnabled = 0;

    uint64_t writeTickMs = 0;
};

struct AutoSupplyStatus {
    uint32_t magic = kAutoSupplyMagic;
    uint32_t version = kAutoSupplyVersion;
    uint32_t state = kAutoSupplyStateIdle;
    uint32_t lastManualSeq = 0;
    uint32_t lastResult = 0;
    char     shopNpcCode[kAutoSupplyCodeLen] = {};
    char     message[96] = {};
    // 最近一次从野外触发补给时的挂机图；崩溃/城里重试时用于回图打怪
    char     lastFarmMapName[kAutoSupplyNameLen] = {};
    // 1=卖完后回挂机图未完成（含回图途中崩溃）；重拉后优先续跑回图，不依赖满包
    uint32_t pendingReturnFarm = 0;
    uint64_t writeTickMs = 0;
};
#pragma pack(pop)

void AutoSupplySetDefaults(AutoSupplyConfig& out);
void AutoSupplyStatusSetDefaults(AutoSupplyStatus& out);

// 内置红/蓝/饲料名→CODE；非内置名返回 nullptr。
const char* AutoSupplyBuiltinRefillCodeForName(const char* nameZh);

std::string AutoSupplyRelPath();
std::string AutoSupplyStatusRelPath();
std::string AutoSupplyPath(const char* binDir);
std::string AutoSupplyStatusPath(const char* binDir);

bool ReadAutoSupply(const char* binDir, AutoSupplyConfig& out);
bool WriteAutoSupply(const char* binDir, const AutoSupplyConfig& cfg);
bool ReadAutoSupplyStatus(const char* binDir, AutoSupplyStatus& out);
bool WriteAutoSupplyStatus(const char* binDir, const AutoSupplyStatus& status);

}  // namespace xcat