cmake_minimum_required(VERSION 3.20)
project(dsvr LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "DungeonSiege.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the DLLs have no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Always emit PDBs and map files so crash offsets from the event log can be resolved.
add_compile_options(/Zi)
add_link_options(/DEBUG /OPT:REF /OPT:ICF /MAP)

set(THIRD_PARTY ${CMAKE_SOURCE_DIR}/third_party)
set(OPENVR_DIR ${THIRD_PARTY}/openvr)
set(DGVOODOO_API_DIR ${THIRD_PARTY}/dgVoodooAPI)

# version.dll: loader injected via the exe's VERSION.dll import (engine hooks live here).
add_library(dsvr_loader SHARED
    src/loader/dllmain.cpp
    src/loader/camera_probe.cpp
    src/loader/camera_probe.h
    src/loader/camera_watch.cpp
    src/loader/camera_watch.h
    src/loader/head_tracking.cpp
    src/loader/head_tracking.h
    src/loader/version.def
    src/common/log.h
)
set_target_properties(dsvr_loader PROPERTIES OUTPUT_NAME version)
target_include_directories(dsvr_loader PRIVATE src)
target_compile_definitions(dsvr_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dsvr_loader PRIVATE /W4 /permissive-)

# SampleAddon.dll: dgVoodoo add-on (the only add-on name dgVoodoo 2.87 loads) that feeds SteamVR.
add_library(dsvr_addon SHARED
    src/addon/addon_main.cpp
    src/addon/frame_ring.cpp
    src/addon/frame_ring.h
    src/addon/vr_overlay.cpp
    src/addon/vr_overlay.h
    src/common/log.h
)
set_target_properties(dsvr_addon PROPERTIES OUTPUT_NAME SampleAddon)
target_include_directories(dsvr_addon PRIVATE src ${OPENVR_DIR}/headers ${DGVOODOO_API_DIR}/Inc)
target_compile_definitions(dsvr_addon PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(dsvr_addon PRIVATE /W4 /permissive-)
target_link_libraries(dsvr_addon PRIVATE
    ${OPENVR_DIR}/lib/win32/openvr_api.lib
    ${DGVOODOO_API_DIR}/Lib/x86/dgVoodooAddon.lib
    d3d11 dxgi
)
add_custom_command(TARGET dsvr_addon POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENVR_DIR}/bin/win32/openvr_api.dll $<TARGET_FILE_DIR:dsvr_addon>
)
