#include "payload_timed_keys.h"

#include "../features/timed_keys/timed_keys.h"
#include "../runtime/bin_dir.h"

#include "xcat_timed_keys.h"

#include <cstring>

namespace x::ipc {
namespace {

uint64_t g_lastWriteTick = 0;
xcat::TimedKeysConfig g_lastConfig{};
bool g_hasLastConfig = false;

void PollOnce() {
    xcat::TimedKeysConfig cfg{};
    if (!xcat::ReadTimedKeys(runtime::GetBinDir(), cfg)) return;
    const bool sameTick = cfg.writeTickMs == g_lastWriteTick;
    const bool sameConfig =
        g_hasLastConfig && std::memcmp(&cfg, &g_lastConfig, sizeof(cfg)) == 0;
    if (sameTick && sameConfig) return;

    g_lastWriteTick = cfg.writeTickMs;
    g_lastConfig = cfg;
    g_hasLastConfig = true;
    features::timed_keys::ApplyConfig(cfg);
}

}  // namespace

void PayloadTimedKeys_ApplyInitial() {
    xcat::TimedKeysConfig cfg{};
    if (xcat::ReadTimedKeys(runtime::GetBinDir(), cfg)) {
        g_lastWriteTick = cfg.writeTickMs;
        g_lastConfig = cfg;
        g_hasLastConfig = true;
        features::timed_keys::ApplyConfig(cfg);
    }
}

void PayloadTimedKeys_Poll() { PollOnce(); }

}  // namespace x::ipc
