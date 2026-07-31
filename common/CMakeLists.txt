add_library(xcat_common STATIC
    process_util.cpp
    xcat_log.cpp
    xcat_config_ini.cpp
    xcat_payload_control.cpp
    xcat_sound.cpp
    xcat_sound_presets.cpp
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
