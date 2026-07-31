# Injected payload DLL — LoadLibrary target for Classic inject.
# OUTPUT_NAME "xcat" → bin/XCat_data/xcat.dll
add_library(xcat_probe SHARED
  xcat_probe.cpp
  ../runtime/bin_dir.cpp
  ../runtime/log.cpp
  ../features/fly/fly.cpp
  ../features/fly/fly_impl.cpp
  ../features/invuln/invuln.cpp
  ../features/kick_sniff/kick_sniff.cpp
  ../features/auto_enter/auto_enter.cpp
  ../features/titlebar/titlebar.cpp
  ../ipc/payload_control.cpp
)
target_include_directories(xcat_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/fly
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/invuln
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/kick_sniff
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/auto_enter
  ${CMAKE_CURRENT_SOURCE_DIR}/../features/titlebar
  ${CMAKE_CURRENT_SOURCE_DIR}/../ipc
  ${CMAKE_SOURCE_DIR}/common
)
target_compile_definitions(xcat_probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
target_link_libraries(xcat_probe PRIVATE xcat_common winmm Psapi User32)
if (MSVC)
  target_compile_options(xcat_probe PRIVATE /utf-8 /EHsc)
  set_property(TARGET xcat_probe PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
set_target_properties(xcat_probe PROPERTIES
  OUTPUT_NAME "xcat"
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_SOURCE_DIR}/bin/XCat_data
  RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${CMAKE_SOURCE_DIR}/bin/XCat_data
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/XCat_data
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
)
