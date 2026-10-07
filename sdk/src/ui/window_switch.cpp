/**
 * @file        ui/window_switch.cpp
 * @brief       Window for Nintendo Switch: the fullscreen default NWindow
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/window_switch.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface_switch.h>
#include <rex/ui/switch_imgui_input.h>
#include <rex/ui/windowed_app_context_switch.h>

#include <switch.h>

#include "rex/ui/switch_saltynx.h"

REXCVAR_DECLARE(bool, input_xbox_layout);  // defined in switch_input_driver.cpp

namespace rex::ui {

bool ViNWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  width_out = owner_.GetActualPhysicalWidth();
  height_out = owner_.GetActualPhysicalHeight();
  return width_out && height_out;
}

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<WindowSwitch>(app_context, title, desired_logical_width,
                                        desired_logical_height);
}

WindowSwitch::WindowSwitch(WindowedAppContext& app_context, const std::string_view title,
                           uint32_t desired_logical_width, uint32_t desired_logical_height)
    : Window(app_context, title, desired_logical_width, desired_logical_height) {}

WindowSwitch::~WindowSwitch() {
  EnterDestructor();
  if (nwindow_) {
    switch_app_context().UnregisterWindow(this);
    nwindow_ = nullptr;
  }
}

SwitchWindowedAppContext& WindowSwitch::switch_app_context() const {
  return static_cast<SwitchWindowedAppContext&>(app_context());
}

/*
 * The effective mode rules, not the real display.
 *
 * Taking appletGetDefaultDisplayResolution and returning made the Reverse-NX line below dead code:
 * with the console in the dock and Reverse-NX on "Fake Handheld", the window stayed at 1920x1080
 * while the scene, which does obey Reverse-NX, went to 1280x720. Result: 720p drawn in the corner of
 * a 1080p surface, with the rest uninitialized. The ReverseNX-RT overlay itself said so: "Default
 * Display Resolution was not checked!".
 *
 * The applet's resolution is only used if it agrees with the effective mode; otherwise the mode rules.
 */
void WindowSwitch::QueryDisplayResolution(uint32_t& width_out, uint32_t& height_out) {
  const bool docked =
      rex::ui::switch_saltynx::ModoBase(appletGetOperationMode() == AppletOperationMode_Console);
  s32 width = 0;
  s32 height = 0;
  if (R_SUCCEEDED(appletGetDefaultDisplayResolution(&width, &height)) && width > 0 && height > 0 &&
      (width >= 1920) == docked) {
    width_out = uint32_t(width);
    height_out = uint32_t(height);
    return;
  }
  width_out = docked ? 1920 : 1280;
  height_out = docked ? 1080 : 720;
}

bool WindowSwitch::OpenImpl() {
  // There is one display and the application owns all of it: the desired size
  // passed to Window::Create is ignored.
  nwindow_ = nwindowGetDefault();
  if (!nwindow_ || !nwindowIsValid(nwindow_)) {
    REXLOG_ERROR("WindowSwitch: no default NWindow");
    nwindow_ = nullptr;
    return false;
  }
  switch_app_context().RegisterWindow(this);

  uint32_t width, height;
  QueryDisplayResolution(width, height);
  REXLOG_INFO("WindowSwitch: {} mode, display resolution {}x{}",
              rex::ui::switch_saltynx::ModoBase(appletGetOperationMode() == AppletOperationMode_Console) ? "docked" : "handheld",
              width, height);

  // Actualize state for the common Window code. Listener dispatch is handled
  // by Window::Open after OpenImpl returns; these only record initial state.
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return true;
  }
  has_focus_ = appletGetFocusState() == AppletFocusState_InFocus;
  if (has_focus_) {
    OnFocusUpdate(true, destruction_receiver);
  }
  return true;
}

void WindowSwitch::RequestCloseImpl() {
  PerformClose();
}

void WindowSwitch::PerformClose() {
  WindowDestructionReceiver destruction_receiver(this);
  OnBeforeClose(destruction_receiver);
  if (destruction_receiver.IsWindowDestroyed()) {
    return;
  }
  if (nwindow_) {
    switch_app_context().UnregisterWindow(this);
    nwindow_ = nullptr;
  }
  OnAfterClose();
}

std::unique_ptr<Surface> WindowSwitch::CreateSurfaceImpl(Surface::TypeFlags allowed_types) {
  if (!nwindow_ || !(allowed_types & Surface::kTypeFlag_ViNWindow)) {
    return nullptr;
  }
  return std::make_unique<ViNWindowSurface>(nwindow_, *this);
}

void WindowSwitch::RequestPaintImpl() {
  // Coalesce: at most one pending paint at a time.
  if (paint_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  switch_app_context().WakeUILoop();
}

void WindowSwitch::HandlePendingPaint() {
  if (!paint_pending_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  // Without focus (HOME menu, sleep, an overlay applet) the display stack may
  // stop taking frames, and a present could block the UI thread.
  if (has_focus_) {
    OnPaint();
  }
}

void WindowSwitch::HandleDisplayModeChange() {
  uint32_t width, height;
  QueryDisplayResolution(width, height);
  if (width == GetActualPhysicalWidth() && height == GetActualPhysicalHeight()) {
    return;
  }
  REXLOG_INFO("WindowSwitch: display resolution {}x{} -> {}x{}", GetActualPhysicalWidth(),
              GetActualPhysicalHeight(), width, height);
  WindowDestructionReceiver destruction_receiver(this);
  OnActualSizeUpdate(width, height, destruction_receiver);
}

void WindowSwitch::HandleFocusChange(bool has_focus) {
  if (has_focus == has_focus_) {
    return;
  }
  has_focus_ = has_focus;
  WindowDestructionReceiver destruction_receiver(this);
  OnFocusUpdate(has_focus, destruction_receiver);
  if (has_focus && !destruction_receiver.IsWindowDestroyed()) {
    // Frames requested while out of focus were dropped.
    OnPaint(true);
  }
}

void WindowSwitch::HandleCloseRequest() {
  WindowDestructionReceiver destruction_receiver(this);
  if (SendCloseRequestToListeners(destruction_receiver) &&
      !destruction_receiver.IsWindowDestroyed()) {
    PerformClose();
  }
}

namespace nx {

// switch_imgui_input.h repeats these bits so it does not include switch.h.
static_assert(kBotonA == HidNpadButton_A && kBotonB == HidNpadButton_B &&
                  kBotonX == HidNpadButton_X && kBotonY == HidNpadButton_Y &&
                  kBotonStickL == HidNpadButton_StickL && kBotonStickR == HidNpadButton_StickR &&
                  kBotonL == HidNpadButton_L && kBotonR == HidNpadButton_R &&
                  kBotonZL == HidNpadButton_ZL && kBotonZR == HidNpadButton_ZR &&
                  kBotonPlus == HidNpadButton_Plus && kBotonMinus == HidNpadButton_Minus &&
                  kBotonIzquierda == HidNpadButton_Left && kBotonArriba == HidNpadButton_Up &&
                  kBotonDerecha == HidNpadButton_Right && kBotonAbajo == HidNpadButton_Down,
              "switch_imgui_input.h: los bits no coinciden con HidNpadButton de libnx");

void LeerEntradaUi(EntradaUi& salida, float toque_a_logico_x, float toque_a_logico_y) {
  // Only called by ImGuiDrawer::Draw, on the UI thread, so the state can be
  // static. The PadState is our own: the game's input driver reads its own from
  // the guest threads, and HID shared memory can be read from several threads at
  // once.
  static bool preparado = false;
  static PadState mando{};
  if (!preparado) {
    // Reference-counted in libnx: the menu does not depend on the input driver
    // having started HID first.
    hidInitialize();
    hidInitializeTouchScreen();
    padInitializeAny(&mando);
    preparado = true;
  }

  salida = EntradaUi{};
  salida.por_posicion = REXCVAR_GET(input_xbox_layout);
  padUpdate(&mando);
  salida.mando_conectado = padIsConnected(&mando);
  if (salida.mando_conectado) {
    salida.botones = padGetButtons(&mando);
    const HidAnalogStickState izquierdo = padGetStickPos(&mando, 0);
    const HidAnalogStickState derecho = padGetStickPos(&mando, 1);
    salida.stick_izq_x = izquierdo.x;
    salida.stick_izq_y = izquierdo.y;
    salida.stick_der_x = derecho.x;
    salida.stick_der_y = derecho.y;
  }

  HidTouchScreenState toque{};
  if (hidGetTouchScreenStates(&toque, 1) != 0 && toque.count > 0) {
    salida.tocando = true;
    salida.toque_x = float(toque.touches[0].x) * toque_a_logico_x;
    salida.toque_y = float(toque.touches[0].y) * toque_a_logico_y;
  }
}

}  // namespace nx

}  // namespace rex::ui
