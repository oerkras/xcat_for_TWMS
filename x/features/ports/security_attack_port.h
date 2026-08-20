#pragma once
// security_attack_port — Classic TWMS SecurityClient 攻包/技能计数窗（只读观测）。
// 读 m_mAttackPacketCnt / m_mAttackSkillCnt / detectTime。
// 不写字典、不 Hook GA .text。BIN：bin/XCat_data/logs/sec_attack.log

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
    int peakKey = 0;            // 两表单键最大值（当前快照）
    int pktPeak = 0;            // 攻包表单键最高
    int skillPeak = 0;          // 技能表单键最高
    int pktPeakId = 0;          // 攻包峰值键（opcode，ushort）
    int skillPeakId = 0;        // 技能峰值键（skillId）
    int windowPeak = 0;         // 本窗/本局见过的单键最高（原生 60s 清窗或软重连落地会重置）
    int windowPktSum = 0;
    int windowSkillSum = 0;
};

void Init();
void Shutdown();
bool EnsureBound();
bool Ready();

// 只读快照并打 tag=SecAttack 日志。
bool ProbeWindow(WindowSnapshot* out = nullptr);
// 静默快照（面板 / PayloadStatus）；不写 x.jsonl、不写 BIN。
bool PeekWindow(WindowSnapshot* out);

// 当前窗内见过的最高值（原生 60s 清窗或软重连落地会重置观测高水位）。
int WindowHighPeak();
int WindowHighPktSum();
int WindowHighSkillSum();

// 新一轮挂机会话：只重置观测高水位，不写游戏字典。
void NoteHangupSession(const char* why);

void StartWorker();
void StopWorker();

}  // namespace x::features::ports::security_attack
