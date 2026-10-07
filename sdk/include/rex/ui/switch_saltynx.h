/*
 * FPS and resolution for the console overlays (through SaltyNX shared memory) and reading of the
 * Reverse-NX state. Details and block format: src/ui/switch_saltynx.cpp.
 */
#ifndef REX_UI_SWITCH_SALTYNX_H_
#define REX_UI_SWITCH_SALTYNX_H_

#include <cstdint>

#include "rex/platform.h"

namespace rex::ui::switch_saltynx {

#if REX_PLATFORM_SWITCH

// Connects to SaltySD, reserves the block and locates the Reverse-NX one. Without SaltyNX it does
// nothing. It can be called as many times as needed. Until there is a valid block it keeps trying,
// and the cheap part (searching for the marker again in the already mapped page) goes before the IPC.
void Iniciar();

// Once per second, with the last second's FPS, their average, the resolution being presented and the
// number of frames since startup. It publishes both things (FPS and resolution) and, if the block is
// missing, hooks it up again. Calling it from the very start is what makes the overlay show data on
// the Most Wanted logo and work when opened later.
void Actualizar(double fps_segundo, double fps_media, uint32_t ancho, uint32_t alto, uint64_t fotogramas);

// On every presentation: the overlay clears the "alive" and resolution markers and only waits
// 100 ms for the game to set them again. It also fills in the frame time, from which the FPS
// average is taken.
void Latir(uint32_t ancho, uint32_t alto);

// Turns all of this on or off (the game passes it from its cvar, before the profile thread starts).
void Habilitar(bool habilitado);

// The mode to obey: Reverse-NX's if it is active, otherwise the real one passed in.
bool ModoBase(bool real);

// What the Reverse-NX block says, as is, for the profile report. Mind the semantics: "por_defecto"
// is its "Controlled by system", and when it is true its "Mode" does not decide anything, it only
// reflects the console's real state. Without a block (no SaltyNX or no plugin), hay = false and the
// rest means nothing.
struct EstadoReverseNx {
  bool hay;
  bool en_base;
  bool por_defecto;
  bool plugin_activo;
};
EstadoReverseNx EstadoReverse();

#else

inline void Iniciar() {}
inline void Actualizar(double, double, uint32_t, uint32_t, uint64_t) {}
inline void Latir(uint32_t, uint32_t) {}
inline void Habilitar(bool) {}
inline bool ModoBase(bool real) { return real; }
struct EstadoReverseNx {
  bool hay;
  bool en_base;
  bool por_defecto;
  bool plugin_activo;
};
inline EstadoReverseNx EstadoReverse() { return {false, false, false, false}; }

#endif  // REX_PLATFORM_SWITCH

}  // namespace rex::ui::switch_saltynx

#endif  // REX_UI_SWITCH_SALTYNX_H_
