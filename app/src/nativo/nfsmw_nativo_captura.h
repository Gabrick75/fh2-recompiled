// nfsmw - captures of the game image for the native renderer tests. With
// nfsmw_captura_cada_s > 0 it saves a PNG every N seconds in capturas/ next
// to the executable, in native and in emulation mode, to compare the two
// without looking at the screen.

#pragma once

#include <functional>

namespace rex::ui {
class Presenter;
}

namespace nfsmw::captura {

// Starts the capture thread if the cvar asks for it. The presenter is requested
// on every capture: it may not exist yet at startup.
void Arrancar(std::function<rex::ui::Presenter*()> obtener_presentador);

// Stops the thread (idempotent).
void Parar();

}  // namespace nfsmw::captura
