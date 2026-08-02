# 可选单文件增量拷贝：SRC 不存在则 STATUS 跳过（不 FATAL）。
# 用法：cmake -DSRC=... -DDST=... -P copy_if_exists.cmake
if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "copy_if_exists requires SRC and DST")
endif()

if(EXISTS "${SRC}")
    get_filename_component(_dst_dir "${DST}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}"
        RESULT_VARIABLE _copy_result
    )
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "copy_if_exists failed: ${SRC} -> ${DST}")
    endif()
else()
    message(STATUS "optional file missing, skip copy: ${SRC}")
endif()
