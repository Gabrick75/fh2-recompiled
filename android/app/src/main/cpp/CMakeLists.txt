cmake_minimum_required(VERSION 3.22.1)
project(fh2_android LANGUAGES C CXX)

# Gradle runs ninja without -j, so it would compile as many files as there are cores (12 here). A generated file
# of the game takes up to ~1.4 GB to compile, so cap the compile jobs to fit the build machine's RAM.
set(FH2_ANDROID_COMPILE_JOBS 6 CACHE STRING "Parallel compile jobs of the Android build")
set_property(GLOBAL APPEND PROPERTY JOB_POOLS fh2_compile=${FH2_ANDROID_COMPILE_JOBS} fh2_link=1)
set(CMAKE_JOB_POOL_COMPILE fh2_compile)
set(CMAKE_JOB_POOL_LINK fh2_link)

# GPU probe for the launcher (diagnostic report and the driver test, GpuProbeActivity): Vulkan version, features
# and a logical device, without the game, through the same loader as the game (the system driver or an imported
# one, rex_android_gpu_driver).
add_library(fh2_diagnostics SHARED android_diagnostico.cpp)
target_include_directories(fh2_diagnostics PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../sdk/thirdparty/vulkan-headers/include"
    "${CMAKE_CURRENT_LIST_DIR}/../../../../../sdk/include")
target_compile_options(fh2_diagnostics PRIVATE -march=armv8-a)
target_link_libraries(fh2_diagnostics PRIVATE rex_android_gpu_driver dl)
target_link_options(fh2_diagnostics PRIVATE "-Wl,-z,max-page-size=16384")

# CPU target of all the native code (the game, the SDK, the GPU plugin). armv8-a runs on every 64-bit phone;
# armv8.2-a (inline LSE atomics instead of the outline helpers, Snapdragon 845 and newer) measured the same on the
# S23 Ultra (event menu 19.0 vs 19.1 fps), so the compatible one is the default.
set(FH2_ANDROID_MARCH "armv8-a" CACHE STRING "-march of the Android native code")
set(REXGLUE_ANDROID_MARCH "${FH2_ANDROID_MARCH}" CACHE STRING "" FORCE)
string(APPEND CMAKE_C_FLAGS " -march=${FH2_ANDROID_MARCH}")
string(APPEND CMAKE_CXX_FLAGS " -march=${FH2_ANDROID_MARCH}")

get_filename_component(FH2_REXSDK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../sdk" ABSOLUTE)
set(REXSDK_DIR "${FH2_REXSDK_DIR}" CACHE PATH "ReXGlue SDK source tree")
set(REXGLUE_USE_VULKAN ON CACHE BOOL "Build ReXGlue's Vulkan backend" FORCE)
set(REXGLUE_USE_D3D12 OFF CACHE BOOL "Android uses Vulkan" FORCE)
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Disable Tracy on Android" FORCE)
set(REXGLUE_VERSION_OVERRIDE "0.10.0-dev.android" CACHE STRING "ReXGlue SDK version for Android")
# The generated guest code is ~330 MB of C++: no debug tables for it.
set(REXGLUE_RECOMP_DEBUG_INFO "none" CACHE STRING "Omit debug tables for the large generated guest code")
set(SDL_SHARED ON CACHE BOOL "SDL shared library loaded by SDLActivity" FORCE)
set(SDL_STATIC OFF CACHE BOOL "Use SDL's shared Android library" FORCE)

# The game (app/) as libmain.so, plus its two DLLs (libfh2_XMediaFacade_default.so and
# libfh2_SpeechFacade_default.so, loaded at run time by XexLoadImage through the soname) and the Xenos
# GPU plugin (librexgpu-xenos.so). The generated sources must already exist: rexglue is a host tool and is
# not built here (run tools/codegen or rexglue codegen on the PC first).
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../../../app" "fh2-app")
target_sources(fh2 PRIVATE "${CMAKE_CURRENT_LIST_DIR}/android_touch.cpp"
                           "${CMAKE_CURRENT_LIST_DIR}/android_rendimiento.cpp"
                           "${CMAKE_CURRENT_LIST_DIR}/android_hdr.cpp"
                           "${CMAKE_CURRENT_LIST_DIR}/android_vibration.cpp")

if(TARGET SDL3-shared)
    set_target_properties(SDL3-shared PROPERTIES
        OUTPUT_NAME "SDL3"
        DEBUG_POSTFIX ""
        RELEASE_POSTFIX ""
        RELWITHDEBINFO_POSTFIX ""
        MINSIZEREL_POSTFIX ""
    )
endif()

# The launcher's home screen video (HomeVideo.java): the game's PressStart.wmv through the libavcodec the game
# links (WMV3 and WMA Pro), shown on a TextureView with the sound through AAudio.
add_library(fh2_home_video SHARED home_video.cpp android_home_video.cpp)
target_link_libraries(fh2_home_video PRIVATE libavcodec libavutil android aaudio log)
target_link_options(fh2_home_video PRIVATE "-Wl,-z,max-page-size=16384"
    "LINKER:--exclude-libs,liblibavcodec.a:liblibavutil.a")

# The namespace hooks adrenotools opens from nativeLibraryDir to load an imported driver: built and packaged.
add_dependencies(fh2 main_hook file_redirect_hook gsl_alloc_hook hook_impl)

# 16 KB pages (Android 15+ devices).
foreach(target fh2 fh2_XMediaFacade_default fh2_SpeechFacade_default)
    if(TARGET ${target})
        target_link_options(${target} PRIVATE "-Wl,-z,max-page-size=16384")
    endif()
endforeach()
foreach(target fh2 rexruntime)
    target_link_options(${target} PRIVATE
        "LINKER:--exclude-libs,liblibavcodec.a:liblibavutil.a")
endforeach()

# Every guest function sub_X is a weak, exported alias of __imp__sub_X (DEFINE_REX_FUNC), so that a hook can
# replace it. In a shared library each of the ~316,000 calls between them then went through the PLT (46,423
# slots in libmain.so; @plt was 5.4 % of the game's main thread on the Moto G9 Play). Bound inside libmain.so
# they become direct branches; a hook still wins, it is chosen when libmain.so is linked.
target_link_options(fh2 PRIVATE "-Wl,-Bsymbolic-functions")

# ThinLTO (as nfsmw-android uses) is off for now: the guest code is about twice NFSMW's and the build machine
# has 14 GB of RAM. Turn it on once the plain build works.
option(FH2_ANDROID_LTO "ThinLTO on the game code and the app (Android, Release)" OFF)
if(FH2_ANDROID_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release")
    foreach(target fh2_recomp fh2)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -flto=thin)
        endif()
    endforeach()
    cmake_host_system_information(RESULT FH2_CORES QUERY NUMBER_OF_LOGICAL_CORES)
    target_link_options(fh2 PRIVATE -flto=thin "-Wl,--thinlto-jobs=${FH2_CORES}"
                        "-Wl,--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    target_link_options(fh2 PRIVATE -Wl,--gc-sections -Wl,--icf=safe)
endif()
