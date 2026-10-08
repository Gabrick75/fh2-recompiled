// nfsmw - graphics settings in the menu (F4) that work with the native renderer.
//
// The FPS and resolution settings that do nothing are removed from the menu and the ones that work are kept.
// Measured on PC:
//   - NFSMW draws at the size of the video mode that VdQueryVideoMode returns, but it only has two:
//     1280x720 and, below that, 1024x576 (tried 854x480, 960x540, 1024x576 and 1152x648). Above
//     1280x720 it stays at 1280x720.
//   - Its pace is set by the vblank, which follows video_mode_refresh_rate: at 30 Hz it gives exactly
//     30 FPS and the game runs at normal speed (the race timer advances in real time).
//   - resolution_scale, draw_resolution_scale_x/y, vsync, anisotropic_override and swap_post_effect are
//     only read by the emulated path: with the native renderer they do nothing.
// With the gamepad, the menu makes it awkward to change free text (resolution), an integer one step at a
// time (video_mode_width/height) or a decimal (video_mode_refresh_rate). That is why the ones that work
// are offered as lists (nfsmw_resolucion_interna and nfsmw_limite_fps) and passed to the video mode at
// startup.

#pragma once

#include <cstdint>

namespace nfsmw::ajustes {

// Optional post-processing in the Graficos/Posproceso category, like the one in GoldenEye-Recomp (which only
// exists in D3D12): color grading (temperature, brightness, contrast, tint, saturation, vibrance and gamma),
// vignette and scanlines. Off by default: the output stays bit-identical. Applied by the output pass of the
// native renderer.
struct Posproceso {
  bool activo = false;
  float brillo = 0.0f;
  float contraste = 1.0f;
  float saturacion = 1.0f;
  float vibracion = 0.0f;
  float temperatura = 0.0f;
  float gamma = 1.0f;
  float tinte_r = 1.0f;
  float tinte_g = 1.0f;
  float tinte_b = 1.0f;
  float tinte = 0.0f;
  float vineta = 0.0f;
  float lineas = 0.0f;
};

// Values in effect (the chosen preset or the custom ones). Reads the cvars with the registry lock: call
// only when VersionPosproceso changes.
Posproceso LeerPosproceso();

// Increments every time the menu changes a post-processing cvar (lock-free: it can be checked every frame).
uint64_t VersionPosproceso();

// nfsmw_antialiasing = fxaa (lock-free: it can be checked every frame).
bool AntialiasingFxaa();

// nfsmw_resplandor_cielo: 0 original, 1 natural, 2 suave (lock-free: it can be checked on every draw).
int ResplandorCielo();

// nfsmw_tratamiento_visual: 0 original, 1 suave, 2 apagado (lock-free: it can be checked on every draw).
int TratamientoVisual();

// Registers the change notifications for the live Graphics settings (post-processing, antialiasing, sky glow
// and the color filter), once at startup.
void VigilarAjustesEnVivo();

// Passes nfsmw_resolucion_interna and nfsmw_limite_fps to the video mode (video_mode_width/height,
// resolution and video_mode_refresh_rate), except the ones that come from the command line. Must run
// before the game requests the video mode.
void AplicarAjustesGraficos();

// With the native renderer, removes from the menu the FPS, resolution and graphics settings that do
// nothing, and the video mode settings that the ones above replace.
void OcultarAjustesSinEfecto();

}  // namespace nfsmw::ajustes
