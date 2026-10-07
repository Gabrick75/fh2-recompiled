/*
 * Ask Horizon OC (sys-clk and its forks) to apply the configured docked column while Reverse-NX
 * says docked. Details and why it is needed: src/ui/switch_sysclk.cpp.
 */
#ifndef REX_UI_SWITCH_SYSCLK_H_
#define REX_UI_SWITCH_SYSCLK_H_

#include <cstdint>

#include "rex/platform.h"

namespace rex::ui::switch_sysclk {

#if REX_PLATFORM_SWITCH

// Once per second. "efectivo" is the mode the game obeys (Reverse-NX's if set) and "real" the
// hardware's: something is done whenever they differ, in either direction, which is exactly what
// the clock sysmodule cannot see (it looks at the hardware).
//   effective docked   + real handheld -> clocks go up to the configured docked column.
//   effective handheld + real docked   -> they go down to the handheld column, and if that column
//                                         is empty to the stock clocks (GPU 307.2 MHz and memory
//                                         1331.2), which is what "Fake Handheld" is expected to do.
//                                         Note: the sysmodule's overlay will keep saying "Docked"
//                                         because that is its profile; what changes are the actual MHz.
void SeguirModo(bool sobremesa_efectivo, bool sobremesa_real);

// Turns all of this on or off (the game passes it from its cvar). Turning it off releases the clocks.
void Habilitar(bool habilitado);

#else

inline void SeguirModo(bool, bool) {}
inline void Habilitar(bool) {}

#endif  // REX_PLATFORM_SWITCH

}  // namespace rex::ui::switch_sysclk

#endif  // REX_UI_SWITCH_SYSCLK_H_
