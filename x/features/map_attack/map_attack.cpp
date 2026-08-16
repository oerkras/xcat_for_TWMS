// map_attack — P2 扩盒。不碰 simple_combat Firing，不 Encode。
#include "map_attack.h"

#include "../ports/map_attack_port.h"
#include "../../runtime/log.h"

namespace x::features::map_attack {

void Init() {
    ports::map_attack::Init();
    runtime::LogI("MapAtk", "feature init");
}

void Shutdown() {
    ports::map_attack::Shutdown();
    runtime::LogI("MapAtk", "feature shutdown");
}

}  // namespace x::features::map_attack
