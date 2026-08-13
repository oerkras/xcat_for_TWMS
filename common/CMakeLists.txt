# 轻量工具库：进程/日志/INI。运维台只链这个，避免编进整包游戏逻辑。
add_library(xcat_util STATIC
    process_util.cpp
    xcat_log.cpp
    xcat_config_ini.cpp
    xcat_access_deny_sticky.cpp
)

target_include_directories(xcat_util PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(xcat_util PUBLIC shell32 ole32)

if(MSVC)
    target_compile_options(xcat_util PRIVATE /utf-8)
    set_property(TARGET xcat_util PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# 启动器/注入侧完整公共库（含自动化/地图/音效等）
add_library(xcat_common STATIC
    xcat_payload_control.cpp
    xcat_payload_status.cpp
    xcat_anchor_lamps.cpp
    xcat_multiskill_select.cpp
    xcat_timed_keys.cpp
    xcat_pet_loot.cpp
    xcat_auto_stat.cpp
    xcat_buffs.cpp
    xcat_worlds_cache.cpp
    xcat_world_names.cpp
    xcat_map_names.cpp
    xcat_map_bounds.cpp
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

target_link_libraries(xcat_common PUBLIC xcat_util winmm)

if(MSVC)
    target_compile_options(xcat_common PRIVATE /utf-8)
    set_property(TARGET xcat_common PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
