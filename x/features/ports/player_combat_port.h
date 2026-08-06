#pragma once
// player_combat_port — Classic TWMS LocalUser 战斗上下文（坐标读 + 主线程视觉收态）
//
// 解析纪律对齐 skill_port / titlebar：WM.MyUser 优先，FindAll + GO 名 MyUser。
// 禁止 INLINE HOOK。HealVisualToAp 仅 Unity pump 线程可写。

#include <cstdint>

namespace x::features::ports::player_combat {

struct CombatCtx {
    bool ok = false;
    void* localUser = nullptr;
    float x = 0.f;
    float y = 0.f;
};

// 视觉层：Ap（魂）/ Apl（皮插值）/ Pos@0x64 / CurPos@0x240（镜头）/ CurFh。
// HealVisualToAp：收 Apl/Pos；若 |Ap−CurPos| 僵死再按 restCam 收 CurPos/PrevPos（BIN：冻镜头）。
struct VisualSnap {
    bool ok = false;
    float apX = 0.f, apY = 0.f;
    float aplX = 0.f, aplY = 0.f;
    float posX = 0.f, posY = 0.f;
    float curPosX = 0.f, curPosY = 0.f;
    uint32_t curFh = 0;
    double rpPos = 0.0;  // RelPos.Pos（有台弧长）
    double rpV = 0.0;    // RelPos.V
    float dApApl = 0.f;   // |Ap−Apl|
    float dApPos = 0.f;   // |Ap−Pos@0x64|
    float dApCur = 0.f;   // |Ap−(CurPos−camOff 近似)| 用 |Ap−CurPos| 粗看
    // UserLocal+0x4B4：e2a28(Key) 键位移用的 int 偏移对（IDA · OnKey 链）
    int32_t keyDeltaX = 0;
    int32_t keyDeltaY = 0;
    bool keyDeltaOk = false;
};

bool EnsureBound();
// 完整上下文：要有效坐标（打怪选怪/贴怪）。
bool QueryCombatCtx(CombatCtx& out);
// 只要 LocalUser 指针（点飞绝对落点不依赖旧 Ap；换图落地 Ap 暂坏时仍可 Doing）。
bool QueryLocalUser(void** outLu);

// 诊断读坐标：即便 PosSane 失败也尽量填 raw x/y 与 why（供 bad_player_pos 日志）。
// 不改变 QueryCombatCtx 的严格语义。
struct PosDiag {
    bool hasLocalUser = false;
    bool luAlive = false;
    bool sane = false;
    float x = 0.f;
    float y = 0.f;
    const char* why = "unknown";
};
void PeekLocalPosDiag(PosDiag& out);

// 采一份视觉快照；失败 out.ok=false。
bool QueryVisualSnap(VisualSnap& out);
// 打日志。comp=Buffs/TimedKeys；phase=pre|post|t+120；note 如 skill=1002 / key='='。
// baseline 非空时附带相对基线的 dAp/dApl/dPos/dAA（ASCII，避免日志编码花字）。
void LogVisualLayer(const char* comp, const char* phase, const char* note,
                    const VisualSnap* baseline = nullptr);

// OnKey→e2a28 探针：phase=pre|post；note 如 key=14；baseline=pre 时 post 仅漂移才打。
// 环境变量 XCAT_PROBE_ONKEY=1 时 pre 也打（含 +0x4B4）；未开则只在 post 漂移时打。
void LogE2a28KeyProbe(const char* phase, const char* note, const VisualSnap* baseline = nullptr);

// DoActiveSkill 只读探针（挂在 skill_port，不进 buffs）：phase=pre|post。
// XCAT_PROBE_DOACTIVE=1 → pre 也打；默认仅 post 漂移（dPos/dAp/dApl/dCur）才打。
void LogDoActiveVisProbe(const char* phase, const char* note, const VisualSnap* baseline = nullptr);

// fill+Doing 收态后只读探针（无写）：抓 CurPos 相对 Ap 是否僵死。
// 默认：dApCur 刚越阈（边沿）或每 2s 卡死续报；XCAT_PROBE_FILLCAM=1 → 每跳都打（会很吵）。
void LogFillDoingCamProbe(const char* note);

// BUFF 施法后只读探针（无写；调用方常在 worker）：不论 dApCur 都采样（≥1s 节流）。
void LogBuffCamProbe(const char* note);

// 收 Apl / Pos@0x64 到 Ap。镜头（CurPos/PrevPos）归引擎，本模块只读不写
//（2026-08-04 撤销镜头自愈，理由见 .cpp 常量区）。
// force=true：无条件写 Apl/Pos（fill+Doing 后）。
// 须在主线程调用（fill+Doing job 或 pump FrameTick）。BUFF/定时键不要调——探针只读。
bool HealVisualToAp(const char* why, bool force = false);

}  // namespace x::features::ports::player_combat
