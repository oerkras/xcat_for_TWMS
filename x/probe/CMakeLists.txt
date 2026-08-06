# Injected payload DLL — LoadLibrary target for Classic inject.
# OUTPUT_NAME "xcat" → bin/XCat_data/xcat.dll
add_library(xcat_probe SHARED
  xcat_probe.cpp
  ../runtime/bin_dir.cpp
  ../runtime/log.cpp
  ../runtime/mono_clock.cpp
  ../runtime/il2cpp_bind.cpp
  ../runtime/il2cpp_shape.cpp
  ../runtime/il2cpp_method.cpp
  ../runtime/il2cpp_prefab.cpp
  ../runtime/il2cpp_container.cpp
  ../runtime/il2cpp_network.cpp
  ../runtime/il2cpp_mapdata.cpp
  ../runtime/main_thread_pump.cpp
  ../runtime/managed_main.cpp
  ../runtime/anchor_lamps.cpp
  ../ui/player_vitals.cpp
  ../features/invuln/invuln.cpp
  ../features/attack_accel/attack_accel.cpp
  ../features/final_attack_force/final_attack_force.cpp
  ../features/skill_max_level/skill_max_level.cpp
  ../features/kick_sniff/kick_sniff.cpp
  ../features/auto_enter/auto_enter.cpp
  ../features/ccu/ccu.cpp
  ../features/ccu/ccu_passive.cpp
  ../features/titlebar/titlebar.cpp
  ../features/titlebar/titlebar_win.cpp
  ../features/titlebar/titlebar_game.cpp
  ../features/ports/mob_pool_port.cpp
  ../features/ports/world_port.cpp
  ../features/ports/consumable_port.cpp
  ../features/ports/pet_port.cpp
  ../features/ports/drop_pool_port.cpp
  ../features/ports/input_port.cpp
  ../features/ports/skill_port.cpp
  ../features/ports/multi_skill_port.cpp
  ../features/ports/security_attack_port.cpp
  ../features/ports/travel_port.cpp
  ../features/ports/player_combat_port.cpp
  ../features/ports/action_gate.cpp
  ../features/ports/attack_input_port.cpp
  ../features/ports/teleport_port.cpp
  ../features/ports/foothold_port.cpp
  ../features/ports/foothold_path.cpp
  ../features/ports/map_bounds_port.cpp
  ../features/ports/user_pool_port.cpp
  ../features/ports/shop_port.cpp
  ../features/ports/attack_rpc_port.cpp
  ../features/attack_rpc/attack_rpc.cpp
  ../features/travel/travel_graph.cpp
  ../features/travel/travel.cpp
  ../features/worldmap_marker_travel/worldmap_marker_travel.cpp
  ../features/sellbag/sellbag.cpp
  ../features/auto_supply/auto_supply.cpp
  ../features/mob_scan/mob_scan.cpp
  ../features/autopot/autopot.cpp
  ../features/pet_feed/pet_feed.cpp
  ../features/pet_loot/pet_loot.cpp
  ../features/timed_keys/timed_keys.cpp
  ../features/buffs/buffs.cpp
  ../features/multi_skill/multi_skill.cpp
  ../features/simple_combat/simple_combat.cpp
  ../features/fly/fly.cpp
  ../features/drop_alert_bypass/drop_alert_bypass.cpp
  ../features/auction_town_bypass/auction_town_bypass.cpp
  ../features/ga_text_probe/ga_text_probe.cpp
  ../features/channel_hop/channel_hop.cpp
  ../features/encounter/encounter.cpp
  ../features/player_hide/player_hide.cpp
  ../features/frame_lock/frame_lock.cpp
  ../features/auto_lie/anti_macro_port.cpp
  ../features/auto_lie/anti_macro_follower.cpp
  ../features/auto_lie/auto_lie.cpp
  ../features/notify/notify.cpp
  ../ipc/payload_control.cpp
  ../ipc/payload_timed_keys.cpp
  ../ipc/payload_pet_loot.cpp
  ../ipc/payload_buffs.cpp
  ../ipc/payload_status.cpp
)
target_include_directories(xcat_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/invuln
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/attack_accel
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/final_attack_force
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/skill_max_level
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/kick_sniff
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/auto_enter
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/ccu
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/titlebar
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/autopot
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/ports
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/mob_scan
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/pet_feed
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/pet_loot
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/timed_keys
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/buffs
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/multi_skill
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/simple_combat
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/fly
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/drop_alert_bypass
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/auction_town_bypass
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/ga_text_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/channel_hop
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/encounter
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/player_hide
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/frame_lock
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/auto_lie
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/notify
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/travel
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/worldmap_marker_travel
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/sellbag
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/attack_rpc
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/auto_supply
  ${CMAKE_CURRENT_SOURCE_DIR}/../ui
  ${CMAKE_CURRENT_SOURCE_DIR}/../ipc
  ${CMAKE_SOURCE_DIR}/common
)
target_compile_definitions(xcat_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(xcat_probe PRIVATE xcat_common winmm Psapi User32)
if (MSVC)
  target_compile_options(xcat_probe PRIVATE /utf-8 /EHsc)
  set_property(TARGET xcat_probe PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
set_target_properties(xcat_probe PROPERTIES
  OUTPUT_NAME "xcat"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin/XCat_data
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
