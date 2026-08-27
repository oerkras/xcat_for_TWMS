#pragma once

#include <cstdint>

namespace xcat {

// 经典版自动加点：user.ini [auto_stat]。独立 section，不进 [core] / PayloadControl。
// 策略对照 Maplecat UseAP（四维权重总和=5，每次 +1）；发包走官方 UIStat.ed6479da。
constexpr uint32_t kAutoStatMagic = 0x58434153u;  // 'XCAS'
constexpr uint32_t kAutoStatVersion = 1u;
// 四维配比权重合计必须等于此值（官方每次升级给 5 AP）。
// 这是「从开启起每 5 点怎么分」，不追已有四维；身上剩多少 AP 加多少。
constexpr uint32_t kAutoStatRatioSum = 5u;
constexpr uint32_t kAutoStatRatioMax = 99u;
// 缺 section / 新用户必须关。禁止改成 1（挂机卡其它项有默认开的先例）。
constexpr uint32_t kAutoStatDefaultEnabled = 0;

struct AutoStatConfig {
    uint32_t magic = kAutoStatMagic;
    uint32_t version = kAutoStatVersion;
    uint32_t enabled = kAutoStatDefaultEnabled;
    uint32_t str = 0;
    uint32_t dex = 0;
    uint32_t intel = 0;  // ini 键名 int
    uint32_t luk = 0;
    uint64_t writeTickMs = 0;
};

void AutoStatSetDefaults(AutoStatConfig& out);
void AutoStatNormalize(AutoStatConfig& cfg);
uint32_t AutoStatRatioSumOf(const AutoStatConfig& cfg);
bool AutoStatRatioOk(const AutoStatConfig& cfg);

bool ReadAutoStat(const char* binDir, AutoStatConfig& out);
bool WriteAutoStat(const char* binDir, const AutoStatConfig& cfg);

// 停机写盘：persistTick==0 表示刚决定停机、还没写成盘，盘上旧 enabled=1 的 tick
// 永远 >0，不能当成「用户又开了」。仅当已经写过一次停机 tick 之后，
// 盘上出现更新的 writeTick 才视为用户重开（对齐全 auto_skill）。
constexpr bool AutoStatPersistDiskIsUserReopen(uint64_t diskWriteTickMs, uint64_t persistTickMs) {
    return persistTickMs != 0 && diskWriteTickMs > persistTickMs;
}
static_assert(!AutoStatPersistDiskIsUserReopen(123, 0), "persistTick=0 must overwrite old enabled=1");
static_assert(AutoStatPersistDiskIsUserReopen(200, 100), "newer disk tick after persist is user reopen");
static_assert(!AutoStatPersistDiskIsUserReopen(50, 100), "older disk tick is not user reopen");

// 初心者 job<100（含 0）不加。1 转起（100/200/300/400/500 及后续 2/3/4 转、双刀 430–434）才加。
inline bool AutoStatJobReady(int job) {
    if (job < 100) return false;
    if (job >= 430 && job <= 434) return true;
    return job <= 522;
}

// 职业主属性与配比是否对得上。对不上必须暂停加点（防呆）。
// 0 转 / 未知职业 / 配比未满 5：不在这里拦，返回 true。
// 规则：主属性权重要 >0；明显错维必须为 0。
//   战士 1xx：必须有力量，禁止智力（允许敏捷 4:1）
//   法师 2xx：必须有智力，禁止力量和敏捷
//   弓手 3xx：必须有敏捷，禁止智力
//   飞侠 4xx / 双刀 430–434：必须有幸运，禁止智力
//   海盗 500：力量或敏捷至少一项，禁止智力
//   拳手 51x：必须有力量，禁止智力
//   枪手 52x：必须有敏捷，禁止智力
inline bool AutoStatJobRatioOk(int job, const AutoStatConfig& cfg) {
    if (!AutoStatJobReady(job)) return true;
    if (!AutoStatRatioOk(cfg)) return true;
    const uint32_t str = cfg.str;
    const uint32_t dex = cfg.dex;
    const uint32_t intel = cfg.intel;
    const uint32_t luk = cfg.luk;
    if (job >= 100 && job <= 132) return str > 0 && intel == 0;
    if (job >= 200 && job <= 232) return intel > 0 && str == 0 && dex == 0;
    if (job >= 300 && job <= 322) return dex > 0 && intel == 0;
    if ((job >= 400 && job <= 422) || (job >= 430 && job <= 434)) return luk > 0 && intel == 0;
    if (job == 500) return (str > 0 || dex > 0) && intel == 0;
    if (job >= 510 && job <= 512) return str > 0 && intel == 0;
    if (job >= 520 && job <= 522) return dex > 0 && intel == 0;
    return true;
}

}  // namespace xcat
