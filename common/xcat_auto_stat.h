#pragma once

#include <cstdint>

namespace xcat {

// 经典版自动加点：user.ini [auto_stat]。独立 section，不进 [core] / PayloadControl。
// 策略对照 Maplecat UseAP（四维权重总和=5，每次 +1）；发包走官方 UIStat.ed6479da。
constexpr uint32_t kAutoStatMagic = 0x58434153u;  // 'XCAS'
constexpr uint32_t kAutoStatVersion = 1u;
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

}  // namespace xcat
