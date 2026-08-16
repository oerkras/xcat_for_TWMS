#include "payload_auto_skill.h"

#include "../features/auto_skill/auto_skill.h"
#include "../runtime/bin_dir.h"

#include "xcat_auto_skill.h"

#include <cstdint>
#include <cstring>

namespace x::ipc {
namespace {

uint64_t g_lastWriteTick = 0;
xcat::AutoSkillConfig g_lastConfig{};
bool g_hasLastConfig = false;

void PollOnce() {
    xcat::AutoSkillConfig cfg{};
    if (!xcat::ReadAutoSkill(runtime::GetBinDir(), cfg)) return;
    const bool sameTick = cfg.writeTickMs == g_lastWriteTick;
    const bool sameConfig =
        g_hasLastConfig && std::memcmp(&cfg, &g_lastConfig, sizeof(cfg)) == 0;
    if (sameTick && sameConfig) return;

    g_lastWriteTick = cfg.writeTickMs;
    g_lastConfig = cfg;
    g_hasLastConfig = true;
    features::auto_skill::ApplyConfig(cfg);
}

}  // namespace

void PayloadAutoSkill_ApplyInitial() {
    xcat::AutoSkillConfig cfg{};
    if (xcat::ReadAutoSkill(runtime::GetBinDir(), cfg)) {
        g_lastWriteTick = cfg.writeTickMs;
        g_lastConfig = cfg;
        g_hasLastConfig = true;
        features::auto_skill::ApplyConfig(cfg);
    }
}

void PayloadAutoSkill_Poll() { PollOnce(); }

}  // namespace x::ipc
