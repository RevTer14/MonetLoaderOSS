cmake_minimum_required(VERSION 3.21.0)
set(CMAKE_EXPORT_COMPILE_COMMANDS)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_SCAN_FOR_MODULES 0)

# ------ Build options ------
# -Wno-format-security because somebody thought that warning for obfuscated log is good idea.
add_compile_options(-ffunction-sections -fdata-sections -fno-rtti -fexceptions
    -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.
    -Wall -Wshadow -Wno-format-security -Wno-unused-private-field)

if(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    add_compile_options(-mfloat-abi=softfp)
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    add_compile_options(-g0 -fvisibility=hidden -fvisibility-inlines-hidden)
    add_link_options(-Wl,--gc-sections,--strip-all,--exclude-libs,ALL)
else()
    # vvv reduce debug build sizes vvv
    add_compile_options(-g0)
    add_link_options(-Wl,--gc-sections)
endif()

# ------ Project ------
project(monetloader VERSION 3.8.0)
add_library(${PROJECT_NAME} SHARED)
include_directories(${PROJECT_SOURCE_DIR}/cpp)
include_directories(${PROJECT_SOURCE_DIR}/lib)
include_directories(${PROJECT_SOURCE_DIR}/lib/lua)

# ------ Libraries ------
# spdlog
add_subdirectory(lib/spdlog)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)

# LuaJIT
set(LUAJIT_SHARED ON)

if(LUAJIT_SHARED)
    set(LUAJIT_SUFFIX .so)
    set(LUAJIT_TYPE shared)
else()
    set(LUAJIT_SUFFIX .a)
    set(LUAJIT_TYPE static)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/lua/bin/${ANDROID_ABI}/${LUAJIT_TYPE}_debug/libluajit-5.1${LUAJIT_SUFFIX})
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/lib/lua/bin/${ANDROID_ABI}/${LUAJIT_TYPE}_release/libluajit-5.1${LUAJIT_SUFFIX})
endif()

# LibreSSL
set(LIBRESSL_APPS OFF)
set(LIBRESSL_TESTS OFF)
add_subdirectory(lib/libressl)
target_link_libraries(${PROJECT_NAME} PRIVATE crypto ssl tls)

# ------ Project sources ------
include(lib/CMakeLists.txt)
include(cpp/CMakeLists.txt)


# ------ Definitions ------
# Build data
set(SAMP_MODE 0) # 0 - disabled, 1 - optional, 2 - required
string(TIMESTAMP BUILD_TIMESTAMP "%s")
target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_BUILD_TIMESTAMP=${BUILD_TIMESTAMP}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "SAMP_MODE=${SAMP_MODE}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "NEW_INPUT_SYSTEM=1")

# Version
if(SAMP_MODE EQUAL 0)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION=\"${PROJECT_VERSION}-sa\"")
elseif(SAMP_MODE EQUAL 1)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION=\"${PROJECT_VERSION}-os\"")
elseif(SAMP_MODE EQUAL 2)
    target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION=\"${PROJECT_VERSION}-rs\"")
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION_MINOR=${PROJECT_VERSION_MINOR}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION_PATCH=${PROJECT_VERSION_PATCH}")
math(EXPR VERSION_INDEX "${PROJECT_VERSION_MAJOR} * 1000000 + ${PROJECT_VERSION_MINOR} * 1000 + ${PROJECT_VERSION_PATCH}")
target_compile_definitions(${PROJECT_NAME} PRIVATE "PROJECT_VERSION_INDEX=${VERSION_INDEX}")

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
