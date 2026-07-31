cmake_minimum_required(VERSION 3.21)
project(xcat_twms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

foreach(_cfg "" "_DEBUG" "_RELEASE" "_RELWITHDEBINFO" "_MINSIZEREL")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${_cfg} ${CMAKE_SOURCE_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${_cfg} ${CMAKE_SOURCE_DIR}/bin)
endforeach()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY                ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG          ${CMAKE_BINARY_DIR}/lib/Debug)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE        ${CMAKE_BINARY_DIR}/lib/Release)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO ${CMAKE_BINARY_DIR}/lib/RelWithDebInfo)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL     ${CMAKE_BINARY_DIR}/lib/MinSizeRel)

include(${CMAKE_SOURCE_DIR}/cmake/imgui.cmake)

add_subdirectory(common)
add_subdirectory(sound_cli)
add_subdirectory(injector)
add_subdirectory(launcher)
add_subdirectory(x/probe)
add_subdirectory(xcat_app)
add_subdirectory(ops_app)
