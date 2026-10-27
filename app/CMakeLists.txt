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
    # Tests on the PC: virtual pad driven by a button script (cvar fh2_teste_botoes).
    src/fh2_teste_entrada.cpp
    # PNG captures of the game image every N seconds (cvar fh2_captura_cada_s).
    src/fh2_captura.cpp
    # Guards on game functions (null checks the console never needed).
    src/fh2_correcoes.cpp
    src/fh2_desempenho.cpp
    # One simulation step per frame shown: the vblank's step event held for the render thread (cvar fh2_ritmo_passos).
    src/fh2_passos.cpp
    src/fh2_perfil.cpp
    src/fh2_censo.cpp
    # The game's Sleep and SwitchToThread per call site (cvar fh2_medir_esperas).
    src/fh2_esperas.cpp
    # The game's two memcpy on the host's memcpy (cvar fh2_memcpy_nativo).
    src/fh2_copias.cpp
    # Grass, objects, reflections, shadows and crowd from 0.25x to 4x (cvars fh2_mundo_*): TrackSettings.xml rewritten.
    src/fh2_mundo.cpp
    # Native renderer (cvar fh2_renderizador = nativo): NFSMW Android Evolved's, built for FH2 with FH2_RECOMP
    # (src/nativo/README.md, docs/renderizador_nativo.md), FH2's hooks and the answers it asks of NFSMW's modules.
    src/nativo/nfsmw_nativo_sistema.cpp
    src/nativo/nfsmw_nativo_destinos.cpp
    src/nativo/nfsmw_nativo_dibujos.cpp
    src/nativo/nfsmw_nativo_shaders.cpp
    src/nativo/nfsmw_nativo_ganchos.cpp
    src/nativo/nfsmw_nativo_texturas_pool.cpp
    src/nativo/nfsmw_nativo_captura.cpp
    src/nativo/nfsmw_shader_library.cpp
    src/nativo/nfsmw_espera_anillo.cpp
    src/nativo/nfsmw_ajustes_graficos.cpp
    src/fh2_nativo_ganchos.cpp
    src/fh2_nativo_enlaces.cpp
)

if(ANDROID)
    # SDLActivity loads SDL_main from libmain.so (android/app/src/main/cpp/CMakeLists.txt builds this).
    add_library(fh2 SHARED ${FH2_SOURCES})
    set_target_properties(fh2 PROPERTIES OUTPUT_NAME main)
elseif(WIN32)
    add_executable(fh2 WIN32 ${FH2_SOURCES})
else()
    add_executable(fh2 ${FH2_SOURCES})
endif()

rexglue_setup_target(fh2 GPU_PLUGINS xenos)

# PC build with the SDK as a subdirectory: rex_app.cpp includes imgui.h and rexruntime links imgui
# privately, so its include path does not reach the executable (same fix as nfsmw-android). The same
# goes for renderdoc_app.h, which rex/ui/vulkan/instance.h includes outside the Switch.
if(TARGET imgui)
    target_include_directories(fh2 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(TARGET renderdoc)
    target_include_directories(fh2 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Native renderer: FH2's branches, its folder, the SDK's xxHash (shader library fingerprints) and the SDK
# presenter's precompiled SPIR-V (it draws the game image into the presenter output).
target_compile_definitions(fh2 PRIVATE FH2_RECOMP=1)
target_include_directories(fh2 PRIVATE src/nativo)
if(REXSDK_DIR)
    target_include_directories(fh2 PRIVATE "${REXSDK_DIR}/thirdparty/xxHash" "${REXSDK_DIR}/src/ui/shaders")
endif()
