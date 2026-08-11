add_executable(xcat_ops WIN32
    main.cpp
    ops_window.cpp
    ops_panel.cpp
    service_proc.cpp
    ops_health.cpp
    log_tail.cpp
    ops_app.rc
)

target_include_directories(xcat_ops PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/common_ui
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_sources(xcat_ops PRIVATE
    ${CMAKE_SOURCE_DIR}/common_ui/xcat_imgui_theme.cpp
    # 仅链地图名离线表，不拉整包 xcat_common（运维台刻意轻量）
    ${CMAKE_SOURCE_DIR}/common/xcat_map_names.cpp
)

target_link_libraries(xcat_ops PRIVATE
    imgui_lib
    xcat_util
    d3d11
    dxgi
    dwmapi
    advapi32
    user32
    shell32
    imm32
    winhttp
    iphlpapi
    ws2_32
)

target_compile_definitions(xcat_ops PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(xcat_ops PRIVATE /utf-8)
    set_property(TARGET xcat_ops PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# Keep ops console out of bin/ so clearing client runtime does not wipe ops.
set_target_properties(xcat_ops PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin_ops
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin_ops
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin_ops
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin_ops
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin_ops
)
