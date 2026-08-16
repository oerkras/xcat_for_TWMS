// mob_gather feature — oneshot seq + 勾选后周期推我方控（不禁 AI）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_gather.h"

#include "../ports/mob_gather_port.h"
#include "../ports/mob_fh_ban.h"
#include "../ports/teleport_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/log.h"
#include "../../../common/xcat_payload_control.h"

#include <Windows.h>

#include <atomic>

namespace x::features::mob_gather {
namespace {

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};

DWORD WINAPI WorkerMain(LPVOID) {
    runtime::LogI("MobGather", "worker start enabled=%d aim=%ums recruit=%ums",
                  ports::mob_gather::IsEnabled() ? 1 : 0, ports::mob_gather::AimIntervalMs(),
                  ports::mob_gather::RecruitIntervalMs());
    bool seqBoot = false;
    uint32_t lastSeq = 0;
    DWORD lastAim = 0;
    DWORD lastRecruit = 0;
    while (!gStop.load()) {
        const uint32_t seq = xcat::ReadMobGatherFireSeq(runtime::GetBinDir());
        if (!seqBoot) {
            seqBoot = true;
            lastSeq = seq;
        } else if (seq != 0 && seq > lastSeq) {
            lastSeq = seq;
            ports::mob_gather::OneshotResult r{};
            const bool ok = ports::mob_gather::TryPushOneshot(&r);
            runtime::LogI("MobGather", "session oneshot seq=%u ok=%d considered=%d pushed=%d why=%s",
                          seq, ok ? 1 : 0, r.considered, r.pushed, r.why ? r.why : "");
        }
        ports::mob_gather::TickHoldWatch();
        ports::mob_gather::TickDyLimRamp();
        ports::mob_gather::TickHomeRecord();
        ports::mob_gather::TickHomeReturn();
        ports::mob_gather::TickSeekCluster();
        const DWORD now = GetTickCount();
        const DWORD aimMs = ports::mob_gather::AimIntervalMs();
        ports::mob_gather::TickClearRelogin();
        if (ports::mob_gather::IsHoldActive()) {
            if (lastAim == 0 || now - lastAim >= aimMs) {
                lastAim = now;
                ports::teleport::FlightState st{};
                if (ports::teleport::QueryFlightState(st) && st.ok) {
                    ports::mob_fh_ban::TickPlayerAim(st.x, st.y, st.vx, st.vy, st.ma, nullptr,
                                                    nullptr);
                }
            }
            if (lastRecruit == 0 || now - lastRecruit >= ports::mob_gather::RecruitIntervalMs()) {
                lastRecruit = now;
                ports::mob_gather::OneshotResult r{};
                (void)ports::mob_gather::TryPushPeriodic(&r);
                ports::mob_gather::TickLandSweep();
            }
        } else {
            lastAim = 0;
            lastRecruit = 0;
        }
        ports::mob_gather::TickSoftRelogin();
        ports::mob_gather::TickClearRelogin();
        DWORD sleepMs = aimMs;
        if (lastAim != 0) {
            const DWORD age = GetTickCount() - lastAim;
            sleepMs = (age < aimMs) ? (aimMs - age) : 1;
        }
        Sleep(sleepMs);
    }
    runtime::LogI("MobGather", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    ports::mob_gather::Init();
    runtime::LogI("MobGather", "feature init (OFF; oneshot seq + periodic if armed)");
}

void Shutdown() {
    StopWorker();
    ports::mob_gather::Shutdown();
}

void StartWorker() {
    if (gThread.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
    if (th) gThread.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

}  // namespace x::features::mob_gather
