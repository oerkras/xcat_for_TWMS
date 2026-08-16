# Injected payload DLL — LoadLibrary target for Classic inject.
# OUTPUT_NAME "xcat" → bin/XCat_data/xcat.dll（仅 Release / RelWithDebInfo / MinSizeRel）
# Debug 单独落到 bin/XCat_data/debug/，避免漏写 --config Release 时静默覆盖注入路径。
add_library(xcat_probe SHARED
  xcat_probe.cpp
  ../runtime/bin_dir.cpp
  ../runtime/log.cpp
  ../runtime/mono_clock.cpp
  ../runtime/il2cpp_bind.cpp
  ../runtime/il2cpp_shape.cpp
  ../runtime/il2cpp_method.cpp
  ../runtime/il2cpp_metadata_lock.cpp
  ../runtime/il2cpp_fault_probe.cpp
  ../runtime/il2cpp_prefab.cpp
  ../runtime/il2cpp_container.cpp
  ../runtime/il2cpp_network.cpp
  ../runtime/il2cpp_mapdata.cpp
  ../runtime/main_thread_pump.cpp
  ../runtime/managed_main.cpp
  ../runtime/hang_autopsy.cpp
  ../features/crash_upload_guard/crash_upload_guard.cpp
  ../runtime/anchor_lamps.cpp
  ../ui/player_vitals.cpp
  ../features/invuln/invuln.cpp
  ../features/attack_accel/attack_accel.cpp
  ../features/final_attack_force/final_attack_force.cpp
  ../features/melee_veto/melee_veto.cpp
  ../features/skill_max_level/skill_max_level.cpp
  ../features/kick_sniff/kick_sniff.cpp
  ../features/auto_enter/auto_enter.cpp
  ../features/ccu/ccu.cpp
  ../features/ccu/ccu_passive.cpp
  ../features/titlebar/titlebar.cpp
  ../features/titlebar/titlebar_win.cpp
  ../features/titlebar/titlebar_game.cpp
  ../features/ports/mob_pool_port.cpp
  ../features/ports/hit_pin_port.cpp
  ../features/ports/map_attack_port.cpp
  ../features/ports/mob_gather_port.cpp
  ../features/ports/mob_fh_ban.cpp
  ../features/ports/world_port.cpp
  ../features/ports/consumable_port.cpp
  ../features/ports/pet_port.cpp
  ../features/ports/drop_pool_port.cpp
  ../features/ports/input_port.cpp
  ../features/ports/unity_kbd_port.cpp
  ../features/ports/skill_port.cpp
  ../features/ports/multi_skill_port.cpp
  ../features/ports/security_attack_port.cpp
  ../features/ports/travel_port.cpp
  ../features/ports/player_combat_port.cpp
  ../features/ports/action_gate.cpp
  ../features/ports/attack_input_port.cpp
  ../features/ports/keypad_walk_bin.cpp
  ../features/ports/key_macro_bin.cpp
  ../features/ports/teleport_port.cpp
  ../features/ports/fly_fh_ban.cpp
  ../features/ports/ground_spoof.cpp
  ../features/ports/curfh_gate_bypass.cpp
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
  ../features/char_boot/char_boot.cpp
  ../features/mob_scan/mob_scan.cpp
  ../features/autopot/autopot.cpp
  ../features/auto_stat/auto_stat.cpp
  ../features/auto_skill/auto_skill.cpp
  ../features/pet_feed/pet_feed.cpp
  ../features/pet_loot/pet_loot.cpp
  ../features/timed_keys/timed_keys.cpp
  ../features/buffs/buffs.cpp
  ../features/multi_skill/multi_skill.cpp
  ../features/simple_combat/simple_combat.cpp
  ../features/simple_combat/heli_rotor.cpp
  ../features/simple_combat/reach_cal.cpp
  ../features/map_attack/map_attack.cpp
  ../features/mob_gather/mob_gather.cpp
  ../features/fly/fly.cpp
  ../features/drop_alert_bypass/drop_alert_bypass.cpp
  ../features/auction_town_bypass/auction_town_bypass.cpp
  ../features/rest_mp_accel/rest_mp_accel.cpp
  ../features/infinite_stars/infinite_stars.cpp
  ../features/ga_text_probe/ga_text_probe.cpp
  ../features/galaxy_token_probe/galaxy_token_probe.cpp
  ../features/soft_login_probe/soft_login_probe.cpp
  ../features/channel_hop/channel_hop.cpp
  ../features/encounter/encounter.cpp
  ../features/player_hide/player_hide.cpp
  ../features/frame_lock/frame_lock.cpp
  ../features/auto_lie/anti_macro_port.cpp
  ../features/auto_lie/anti_macro_follower.cpp
  ../features/auto_lie/mouse_region_overlay.cpp
  ../features/auto_lie/mouse_trajectory_sim.cpp
  ../features/auto_lie/lie_log.cpp
  ../features/auto_lie/lie_stats.cpp
  ../features/auto_lie/auto_lie.cpp
  ../features/movepath_flush_probe/movepath_flush_probe.cpp
  ../features/notify/notify.cpp
  ../ipc/payload_control.cpp
  ../ipc/payload_timed_keys.cpp
  ../ipc/payload_pet_loot.cpp
  ../ipc/payload_auto_stat.cpp
  ../ipc/payload_auto_skill.cpp
  ../ipc/payload_buffs.cpp
  ../ipc/payload_status.cpp
)
target_include_directories(xcat_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/invuln
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/attack_accel
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/final_attack_force
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/melee_veto
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/map_attack
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/mob_gather
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/fly
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/drop_alert_bypass
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/auction_town_bypass
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/rest_mp_accel
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/infinite_stars
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/ga_text_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/galaxy_token_probe
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/soft_login_probe
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
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/char_boot
  ${CMAKE_CURRENT_SOURCE_DIR}/../ui
  ${CMAKE_CURRENT_SOURCE_DIR}/../ipc
  ${CMAKE_SOURCE_DIR}/common
)
target_compile_definitions(xcat_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(xcat_probe PRIVATE xcat_common winmm Psapi User32)
if (MSVC)
  target_compile_options(xcat_probe PRIVATE /utf-8 /EHsc)
  # 卡死取证（hang_autopsy）落盘的只有 `xcat.dll+0xRVA`。没有链接器 PDB 就永远翻不成函数名，
  # 2026-08-09 主线程死锁栈里那 6 帧 XCat 代码就因此只能靠猜。所有配置都产出 PDB。
  # /PDBALTPATH: 只把文件名写进 PE 的 RSDS，避免本机绝对路径（含 Windows 用户名）随 DLL 外发。
  # 磁盘上的真实 PDB 仍由 PDB_OUTPUT_DIRECTORY 决定，本地 hang 反查不受影响。
  # 显式补回 /OPT:REF /OPT:ICF：MSVC 在给了 /DEBUG 之后会把这两项默认关掉，不补就等于
  # 顺手改了 Release 的代码布局与体积，可能把要查的时序问题一起盖掉。
  target_link_options(xcat_probe PRIVATE
      /DEBUG
      /PDBALTPATH:xcat.pdb
      $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
      $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
  )
  set_property(TARGET xcat_probe PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
set_target_properties(xcat_probe PROPERTIES
  OUTPUT_NAME "xcat"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  # Debug 不得覆盖注入路径：漏 --config Release 时最多「DLL 没更新」，
  # 不会悄悄换成时序完全不同的 Debug 版（本仓主泵对时序极敏感）。
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/XCat_data/debug
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin/XCat_data
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
  # 逐配置写死，理由同上面的 RUNTIME_OUTPUT_DIRECTORY_*：VS 这类多配置生成器会给
  # 不带 _<CONFIG> 后缀的输出目录再追加一层 Release/Debug，PDB 就跟 DLL 分家了。
  PDB_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  PDB_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/XCat_data/debug
  PDB_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/XCat_data
  PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin/XCat_data
  PDB_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin/XCat_data
)
