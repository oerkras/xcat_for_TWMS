# 目录内文件增量同步：SRC_DIR 不存在则 STATUS 跳过（不重跑 dump，不 FATAL）。
# 用法：cmake -DSRC_DIR=... -DDST_DIR=... -P sync_dir_if_exists.cmake
if(NOT DEFINED SRC_DIR OR NOT DEFINED DST_DIR)
    message(FATAL_ERROR "sync_dir_if_exists requires SRC_DIR and DST_DIR")
endif()

if(NOT IS_DIRECTORY "${SRC_DIR}")
    message(STATUS "optional dir missing, skip sync: ${SRC_DIR}")
    return()
endif()

file(MAKE_DIRECTORY "${DST_DIR}")
file(GLOB _entries LIST_DIRECTORIES false "${SRC_DIR}/*")
set(_copied 0)
foreach(_src ${_entries})
    if(IS_DIRECTORY "${_src}")
        continue()
    endif()
    get_filename_component(_name "${_src}" NAME)
    # 不把仓库侧说明文档塞进运行目录（bin 里已有的 SOURCE.md 可保留，不覆盖也可）
    if(_name STREQUAL "SOURCE.md" OR _name STREQUAL "ANALYSIS_NOTES.md")
        continue()
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${DST_DIR}/${_name}"
        RESULT_VARIABLE _copy_result
    )
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "sync_dir_if_exists failed: ${_src} -> ${DST_DIR}/${_name}")
    endif()
    math(EXPR _copied "${_copied}+1")
endforeach()
message(STATUS "synced ${_copied} file(s): ${SRC_DIR} -> ${DST_DIR}")
