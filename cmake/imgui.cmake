# Prefer sibling fengxing FetchContent cache when offline
set(_XCAT_IMGUI_CACHE "${CMAKE_SOURCE_DIR}/../xcat_for_fengxing/build/_deps/imgui-src")
if(EXISTS "${_XCAT_IMGUI_CACHE}/imgui.h")
  set(imgui_SOURCE_DIR "${_XCAT_IMGUI_CACHE}")
  message(STATUS "imgui: using cache ${_XCAT_IMGUI_CACHE}")
else()
  include(FetchContent)
  FetchContent_Declare(
      imgui
      GIT_REPOSITORY https://github.com/ocornut/imgui.git
      GIT_TAG        v1.91.8
      GIT_SHALLOW    TRUE
      UPDATE_DISCONNECTED TRUE
  )
  FetchContent_MakeAvailable(imgui)
endif()

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_compile_definitions(imgui_lib PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
if(MSVC)
    target_compile_options(imgui_lib PRIVATE /utf-8 /MP)
    set_property(TARGET imgui_lib PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
