// Prototype on ShiftGlue: the launcher's touch controls as a pad. FH2 Recompiled's SDK takes them in its SDL input
// driver (rex_sdl_set_touch_gamepad_state); ShiftGlue has no such entry, so this driver reads the same state.

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include <atomic>
#include <cstring>

namespace {

namespace in = rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

std::atomic<uint64_t> g_sticks{0};  // lx, ly, rx, ry
std::atomic<uint32_t> g_rest{0};    // buttons | lt << 16 | rt << 24
std::atomic<uint32_t> g_packet{1};

constexpr in::DeviceId kTouchPad = in::DeviceId(0x4648325443484150ull);  // "FH2TCHAP"

class TouchPad final : public in::InputDriver {
 public:
  TouchPad() : InputDriver(nullptr, 0) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kTouchPad;
    info.name = "Touch controls";
    info.synthetic = true;
    out.push_back(info);
  }
  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kTouchPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_state) {
      const uint64_t s = g_sticks.load(std::memory_order_acquire);
      const uint32_t r = g_rest.load(std::memory_order_acquire);
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = g_packet.load(std::memory_order_relaxed);
      out_state->gamepad.buttons = uint16_t(r & 0xFFFF);
      out_state->gamepad.left_trigger = uint8_t(r >> 16);
      out_state->gamepad.right_trigger = uint8_t(r >> 24);
      out_state->gamepad.thumb_lx = int16_t(s & 0xFFFF);
      out_state->gamepad.thumb_ly = int16_t((s >> 16) & 0xFFFF);
      out_state->gamepad.thumb_rx = int16_t((s >> 32) & 0xFFFF);
      out_state->gamepad.thumb_ry = int16_t((s >> 48) & 0xFFFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t, in::X_INPUT_CAPABILITIES* out_caps) override {
    if (id != kTouchPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION*) override {
    return id == kTouchPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t, in::X_INPUT_KEYSTROKE*) override {
    return id == kTouchPad ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

}  // namespace

extern "C" void rex_sdl_set_touch_gamepad_state(uint16_t buttons, int16_t left_x, int16_t left_y, int16_t right_x,
                                                int16_t right_y, uint8_t left_trigger, uint8_t right_trigger) {
  g_sticks.store(uint64_t(uint16_t(left_x)) | uint64_t(uint16_t(left_y)) << 16 | uint64_t(uint16_t(right_x)) << 32 |
                     uint64_t(uint16_t(right_y)) << 48,
                 std::memory_order_release);
  g_rest.store(uint32_t(buttons) | uint32_t(left_trigger) << 16 | uint32_t(right_trigger) << 24,
               std::memory_order_release);
  g_packet.fetch_add(1, std::memory_order_relaxed);
}

// android_rendimiento.cpp reads FH2 Recompiled's native renderer counters; this prototype has no native renderer.
std::atomic<float> g_fh2_nativo_fps{-1.0f};
std::atomic<float> g_fh2_nativo_ms{-1.0f};

namespace fh2::toque {

void Envolver(rex::RuntimeConfig& config) {
  if (!config.input_factory) return;
  auto base = config.input_factory;
  config.input_factory = [base](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      static_cast<in::InputSystem*>(system.get())->AddDriver(std::make_unique<TouchPad>());
      REXLOG_INFO("[toque] touch controls pad added");
    }
    return system;
  };
}

}  // namespace fh2::toque
