add_library(xcat_common STATIC
    process_util.cpp
    xcat_log.cpp
    xcat_config_ini.cpp
    xcat_payload_control.cpp
    xcat_payload_status.cpp
    xcat_anchor_lamps.cpp
    xcat_multiskill_select.cpp
    xcat_timed_keys.cpp
    xcat_pet_loot.cpp
    xcat_buffs.cpp
    xcat_worlds_cache.cpp
    xcat_world_names.cpp
    xcat_map_names.cpp
    xcat_item_catalog.cpp
    xcat_sellbag.cpp
    xcat_auto_supply.cpp
    xcat_skill_names.cpp
    xcat_sound.cpp
    xcat_sound_presets.cpp
    xcat_payload_notify.cpp
)

target_include_directories(xcat_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(xcat_common PUBLIC winmm)

if(MSVC)
    target_compile_options(xcat_common PRIVATE /utf-8)
    set_property(TARGET xcat_common PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
