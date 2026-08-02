// attack_rpc feature — thin worker over attack_rpc_port（默认关）.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_rpc.h"

#include "../ports/attack_rpc_port.h"
#include "../ports/security_attack_port.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cstdlib>

namespace x::features::attack_rpc {
namespace {

std::atomic<bool> gStop{false};
std::atomic<HANDLE> gThread{nullptr};
DWORD gLastSecProbeMs = 0;
constexpr DWORD kTickMs = 40;
constexpr DWORD kSecProbeMs = 20000;

bool EnvEnabled() {
    char buf[8]{};
    if (GetEnvironmentVariableA("ATTACK_RPC", buf, sizeof(buf)) == 0) return false;
    return buf[0] == '1' || buf[0] == 'Y' || buf[0] == 'y' || buf[0] == 'T' || buf[0] == 't';
}

DWORD WINAPI WorkerMain(LPVOID) {
    runtime::LogI("AttackRpc", "worker start enabled=%d", ports::attack_rpc::IsEnabled() ? 1 : 0);
    ports::security_attack::Init();
    while (!gStop.load()) {
        ports::attack_rpc::Tick();
        const DWORD now = GetTickCount();
        if (ports::attack_rpc::IsEnabled() &&
            (!gLastSecProbeMs ||
             static_cast<DWORD>(now - gLastSecProbeMs) >= kSecProbeMs)) {
            gLastSecProbeMs = now;
            ports::security_attack::ProbeWindow();
        }
        Sleep(kTickMs);
    }
    runtime::LogI("AttackRpc", "worker stop");
    return 0;
}

}  // namespace

void Init() {
    ports::attack_rpc::Init();
    if (EnvEnabled()) {
        // Optional ramp via env (still requires ATTACK_RPC=1)
        char buf[32]{};
        if (GetEnvironmentVariableA("ATTACK_RPC_MOBS", buf, sizeof(buf)) > 0) {
            const int n = atoi(buf);
            if (n > 0) ports::attack_rpc::SetMaxMobs(n);
        }
        if (GetEnvironmentVariableA("ATTACK_RPC_MS", buf, sizeof(buf)) > 0) {
            const int ms = atoi(buf);
            if (ms > 0) ports::attack_rpc::SetIntervalMs(static_cast<DWORD>(ms));
        }
        if (GetEnvironmentVariableA("ATTACK_RPC_DMG", buf, sizeof(buf)) > 0) {
            const int d = atoi(buf);
            if (d > 0) ports::attack_rpc::SetDamage(d);
        }
        ports::attack_rpc::SetEnabled(true);
        runtime::LogW("AttackRpc",
                      "ATTACK_RPC=1 — experimental forge ON (mobs=%d ms=%lu dmg=%d)",
                      ports::attack_rpc::GetMaxMobs(),
                      static_cast<unsigned long>(ports::attack_rpc::GetIntervalMs()),
                      ports::attack_rpc::GetDamage());
    } else {
        runtime::LogI("AttackRpc", "feature init (OFF; set ATTACK_RPC=1 to enable)");
    }
}

void Shutdown() {
    StopWorker();
    ports::attack_rpc::Shutdown();
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

}  // namespace x::features::attack_rpc
