# Classic LoadLibraryW injector (no Blackbone / no stealth)

add_library(xcat_inject STATIC
  classic_loadlibrary.cpp
  inject_after_launch.cpp
)
target_include_directories(xcat_inject PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/common
)
target_link_libraries(xcat_inject PUBLIC xcat_common psapi)
target_compile_definitions(xcat_inject PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
if (MSVC)
  target_compile_options(xcat_inject PRIVATE /utf-8)
  set_property(TARGET xcat_inject PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
  )
endif()
