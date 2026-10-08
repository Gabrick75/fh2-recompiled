// What the native renderer of NFSMW Android Evolved asks of Most Wanted's own modules, which FH2 does not have:
// road reflection on demand, the 30 FPS guard, the scenery LOD summary, the D3D composite marker and the AA table.
// Each answer is the one that leaves the renderer's own behaviour (as in NFS Carbon's carbon_nativo_enlaces.cpp).
#include <atomic>
#include <cstdint>
#include <string>

// Game frames, one per Swap; only for the "game ahead" measurement. FH2's Swap is not hooked yet.
std::atomic<uint64_t> g_nfsmw_fotogramas_juego{0};

namespace nfsmw::reflejo_demanda {
void AnotarLectura() {}
void AnotarCopia() {}
void AnotarSwap() {}
bool MedirVisibilidad() { return false; }
bool VisibilidadComprobada() { return false; }
void AnotarVisible(bool) {}
void AnotarOculto() {}
void AnotarTestigo(bool) {}
}  // namespace nfsmw::reflejo_demanda

namespace nfsmw::guardia30 {
void Latir(double) {}
void Informe() {}
bool SinSombras(bool del_usuario) { return del_usuario; }
}  // namespace nfsmw::guardia30

namespace nfsmw::escenario_lod {
std::string Resumen() { return "LOD del escenario: no aplica en FH2"; }
}  // namespace nfsmw::escenario_lod

namespace nfsmw::render_targets {
// Occlusion queries are counted with one sample per pixel: carbon_single_pass selects the game's mode 2 (one
// tile, no MSAA).
uint32_t MuestrasOriginalesModoActual(const uint8_t*) { return 1; }
}  // namespace nfsmw::render_targets

namespace nfsmw::nativo {
void ActivarConsumidorMarcadores(bool) {}
void AnotarComprobacionMarcador(bool, uint32_t, uint32_t, uint32_t, uint32_t) {}
}  // namespace nfsmw::nativo
