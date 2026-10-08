// nfsmw - who waits for whom during stutters.
//
// In the 216 frames over 45 ms of one console run almost nobody is working: the main thread waits for the
// preparer's handoff, the PM4 ring runs out of work or waits for the GPU, and the preparer sleeps inside
// sub_826E8EE8 (COM-style virtual calls, unidentified). Each wait adds its time here, and the "[tiron]" line
// (nfsmw_nativo_destinos.cpp) writes how much each one took in that frame.
// It only costs two clock reads per wait, and those waits sleep anyway.

#pragma once

#include <atomic>
#include <cstdint>

namespace nfsmw::esperas {

enum Tipo : uint32_t {
  kRelevoEjecutor,    // Main XThread waiting for the preparer to hand over the frame (sub_8262D988)
  kRelevoPreparador,  // the preparer waiting for the executor to finish the previous one (sub_8262D988)
  kSitioAnillo,       // the D3D waiting for the ring to advance (sub_825A5D18)
  kJuegoMedio,        // the game inside sub_826E8EE8
  kAnilloSinTrabajo,  // the ring thread with no packets to read
  kAnilloRegMem,      // the ring thread in a WAIT_REG_MEM (waiting for the game to write)
  // The game's side in the stutter frame ("[tiron] juego" line).
  kEjecutorSinOrdenes,   // Main XThread with the list open and no new commands (sub_823C83F8)
  kPreparadorLista,      // the preparer filling the command list (all of sub_82445660: culling and eView)
  kPreparadorFuera,      // the preparer between two fills: its simulation plus its handoff wait
  kPreparadorEscenario,  // TreeCull (sub_824C2F48) with its DrawAScenery calls, inside the fill
  kNumTipos
};

inline std::atomic<uint64_t> g_ns[kNumTipos];
inline std::atomic<uint64_t> g_veces[kNumTipos];
inline std::atomic<uint32_t> g_vtabla_medio{0};    // vtable of the object of the last call to sub_826E8EE8
inline std::atomic<uint32_t> g_llamante_medio{0};  // and its return address

// What the ring re-checks of the textures (XXH3 of the guest bytes) and how many checks are postponed by
// the per-frame budget (nfsmw_nativo_huellas_kb_fotograma).
inline std::atomic<uint64_t> g_bytes_huella{0};
inline std::atomic<uint64_t> g_huellas_aplazadas{0};

// What the ring does in the stutter frame. The race stutters are the ring being full (the game waits
// 18-54 ms for space) and the texture checks stay under 8 MB: this shows where the ring's time goes. Only the
// ring thread writes all of this.
inline std::atomic<uint64_t> g_dibujos{0};             // draws read from the ring
inline std::atomic<uint64_t> g_ns_anillo_trabajando{0};  // leyendo y ejecutando paquetes
inline std::atomic<uint64_t> g_ns_texturas{0};         // check, detile and prepare the textures in use
inline std::atomic<uint64_t> g_texturas_subidas{0};    // textures that changed and are uploaded again
inline std::atomic<uint64_t> g_bytes_subidos{0};
inline std::atomic<uint64_t> g_texturas_creadas{0};
// Of that texture time, what goes into the two fingerprints (XXH3 of the guest memory and of the already
// prepared data).
inline std::atomic<uint64_t> g_ns_huella_cruda{0};
inline std::atomic<uint64_t> g_ns_huella_datos{0};
// How long the ring thread waits for the vertex copy thread (EsperarSubidas in nfsmw_nativo_dibujos.cpp:
// before each submit and before returning the read pointer to the game), and how many times it really waits.
// Previously this was only reported when the game closed. Only the ring thread writes it.
inline std::atomic<uint64_t> g_ns_esperando_copias{0};
inline std::atomic<uint64_t> g_esperas_copias{0};
// What the ring copies itself during those waits, from what the thread had not taken yet, and how many
// copies (nfsmw_nativo_subidas_ayuda). g_ns_esperando_copias is therefore only the wait. Ring thread only.
inline std::atomic<uint64_t> g_ns_ayudando_copias{0};
inline std::atomic<uint64_t> g_copias_ayudadas{0};
// What the ring spends creating textures (image, pool memory and view; "texturas X ms" starts after they
// are created and did not include it), how long it waits for the binding thread and how many textures that
// thread bound (nfsmw_nativo_texturas_enlace_hilo in nfsmw_nativo_dibujos.cpp). Only the ring thread writes it.
inline std::atomic<uint64_t> g_ns_crear_texturas{0};
inline std::atomic<uint64_t> g_ns_esperando_enlaces{0};
inline std::atomic<uint64_t> g_texturas_enlazadas_hilo{0};
// New textures with the fingerprint, the detiling and the byte order done on the fingerprint thread
// (nfsmw_nativo_texturas_huella_hilo in nfsmw_nativo_dibujos.cpp): what the ring spends copying guest memory
// (snapshots; counted inside "texturas"), how many the thread prepares and in how long, how many the ring
// keeps for itself at submit time and in how long, and how long the ring waits for the thread. Only the ring
// thread writes it (when planning and when collecting).
inline std::atomic<uint64_t> g_ns_huella_instantanea{0};
inline std::atomic<uint64_t> g_texturas_huella_hilo{0};
inline std::atomic<uint64_t> g_ns_huella_hilo{0};
inline std::atomic<uint64_t> g_texturas_huella_anillo{0};
inline std::atomic<uint64_t> g_ns_huella_anillo{0};
inline std::atomic<uint64_t> g_ns_esperando_huellas{0};

inline void Sumar(Tipo tipo, uint64_t ns) {
  g_ns[tipo].fetch_add(ns, std::memory_order_relaxed);
  g_veces[tipo].fetch_add(1, std::memory_order_relaxed);
}

}  // namespace nfsmw::esperas
