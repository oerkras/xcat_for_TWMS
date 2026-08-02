#include "payload_pet_loot.h"

#include "../features/pet_loot/pet_loot.h"
#include "../runtime/bin_dir.h"

#include "xcat_pet_loot.h"

#include <cstring>

namespace x::ipc {
namespace {

uint64_t g_lastWriteTick = 0;
xcat::PetLootConfig g_lastConfig{};
bool g_hasLastConfig = false;

void PollOnce() {
    xcat::PetLootConfig cfg{};
    if (!xcat::ReadPetLoot(runtime::GetBinDir(), cfg)) return;
    const bool sameTick = cfg.writeTickMs == g_lastWriteTick;
    const bool sameConfig =
        g_hasLastConfig && std::memcmp(&cfg, &g_lastConfig, sizeof(cfg)) == 0;
    if (sameTick && sameConfig) return;

    g_lastWriteTick = cfg.writeTickMs;
    g_lastConfig = cfg;
    g_hasLastConfig = true;
    features::pet_loot::ApplyConfig(cfg);
}

}  // namespace

void PayloadPetLoot_ApplyInitial() {
    xcat::PetLootConfig cfg{};
    if (xcat::ReadPetLoot(runtime::GetBinDir(), cfg)) {
        g_lastWriteTick = cfg.writeTickMs;
        g_lastConfig = cfg;
        g_hasLastConfig = true;
        features::pet_loot::ApplyConfig(cfg);
    }
}

void PayloadPetLoot_Poll() { PollOnce(); }

}  // namespace x::ipc
