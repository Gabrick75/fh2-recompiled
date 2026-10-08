// nfsmw - road reflection on demand. The decision is made in nfsmw_recortes_carrera.cpp.
//
// The reflection (views 4 and 5, a 640x360 or 512x288 pass) was drawn in every race frame: ~210 draws on
// average. In one run 42 % of those draws were for copies nobody read (stretches with no water in view).
// The native renderer records here when it is resolved and when it is read, and the game only draws it
// if it was read recently.
//
// And only if the water is really visible. At the Heritage & Omega start the reflection was read in
// 100 % of frames (175-338 draws per copy) without the sea showing on screen (PC captures at 100, 110
// and 120 s): the water is submitted because it falls inside the frustum, but the terrain covers it
// completely. The ring measures every draw that samples the reflection with an occlusion query and
// records whether it left any sample.
#pragma once

#include <cstdint>

namespace nfsmw::reflejo_demanda {

// Guest physical address where the game resolves the reflection. It is the same in every measured run
// (PC and console); nfsmw_recortes_carrera.cpp checks it in every run before skipping any frame.
constexpr uint32_t kDireccion = 0x07C5A000;

// From the native renderer (ring thread).
void AnotarLectura();  // a draw samples the reflection texture
void AnotarCopia();    // the reflection is resolved to kDireccion
void AnotarSwap();     // end of a renderer frame

// nfsmw_reflejo_visibilidad. All from the ring thread.
// Whether to measure: the setting is on and the guard has not tripped. Asked once per frame.
bool MedirVisibilidad();
// Whether the witnesses are already verified (visibility decides). Until then the witness is measured
// more often.
bool VisibilidadComprobada();
// A draw that samples the reflection left samples on screen. medido = false: it could not be measured
// (no room for the query, a game query already open, the deferred sky...) and is treated as visible,
// the safe choice.
void AnotarVisible(bool medido);
// A draw that samples the reflection left no samples: the terrain or something else covered it entirely.
void AnotarOculto();
// The witness: the final race composite, which covers the whole screen and must leave samples. If it
// ever gives 0, the queries cannot be trusted and the decision falls back to reads.
void AnotarTestigo(bool con_muestras);

}  // namespace nfsmw::reflejo_demanda
