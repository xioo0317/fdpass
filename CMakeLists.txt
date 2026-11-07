cmake_minimum_required(VERSION 3.22)
project(fdpass LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(fdpass SHARED
    src/fdpass_core.c
    src/fdpass_jni.c
)

target_include_directories(fdpass PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 只导出 3 个 JNIEXPORT 函数，其余（core 辅助）全部 hidden
set_target_properties(fdpass PROPERTIES
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME fdpass
)

if(ANDROID)
    # NDK sysroot 自带 jni.h，无需 find_package(JNI)
else()
    find_package(JNI REQUIRED)
    target_include_directories(fdpass PRIVATE ${JNI_INCLUDE_DIRS})
endif()

target_compile_options(fdpass PRIVATE -Wall -Wextra -Werror -O2)
