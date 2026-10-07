# fh2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(fh2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FH2_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(fh2 WIN32 ${FH2_SOURCES})
else()
    add_executable(fh2 ${FH2_SOURCES})
endif()

rexglue_setup_target(fh2)

# PC build with the SDK as a subdirectory: rex_app.cpp includes imgui.h and rexruntime links imgui
# privately, so its include path does not reach the executable (same fix as nfsmw-android). The same
# goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes outside the Switch.
if(TARGET imgui)
    target_include_directories(fh2 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET renderdoc)
    target_include_directories(fh2 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
