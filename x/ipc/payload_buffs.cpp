#include "payload_buffs.h"

#include "../features/buffs/buffs.h"
#include "../runtime/bin_dir.h"

#include "xcat_buffs.h"

#include <cstring>

namespace x::ipc {
namespace {

uint64_t g_lastWriteTick = 0;
uint32_t g_lastRefreshSeq = 0;
xcat::BuffsConfig g_lastConfig{};
bool g_hasLastConfig = false;

void PollOnce() {
    xcat::BuffsConfig cfg{};
    if (!xcat::ReadBuffs(runtime::GetBinDir(), cfg)) return;
    const bool sameTick = cfg.writeTickMs == g_lastWriteTick;
    const bool sameRefresh = cfg.refreshSeq == g_lastRefreshSeq;
    const bool sameConfig =
        g_hasLastConfig && std::memcmp(&cfg, &g_lastConfig, sizeof(cfg)) == 0;
    if (sameTick && sameRefresh && sameConfig) return;

    g_lastWriteTick = cfg.writeTickMs;
    g_lastRefreshSeq = cfg.refreshSeq;
    g_lastConfig = cfg;
    g_hasLastConfig = true;
    features::buffs::ApplyConfig(cfg);
}

}  // namespace

void PayloadBuffs_ApplyInitial() {
    xcat::BuffsConfig cfg{};
    if (xcat::ReadBuffs(runtime::GetBinDir(), cfg)) {
        g_lastWriteTick = cfg.writeTickMs;
        g_lastRefreshSeq = cfg.refreshSeq;
        g_lastConfig = cfg;
        g_hasLastConfig = true;
        features::buffs::ApplyConfig(cfg);
    }
}

void PayloadBuffs_Poll() { PollOnce(); }

}  // namespace x::ipc
