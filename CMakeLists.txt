cmake_minimum_required(VERSION 3.15)

cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "/DEBUG:FULL /INCREMENTAL:NO" CACHE STRING "")
set(CMAKE_BUILD_TYPE "Release" CACHE STRING "")

project(ExtendedTrace)

add_library(ExtendedTrace SHARED
    src/pluginmain.cpp
    src/plugin.cpp
    src/tracer.cpp
    src/dialog.cpp
    src/pedumper.cpp
    src/pluginmain.h
    src/plugin.h
    src/tracer.h
    src/dialog.h
    src/pedumper.h
)

target_include_directories(ExtendedTrace PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/3rd_party)
target_link_directories(ExtendedTrace PRIVATE ${CMAKE_SOURCE_DIR}/3rd_party/pluginsdk)

target_link_libraries(ExtendedTrace PRIVATE x64dbg x64bridge)
set_target_properties(ExtendedTrace PROPERTIES SUFFIX ".dp64")

target_link_libraries(ExtendedTrace PRIVATE comctl32 shlwapi)
target_compile_definitions(ExtendedTrace PRIVATE "-DPLUGIN_NAME=\"ExtendedTrace\"")
