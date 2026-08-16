#pragma once

#include <cstdint>

namespace xcat {

// 经典版一键起号：user.ini [char_boot] / [char_boot_status]。独立 section，不进 [core]。
// 无 enabled 键；触发只走 manualSeq + manualKind。v1 只做法师 1 转。
constexpr uint32_t kCharBootMagic = 0x58434254u;  // 'XCBT'
constexpr uint32_t kCharBootVersion = 1u;

constexpr uint32_t kCharBootDefaultFarmMap = 40000u;
constexpr uint32_t kCharBootDefaultHangupMap = 101010000u;
constexpr uint32_t kCharBootDefaultLevelMin = 8u;
constexpr uint32_t kCharBootDefaultMesoMin = 1000u;
constexpr uint32_t kCharBootLevelMinLo = 8u;
constexpr uint32_t kCharBootLevelMinHi = 200u;
constexpr uint32_t kCharBootMesoMinLo = 1u;
constexpr uint32_t kCharBootMesoMinHi = 2000000000u;

constexpr uint32_t kCharBootDepartLevel = 0u;
constexpr uint32_t kCharBootDepartMeso = 1u;

constexpr uint32_t kCharBootManualNone = 0u;
constexpr uint32_t kCharBootManualStart = 1u;
constexpr uint32_t kCharBootManualStop = 2u;

// 桑克斯船费；不是配置。
constexpr int64_t kCharBootShipFareMeso = 150;

struct CharBootConfig {
    uint32_t magic = kCharBootMagic;
    uint32_t version = kCharBootVersion;
    uint32_t farmMap = kCharBootDefaultFarmMap;
    uint32_t hangupMap = kCharBootDefaultHangupMap;
    uint32_t departKind = kCharBootDepartLevel;
    uint32_t levelMin = kCharBootDefaultLevelMin;
    uint32_t mesoMin = kCharBootDefaultMesoMin;
    uint32_t requireInt20 = 0;  // leftover ini; always 0. Hans does not check INT.
    uint32_t farmTimeoutMin = 0;
    uint32_t autoCreateChar = 0;
    uint32_t manualSeq = 0;
    uint32_t manualKind = kCharBootManualNone;
    uint64_t writeTickMs = 0;
};

struct CharBootStatus {
    char state[32]{};
    char message[160]{};
    char lastWhy[96]{};
    uint32_t mapId = 0;
    int32_t level = 0;
    int32_t job = 0;
    int32_t intel = 0;
    int64_t meso = -1;
    uint32_t ready = 0;
    uint32_t hangupMap = 0;
    uint64_t writeTickMs = 0;
};

void CharBootSetDefaults(CharBootConfig& out);
void CharBootNormalize(CharBootConfig& cfg);
void CharBootStatusSetDefaults(CharBootStatus& out);

bool CharBootStateIsBusy(const char* state);

bool ReadCharBoot(const char* binDir, CharBootConfig& out);
bool WriteCharBoot(const char* binDir, const CharBootConfig& cfg);

bool ReadCharBootStatus(const char* binDir, CharBootStatus& out);
bool WriteCharBootStatus(const char* binDir, const CharBootStatus& status);

}  // namespace xcat
