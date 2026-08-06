add_executable(xcat WIN32
    main.cpp
    app_window.cpp
    app_dpi.cpp
    app_font.cpp
    app_theme.cpp
    app_sound.cpp
    app_notify.cpp
    app_event_log.cpp
    log_upload.cpp
    log_upload_ui.cpp
    log_tail.cpp
    launch_panel.cpp
    attach_inject.cpp
    hangup_schedule.cpp
    guardian_policy.cpp
    workspace_tabs.cpp
    tdr_tune.cpp
    runtime_leds.cpp
    status_bar.cpp
    single_instance.cpp
    update_client.cpp
    lie_ai_pump.cpp
    anti_bot_ai/lie_ai_config.cpp
    anti_bot_ai/llm_http.cpp
    anti_bot_ai/llm_provider.cpp
    xcat_app.rc
    ${CMAKE_SOURCE_DIR}/common_ui/xcat_imgui_theme.cpp
)

target_include_directories(xcat PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common_ui
    ${CMAKE_SOURCE_DIR}/launcher
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(xcat PRIVATE
    imgui_lib
    xcat_common
    msc_launch
    xcat_inject
    d3d11
    dxgi
    dwmapi
    shell32
    user32
    ole32
    imm32
    advapi32
    comdlg32
    winhttp
    bcrypt
    iphlpapi
)

add_dependencies(xcat xcat_probe)

target_compile_definitions(xcat PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(xcat PRIVATE /utf-8)
    set_property(TARGET xcat PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    # 内嵌应用清单 requireAdministrator：双击 xcat.exe 即弹 UAC 以管理员运行。
    # 注入 / OpenProcess 等高权限操作需要管理员；模式对照仓枫星 xcat_app。
    target_link_options(xcat PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

set_target_properties(xcat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin
)

# 离线表 / 赶路 seed：仅 copy_if_different，不重跑 dump。源目录缺失则跳过。
add_custom_command(TARGET xcat POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:xcat>/XCat_data/dataservice"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:xcat>/XCat_data/state"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:xcat>/XCat_data/skill_catalog"
    COMMAND ${CMAKE_COMMAND}
            -DSRC_DIR="${CMAKE_SOURCE_DIR}/dumps/offline_tables/tsv"
            -DDST_DIR="$<TARGET_FILE_DIR:xcat>/XCat_data/dataservice"
            -P "${CMAKE_SOURCE_DIR}/cmake/sync_dir_if_exists.cmake"
    COMMAND ${CMAKE_COMMAND}
            -DSRC="${CMAKE_SOURCE_DIR}/dumps/offline_tables/tsv/skill_catalog_full.tsv"
            -DDST="$<TARGET_FILE_DIR:xcat>/XCat_data/skill_catalog/skill_catalog_full.tsv"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    COMMAND ${CMAKE_COMMAND}
            -DSRC="${CMAKE_SOURCE_DIR}/dumps/offline_tables/SOURCE.md"
            -DDST="$<TARGET_FILE_DIR:xcat>/XCat_data/dataservice/SOURCE.md"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    COMMAND ${CMAKE_COMMAND}
            -DSRC="${CMAKE_SOURCE_DIR}/dumps/twms_routes/travel_graph.seed.tsv"
            -DDST="$<TARGET_FILE_DIR:xcat>/XCat_data/state/travel_graph.seed.tsv"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    COMMAND ${CMAKE_COMMAND}
            -DSRC="${CMAKE_SOURCE_DIR}/dumps/twms_routes/travel_catalog.tsv"
            -DDST="$<TARGET_FILE_DIR:xcat>/XCat_data/state/travel_catalog.tsv"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    COMMAND ${CMAKE_COMMAND}
            -DSRC="${CMAKE_SOURCE_DIR}/dumps/twms_routes/travel_harbor.tsv"
            -DDST="$<TARGET_FILE_DIR:xcat>/XCat_data/state/travel_harbor.tsv"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    COMMAND ${CMAKE_COMMAND}
            -DSRC="${CMAKE_SOURCE_DIR}/dumps/twms_routes/travel_script_portal.tsv"
            -DDST="$<TARGET_FILE_DIR:xcat>/XCat_data/state/travel_script_portal.tsv"
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_if_exists.cmake"
    COMMENT "Sync offline dataservice TSV + twms travel seeds (copy_if_different only)"
)
