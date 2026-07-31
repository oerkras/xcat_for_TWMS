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
    workspace_tabs.cpp
    runtime_leds.cpp
    status_bar.cpp
    single_instance.cpp
    update_client.cpp
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
    winhttp
    bcrypt
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
endif()

set_target_properties(xcat PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin
)
