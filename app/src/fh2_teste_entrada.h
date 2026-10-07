// fh2 - automated tests on the PC: a virtual pad driven by a button script ("teste de entrada").
//
// With fh2_teste_botoes (for example "20:start,26:a,40-60:rt") the app adds to the SDK input
// system a synthetic device that presses those buttons at those seconds after the input system is
// created, so a test can reach a given screen unattended. It coexists with the keyboard and a real
// pad (all devices of user 0 are merged). Empty (the default) adds nothing.
// Adapted from nfsmw-android (nfsmw_prueba_entrada).

#pragma once

#include <rex/runtime.h>

namespace fh2::teste {

// Wraps config.input_factory to add the script pad if the cvar is not empty. Call it in
// OnPreSetup, once ReXApp has installed the default factory.
void EnvolverEntrada(rex::RuntimeConfig& config);

}  // namespace fh2::teste
