#pragma once
// MapData / PortalManager / WM.currentMapData 字段防漂 SSOT（经典版 TWMS）。
// hash → field_get_offset；dump 常量仅 fallback。

#include <cstddef>

namespace x::runtime::il2cpp_mapdata {

void Ensure();

// WorldManager → MapData*
size_t OffWmMapData();  // fb 0x88

// MapData（hash a2eca01a… · remount 2026-08-06）
size_t OffMapId();         // fb 0x10
size_t OffMapLifeList();   // fb 0x38 List<MapLifeData>
size_t OffMapPortals();    // fb 0x40 List<MapPortalData>
size_t OffFootholdMap();   // fb 0xE0 Dictionary<uint,StaticFoothold>
size_t OffLadderRopes();   // fb 0x108 List<LadderOrRope>

// PortalManager（hash cce70e13…）
size_t OffPmPortalList();  // fb 0x10 List<Portal>

}  // namespace x::runtime::il2cpp_mapdata
