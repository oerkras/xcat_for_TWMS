#pragma once
// security_attack_port — Classic TWMS SecurityClient 攻包/技能计数窗只读探针。
// 读 m_mAttackPacketCnt / m_mAttackSkillCnt / detectTime；不 Hook、不调 SendAttackPacketCountCheck。

#include <cstdint>

namespace x::features::ports::security_attack {

// 与 dump 元数据 / 函数体解码一致（见 Dumps/runtime/_dig_securityclient_attack_window.md）
constexpr int kTermMs = 60000;
constexpr int kCheckCount = 2000;
constexpr int kClientHacksAttackPacketCountCheck = 20;

struct WindowSnapshot {
    bool ok = false;
    bool staticReady = false;   // klass + static_fields 解析成功
    bool pktDictOk = false;
    bool skillDictOk = false;
    int pktSum = 0;             // AttackPacket 计数字典 value 合计
    int pktKeys = 0;            // 存活 entry 数
    int skillSum = 0;
    int skillKeys = 0;
    int detectTime = 0;         // m_tAttackPacketCntDetectTime（游戏 tCur，非 GetTickCount）
    int windowAgeMs = -1;       // 保留字段；时钟不同源，探针固定 -1
    int pctOfCheck = 0;         // peakKey * 100 / CHECK_COUNT（type20 按单键）
};

void Init();
void Shutdown();
bool EnsureBound();
bool Ready();

// 只读快照并打 tag=SecAttack 日志。
bool ProbeWindow(WindowSnapshot* out = nullptr);

}  // namespace x::features::ports::security_attack
