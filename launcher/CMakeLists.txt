# Subdirectory of root xcat_twms — NGM core + login session (embedded in xcat)

add_library(msc_launch STATIC
  msc_launch.cpp
  ott_ticket_fetch.cpp
  http_client.cpp
  http_beanfun_login.cpp
  http_gamapass_login.cpp
  chromium_cdp.cpp
  gamapass_cdp_login.cpp
  gamapass_ticket_harvest.cpp
  gamapass_uia_login.cpp
  gamapass_device_login.cpp
  gamapass_login_phase.cpp
  ngm_protocol_allow.cpp
  win_uia.cpp
  msc_webview_login.cpp
)
target_include_directories(msc_launch PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
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
  ws2_32
  winhttp
  ole32
  oleaut32
  shlwapi
  version
  bcrypt
  crypt32
  rpcrt4
  UIAutomationCore
  xcat_inject
)

add_executable(msc_launch_smoke msc_launch_smoke.cpp)
target_link_libraries(msc_launch_smoke PRIVATE msc_launch)
target_compile_definitions(msc_launch_smoke PRIVATE UNICODE _UNICODE)
if (MSVC)
  target_compile_options(msc_launch_smoke PRIVATE /utf-8)
  set_property(TARGET msc_launch_smoke PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
set_target_properties(msc_launch_smoke PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin
)
