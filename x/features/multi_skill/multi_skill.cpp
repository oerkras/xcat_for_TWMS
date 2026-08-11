// Classic TWMS multi_skill worker — refresh learned list + cast-request + Tick queue.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "multi_skill.h"

#include "../ports/multi_skill_port.h"
#include "../ports/skill_port.h"
#include "../ports/attack_input_port.h"
#include "../ports/security_attack_port.h"
#include "../../ipc/payload_control.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"

#include "xcat_multiskill_select.h"

#include <Windows.h>
#include <timeapi.h>

#include <atomic>
#include <cstring>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace x::features::multi_skill {
namespace {

std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
DWORD gLastSkillRefreshMs = 0;
DWORD gLastSecAttackProbeMs = 0;
constexpr DWORD kSkillRefreshMs = 3000;
constexpr DWORD kSecAttackProbeMs = 15000;
constexpr DWORD kTickMs = 30;
constexpr DWORD kIdleTickMs = 200;  // 多发关闭：降唤醒频率（仍要 Poll IPC / 学技刷新）

void RefreshLearnedSkills() {
    if (!ports::skill::EnsureBound()) return;
    ports::skill::SkillInfoLite buf[ports::skill::kMaxLearnedSkills]{};
    const int n = ports::skill::ListLearnedSkills(buf, ports::skill::kMaxLearnedSkills);
    std::vector<xcat::LearnedSkillRow> rows;
    rows.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        xcat::LearnedSkillRow r{};
        strncpy_s(r.code, buf[i].code, _TRUNCATE);
        // name 已由 ResolveSkillName（offline-first）填好。
        strncpy_s(r.name, buf[i].name[0] ? buf[i].name : buf[i].code, _TRUNCATE);
        r.level = buf[i].level;
        rows.push_back(r);
    }
    if (xcat::WriteLearnedSkillsTsv(runtime::GetBinDir(), rows)) {
        runtime::LogI("MultiSkill", "learned_skills refreshed n=%d", n);
    }
}

DWORD WINAPI WorkerMain(LPVOID) {
    runtime::LogI("MultiSkill", "worker start");
    ports::security_attack::Init();
    ports::attack::Init();
    timeBeginPeriod(1);
    while (!gWorkerStop.load()) {
        x::ipc::PayloadControl_Poll();

        const bool enabled = ports::multi_skill::IsEnabled();

        if (xcat::ConsumeMultiSkillCastRequest(runtime::GetBinDir())) {
            // 出刀前采一次 SecurityClient 计数窗（type20）；LiveValue 430/557/558 已挖空，不挂热路径。
            ports::security_attack::ProbeWindow();
            char reason[64]{};
            if (!enabled) {
                runtime::LogW("MultiSkill", "cast-request ignored (disabled)");
            } else {
                const bool ok = ports::multi_skill::TryCast(reason, sizeof(reason));
                runtime::LogI("MultiSkill", "cast-request TryCast ok=%d reason=%s", ok ? 1 : 0,
                              reason);
            }
        }

        const DWORD now = GetTickCount();
        // 已学技能表给面板用：关多发也要刷；SecAttack 周期探针仅开启时做。
        if (!gLastSkillRefreshMs ||
            static_cast<DWORD>(now - gLastSkillRefreshMs) >= kSkillRefreshMs) {
            gLastSkillRefreshMs = now;
            RefreshLearnedSkills();
        }
        if (enabled &&
            (!gLastSecAttackProbeMs ||
             static_cast<DWORD>(now - gLastSecAttackProbeMs) >= kSecAttackProbeMs)) {
            gLastSecAttackProbeMs = now;
            ports::security_attack::ProbeWindow();
        }

        // Tick：关闭时内部早退（只泵 TickReleases）；开启才跑队列/忙锁。
        ports::multi_skill::Tick();
        Sleep(enabled ? kTickMs : kIdleTickMs);
    }
    ports::attack::ForceRelease();
    ports::security_attack::Shutdown();
    timeEndPeriod(1);
    runtime::LogI("MultiSkill", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    // skill_port 由 buffs 或本模块共用；重复 Init 安全。
    ports::skill::Init();
    ports::multi_skill::Init();
    runtime::LogI("MultiSkill", "feature init");
}

void Shutdown() {
    StopWorker();
    ports::multi_skill::Shutdown();
    // 不 Shutdown skill_port：buffs 仍可能占用。
}

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop = false;
    HANDLE th = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
    gWorkerThread.store(th);
}

void StopWorker() {
    gWorkerStop = true;
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) {
        // DllMain DETACH: never join under loader lock — just signal.
        CloseHandle(th);
    }
}

void SetConfig(bool enabled, uint32_t gapMs, bool safeStagger) {
    ports::multi_skill::SetConfig(enabled, gapMs, safeStagger);
}

void SetSendUseRequest(bool on) { ports::multi_skill::SetSendUseRequest(on); }

bool IsEnabled() { return ports::multi_skill::IsEnabled(); }

bool TryCast(char* out, int outSz) { return ports::multi_skill::TryCast(out, outSz); }

bool IsBurstBusy() { return ports::multi_skill::IsBurstBusy(); }

bool CancelPendingBurstForRetarget() {
    return ports::multi_skill::CancelPendingBurstForRetarget();
}

}  // namespace x::features::multi_skill
