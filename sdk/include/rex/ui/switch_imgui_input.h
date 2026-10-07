/**
 * @file        rex/ui/switch_imgui_input.h
 * @brief       Switch gamepad and touch screen for the SDK's ImGui menus
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cfloat>
#include <cstdint>

#include <imgui.h>

/*
 * The SDK menus, with the gamepad and the touch screen
 *
 * On PC the menus (settings, console, achievements) are driven with mouse and keyboard. The Switch
 * has neither and nothing fed input to ImGui: the menu showed up but could not be navigated or
 * touched. This translates the gamepad and the touch panel into ImGui events.
 *
 *  - window_switch.cpp reads HID (LeerEntradaUi). It is the only file that includes switch.h.
 *  - ImGuiDrawer::Draw calls LeerEntradaUi and AplicarEntrada on the UI thread, right before
 *    ImGui::NewFrame. While a menu is open Draw runs continuously, so the gamepad responds at the
 *    UI rate, not the game's.
 *  - ReXApp does not pass the gamepad to the game while a menu has navigation focus or a finger is
 *    on a window (JuegoRecibeMando).
 *  - The L+R+D-pad and L+R+ZL shortcuts (switch_input_driver.cpp) do not reach the menu: those
 *    buttons do not count until they are released.
 *  - L+R and the right stick move the debug overlay (F3) if it is open (debug_overlay.cpp);
 *    meanwhile the game does not see L, R or that stick.
 *
 * Buttons, as in the game's menus (input_xbox_layout, in switch_input_driver.cpp):
 *   A               accept: check, open, choose
 *   B               back: close a dropdown, leave a field
 *   X (top)         activate a text field (without a keyboard it does not type; B leaves)
 *   Y (left)        nothing (see AplicarEntrada)
 *   D-pad           move
 *   left stick      scroll
 * With input_xbox_layout = true they go by position, as in 1.0.0: the bottom one (B)
 * accepts and the right one (A) goes back.
 * Touching the screen is a click.
 */

namespace rex::ui::nx {

// HidNpadButton bits from libnx. Copied to avoid including switch.h, which clashes
// with SDK types; window_switch.cpp checks with static_assert that they match.
inline constexpr uint64_t kBotonA = uint64_t(1) << 0;
inline constexpr uint64_t kBotonB = uint64_t(1) << 1;
inline constexpr uint64_t kBotonX = uint64_t(1) << 2;
inline constexpr uint64_t kBotonY = uint64_t(1) << 3;
inline constexpr uint64_t kBotonStickL = uint64_t(1) << 4;
inline constexpr uint64_t kBotonStickR = uint64_t(1) << 5;
inline constexpr uint64_t kBotonL = uint64_t(1) << 6;
inline constexpr uint64_t kBotonR = uint64_t(1) << 7;
inline constexpr uint64_t kBotonZL = uint64_t(1) << 8;
inline constexpr uint64_t kBotonZR = uint64_t(1) << 9;
inline constexpr uint64_t kBotonPlus = uint64_t(1) << 10;
inline constexpr uint64_t kBotonMinus = uint64_t(1) << 11;
inline constexpr uint64_t kBotonIzquierda = uint64_t(1) << 12;
inline constexpr uint64_t kBotonArriba = uint64_t(1) << 13;
inline constexpr uint64_t kBotonDerecha = uint64_t(1) << 14;
inline constexpr uint64_t kBotonAbajo = uint64_t(1) << 15;

// What was read from HID in one UI frame.
struct EntradaUi {
  bool mando_conectado = false;
  uint64_t botones = 0;
  // As HID reports them: -32767..32767, up and right positive.
  int32_t stick_izq_x = 0;
  int32_t stick_izq_y = 0;
  int32_t stick_der_x = 0;
  int32_t stick_der_y = 0;
  bool tocando = false;
  // Touched point, already in ImGui logical coordinates.
  float toque_x = 0.0f;
  float toque_y = 0.0f;
  // input_xbox_layout: face buttons by position instead of by letter.
  bool por_posicion = false;
};

// What must be remembered from one frame to the next.
struct EstadoEntradaUi {
  bool tocaba = false;
  // Buttons of an L+R+... shortcut that are still held.
  uint64_t de_atajo = 0;
};

// In window_switch.cpp; UI thread only. The factors convert from the touch panel
// (1280x720) to ImGui logical coordinates.
void LeerEntradaUi(EntradaUi& salida, float toque_a_logico_x, float toque_a_logico_y);

inline void ConfigurarNavegacion(ImGuiIO& io) {
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // A accepts, as in the game; AplicarEntrada sets it every frame from input_xbox_layout.
  io.ConfigNavSwapGamepadButtons = true;
}

namespace detalle {

// One direction of a stick axis, 0..1, with a dead zone.
inline float Sentido(int32_t valor, bool positivo) {
  constexpr float kMaximo = 32767.0f;
  constexpr float kZonaMuerta = 0.25f;
  float v = float(valor) / kMaximo;
  if (!positivo) {
    v = -v;
  }
  if (v <= kZonaMuerta) {
    return 0.0f;
  }
  v = (v - kZonaMuerta) / (1.0f - kZonaMuerta);
  return v > 1.0f ? 1.0f : v;
}

inline void Stick(ImGuiIO& io, ImGuiKey arriba, ImGuiKey abajo, ImGuiKey izquierda,
                  ImGuiKey derecha, int32_t x, int32_t y) {
  const float a = Sentido(y, true);
  const float b = Sentido(y, false);
  const float i = Sentido(x, false);
  const float d = Sentido(x, true);
  io.AddKeyAnalogEvent(arriba, a > 0.0f, a);
  io.AddKeyAnalogEvent(abajo, b > 0.0f, b);
  io.AddKeyAnalogEvent(izquierda, i > 0.0f, i);
  io.AddKeyAnalogEvent(derecha, d > 0.0f, d);
}

}  // namespace detalle

// Translates a HID reading into ImGui events. ImGui discards events that repeat
// the previous state, so it can be called every frame.
inline void AplicarEntrada(ImGuiIO& io, const EntradaUi& e, EstadoEntradaUi& estado) {
  if (e.mando_conectado) {
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  } else {
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  }
  // ImGui reads the buttons by position: it accepts with the bottom one (B on the Switch) and goes
  // back with the right one (A). Swapped, A accepts and B goes back, as in the game with the mapping
  // by letter.
  io.ConfigNavSwapGamepadButtons = !e.por_posicion;

  const uint64_t leidos = e.mando_conectado ? e.botones : 0;
  // With L and R held, the D-pad and ZL are input driver shortcuts: they open and close menus or
  // start the A/B tests. The driver only sees them when the game polls the gamepad, and the menu
  // changes later, on the UI thread; meanwhile the held D-pad would repeat here and move the focus.
  // They stay excluded even if L or R is released before them.
  constexpr uint64_t kModificador = kBotonL | kBotonR;
  constexpr uint64_t kDeAtajo =
      kBotonArriba | kBotonAbajo | kBotonIzquierda | kBotonDerecha | kBotonZL;
  if ((leidos & kModificador) == kModificador) {
    estado.de_atajo |= leidos & kDeAtajo;
  }
  estado.de_atajo &= leidos;
  const uint64_t b = leidos & ~estado.de_atajo;

  // Y is not passed to ImGui. The left face button is ImGui's menu button: held, it opens the window
  // selector, which on the console moved the F3 overlay with L/R and the stick and took focus away
  // from Settings.
  struct Boton {
    uint64_t bit;
    ImGuiKey tecla;
  };
  static constexpr Boton kBotones[] = {
      {kBotonB, ImGuiKey_GamepadFaceDown},         {kBotonA, ImGuiKey_GamepadFaceRight},
      {kBotonX, ImGuiKey_GamepadFaceUp},           {kBotonArriba, ImGuiKey_GamepadDpadUp},
      {kBotonAbajo, ImGuiKey_GamepadDpadDown},     {kBotonIzquierda, ImGuiKey_GamepadDpadLeft},
      {kBotonDerecha, ImGuiKey_GamepadDpadRight},  {kBotonL, ImGuiKey_GamepadL1},
      {kBotonR, ImGuiKey_GamepadR1},               {kBotonStickL, ImGuiKey_GamepadL3},
      {kBotonStickR, ImGuiKey_GamepadR3},          {kBotonPlus, ImGuiKey_GamepadStart},
      {kBotonMinus, ImGuiKey_GamepadBack},
  };
  for (const Boton& boton : kBotones) {
    io.AddKeyEvent(boton.tecla, (b & boton.bit) != 0);
  }
  // ZL and ZR are digital on Switch.
  const bool zl = (b & kBotonZL) != 0;
  const bool zr = (b & kBotonZR) != 0;
  io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, zl, zl ? 1.0f : 0.0f);
  io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, zr, zr ? 1.0f : 0.0f);
  detalle::Stick(io, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown,
                 ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                 e.mando_conectado ? e.stick_izq_x : 0, e.mando_conectado ? e.stick_izq_y : 0);
  detalle::Stick(io, ImGuiKey_GamepadRStickUp, ImGuiKey_GamepadRStickDown,
                 ImGuiKey_GamepadRStickLeft, ImGuiKey_GamepadRStickRight,
                 e.mando_conectado ? e.stick_der_x : 0, e.mando_conectado ? e.stick_der_y : 0);

  // The touch panel as a mouse: touching presses and lifting releases at the same point. ImGui
  // defers to another frame any movement that arrives after the release, so the click lands where
  // the finger was before the pointer is removed, and nothing stays highlighted without a finger.
  if (e.tocando) {
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(e.toque_x, e.toque_y);
    if (!estado.tocaba) {
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    }
  } else if (estado.tocaba) {
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
  estado.tocaba = e.tocando;
}

// The gamepad belongs to the menu, not the game, while a window has navigation focus or a
// finger is on it. Windows that only display data (the debug overlay) carry NoNavInputs and
// do not take it.
inline bool JuegoRecibeMando(const ImGuiIO& io) {
  return !io.WantCaptureMouse && !io.NavActive;
}

}  // namespace rex::ui::nx
