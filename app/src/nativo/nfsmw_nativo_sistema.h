// nfsmw - native renderer, step C1: the app's own graphics system.
//
// With nfsmw_renderizador = "nativo", OnPreSetup puts this system in
// config.graphics and the Xenos emulation plugin is not loaded. Details are
// in docs/native-renderer.md.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace nfsmw::nativo {

// true if the nfsmw_renderizador cvar asks for the native path.
bool Activo();

// Ring thread progress, for the game's D3D waits (nfsmw_espera_anillo.cpp): it increases every time
// the ring thread returns the read pointer or delivers an interrupt.
uint32_t ProgresoAnillo();

// Waits at most 'limite' for ProgresoAnillo() to differ from 'visto'. true if it advanced.
bool EsperarProgresoAnillo(uint32_t visto, std::chrono::microseconds limite);

// Called by the ring thread after writing the read pointer to game memory, and when delivering
// an interrupt. Cheap if nobody is waiting.
void AvisarProgresoAnillo();

// Swaps presented by the native renderer since startup (0 with Xenos emulation). The watchdog in
// nfsmw_app.h counts them as progress: with blocking waits the game threads sleep in the same place
// for most of the frame.
uint64_t SwapsNativos();

// The native graphics system: the SDK presenter, a reserved MMIO range, the vblank
// thread and the command ring sink.
std::unique_ptr<rex::system::IGraphicsSystem> CrearSistemaGrafico();

}  // namespace nfsmw::nativo
