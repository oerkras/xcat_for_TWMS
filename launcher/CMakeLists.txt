# Subdirectory of root xcat_twms — NGM core + WebView login session (embedded in xcat)

set(WV2_ROOT "${CMAKE_SOURCE_DIR}/third_party/webview2")
set(WV2_INCLUDE "${WV2_ROOT}/build/native/include")
set(WV2_LIB_DIR "${WV2_ROOT}/build/native/x64")

add_library(msc_launch STATIC
  msc_launch.cpp
  ott_ticket_fetch.cpp
  msc_webview_login.cpp
)
target_include_directories(msc_launch PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${WV2_INCLUDE}
  ${CMAKE_SOURCE_DIR}/injector
)
target_compile_definitions(msc_launch PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if (MSVC)
  target_compile_options(msc_launch PUBLIC /utf-8)
  set_property(TARGET msc_launch PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
target_link_libraries(msc_launch PUBLIC
  shell32
  advapi32
  winhttp
  ole32
  shlwapi
  version
  xcat_inject
  "${WV2_LIB_DIR}/WebView2LoaderStatic.lib"
)

# 可选独立窗口（调试用）；默认仍产出到 bin，主路径已聚合进 xcat.exe
option(XCAT_BUILD_MSC_LAUNCHER_EXE "Build standalone MscLauncher.exe" OFF)
if (XCAT_BUILD_MSC_LAUNCHER_EXE)
  add_executable(msc_launcher WIN32 msc_launcher_app.cpp)
  target_include_directories(msc_launcher PRIVATE ${WV2_INCLUDE})
  target_link_libraries(msc_launcher PRIVATE msc_launch)
  target_compile_definitions(msc_launcher PRIVATE UNICODE _UNICODE)
  if (MSVC)
    target_compile_options(msc_launcher PRIVATE /utf-8)
    set_property(TARGET msc_launcher PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
  endif()
  set_target_properties(msc_launcher PROPERTIES
    OUTPUT_NAME "MscLauncher"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin
  )
endif()

add_executable(msc_launch_smoke msc_launch_smoke.cpp)
target_link_libraries(msc_launch_smoke PRIVATE msc_launch)
target_compile_definitions(msc_launch_smoke PRIVATE UNICODE _UNICODE)
if (MSVC)
  target_compile_options(msc_launch_smoke PRIVATE /utf-8)
endif()
set_target_properties(msc_launch_smoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
)
