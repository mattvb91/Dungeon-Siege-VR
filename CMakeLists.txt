cmake_minimum_required(VERSION 3.20)
project(dsvr LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "DungeonSiege.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLL has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(dsvr SHARED
    src/dllmain.cpp
    src/log.h
    src/version.def
)
set_target_properties(dsvr PROPERTIES OUTPUT_NAME version)
target_compile_definitions(dsvr PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dsvr PRIVATE /W4 /permissive-)
