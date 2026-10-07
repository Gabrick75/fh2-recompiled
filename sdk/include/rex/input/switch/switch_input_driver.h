/**
 * @file        rex/input/switch/switch_input_driver.h
 * @brief       Input driver for Nintendo Switch pads through libnx HID
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/input/input_driver.h>

namespace rex::input::nx {

// Polls the pads directly from HID shared memory on the calling guest thread.
// Unlike the SDL driver it needs no window and no UI-thread event pump.
//
// Slot 0 merges the handheld controllers with player 1, so the console works
// both undocked and with a Pro Controller or detached Joy-Cons. Slots 1-3 are
// players 2-4.
class SwitchInputDriver final : public InputDriver {
 public:
  static constexpr size_t kSlotCount = 4;

  explicit SwitchInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SwitchInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

  /*
   * The driver is created without a window; it arrives later through InputSystem::AttachWindow.
   * It is kept to send the menu shortcuts to the UI thread.
   */
  void OnWindowAvailable(rex::ui::Window* window) override {
    ventana_ui_.store(window, std::memory_order_release);
  }

 private:
  // Holds libnx types, which stay out of this header: switch.h declares global
  // Thread, Mutex and Event types.
  struct Slot;

  // All of these require mutex_.
  Slot* FindSlot(DeviceId id);
  void Poll(size_t index);
  /* SDK menus on the gamepad: L+R+D-pad. See the .cpp. */
  void DispatchMenuShortcuts(Slot& slot, uint64_t held);

  std::mutex mutex_;
  std::array<std::unique_ptr<Slot>, kSlotCount> slots_;
  uint32_t connection_counter_ = 0;
  bool initialized_ = false;
  std::atomic<rex::ui::Window*> ventana_ui_{nullptr};
};

}  // namespace rex::input::nx
