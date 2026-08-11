#pragma once

#include <cstdint>
#include <string>

namespace xcat {

// sellbag：经典版一键卖装备栏/卖其他栏 + 保留白名单（对照枫星 sellbag；产品=TWMS）。
// 白名单与手动命令：state/user.ini [sellbag]；面板写 manualSeq/manualMask，payload 热读。

constexpr uint32_t kSellbagMagic   = 0x47414253u;  // 'SBAG'
constexpr uint32_t kSellbagVersion = 2u;

constexpr int kSellbagMaxKeepRules = 32;
constexpr int kSellbagNameKeyLen   = 48;

// 全新配置默认保留关键词（物品名包含匹配；繁中「礦」覆盖各类矿石/礦石名）。
constexpr const char* kSellbagDefaultKeepNameKey = "礦";

// 卖出目标栏位掩码。
constexpr uint32_t kSellbagBagEquip = 1u << 0;  // 装备栏
constexpr uint32_t kSellbagBagEtc   = 1u << 1;  // 其他/ETC 栏
constexpr uint32_t kSellbagBagAll   = kSellbagBagEquip | kSellbagBagEtc;

#pragma pack(push, 1)
// 保留白名单规则：物品名（繁中）关键词包含匹配，命中即跳过不卖。
// targetMask 字段保留布局兼容；读写强制 all（装备+其他共用，无分栏 UI）。
struct SellbagKeepRule {
    uint32_t enabled    = 0;                       // 0/1
    uint32_t targetMask = kSellbagBagAll;          // 恒为 all；勿按栏拆分
    char     nameKey[kSellbagNameKeyLen] = {};     // 名称关键词（UTF-8）
};

struct SellbagConfig {
    uint32_t magic   = kSellbagMagic;
    uint32_t version = kSellbagVersion;
    uint32_t keepRuleCount = 0;                    // [0, kSellbagMaxKeepRules]
    SellbagKeepRule keepRules[kSellbagMaxKeepRules]{};
    uint64_t writeTickMs = 0;
    uint32_t manualSeq = 0;                         // launcher 手动命令递增序号
    uint32_t manualMask = 0;                        // kSellbagBagEquip / kSellbagBagEtc / kSellbagBagAll
    uint32_t abortSeq = 0;                          // launcher 中止卖出递增序号
};
#pragma pack(pop)

void SellbagSetDefaults(SellbagConfig& out);

std::string SellbagRelPath();
std::string SellbagPath(const char* binDir);

bool ReadSellbag(const char* binDir, SellbagConfig& out);
bool WriteSellbag(const char* binDir, const SellbagConfig& cfg);

bool ReadSellbagKeepRulesIni(const char* binDir, SellbagConfig& out, uint64_t* outWriteTick);
bool WriteSellbagKeepRulesIni(const char* binDir, const SellbagConfig& cfg, uint64_t writeTickMs);

}  // namespace xcat
