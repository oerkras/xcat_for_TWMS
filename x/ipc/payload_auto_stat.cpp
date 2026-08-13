#include "payload_auto_stat.h"

#include "../features/auto_stat/auto_stat.h"
#include "../runtime/bin_dir.h"

#include "xcat_auto_stat.h"

#include <cstdint>
#include <cstring>

namespace x::ipc {
namespace {

uint64_t g_lastWriteTick = 0;
xcat::AutoStatConfig g_lastConfig{};
bool g_hasLastConfig = false;

void PollOnce() {
    xcat::AutoStatConfig cfg{};
    if (!xcat::ReadAutoStat(runtime::GetBinDir(), cfg)) return;
    const bool sameTick = cfg.writeTickMs == g_lastWriteTick;
    const bool sameConfig =
        g_hasLastConfig && std::memcmp(&cfg, &g_lastConfig, sizeof(cfg)) == 0;
    if (sameTick && sameConfig) return;

    g_lastWriteTick = cfg.writeTickMs;
    g_lastConfig = cfg;
    g_hasLastConfig = true;
    features::auto_stat::ApplyConfig(cfg);
}

}  // namespace

void PayloadAutoStat_ApplyInitial() {
    xcat::AutoStatConfig cfg{};
    if (xcat::ReadAutoStat(runtime::GetBinDir(), cfg)) {
        g_lastWriteTick = cfg.writeTickMs;
        g_lastConfig = cfg;
        g_hasLastConfig = true;
        features::auto_stat::ApplyConfig(cfg);
    }
}

void PayloadAutoStat_Poll() { PollOnce(); }

}  // namespace x::ipc
