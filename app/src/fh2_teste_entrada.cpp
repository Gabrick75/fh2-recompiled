// fh2 - automated tests on the PC (see fh2_teste_entrada.h).

#include "fh2_teste_entrada.h"

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

REXCVAR_DEFINE_STRING(fh2_teste_botoes, "", "FH2",
                      "Tests: button script of a virtual pad, in seconds since start. "
                      "\"20:start,26:a,40-60:rt\" presses START at 20 s, A at 26 s and holds the right "
                      "trigger from 40 to 60 s. Buttons: a b x y start back up down left right lb rb lt rt "
                      "stick_up stick_down stick_left stick_right (left stick) and ls rs (thumbstick clicks)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace fh2::teste {
namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using Clock = std::chrono::steady_clock;
namespace in = rex::input;

constexpr in::DeviceId kScriptPad = in::DeviceId(0x4648325343524950ull);  // "FH2SCRIP"
constexpr double kPress = 0.25;  // seconds of a single press

struct Button {
  std::string_view name;
  uint32_t bits;
  uint16_t vk;  // XInputGetKeystroke VK_PAD code
};
constexpr Button kButtons[] = {
    {"a", 0x1000, 0x5800},      {"b", 0x2000, 0x5801},         {"x", 0x4000, 0x5802},
    {"y", 0x8000, 0x5803},      {"rb", 0x0200, 0x5804},        {"lb", 0x0100, 0x5805},
    {"lt", 1u << 16, 0x5806},   {"rt", 1u << 17, 0x5807},      {"up", 0x0001, 0x5810},
    {"down", 0x0002, 0x5811},   {"left", 0x0004, 0x5812},      {"right", 0x0008, 0x5813},
    {"start", 0x0010, 0x5814},  {"back", 0x0020, 0x5815},      {"stick_up", 1u << 18, 0x5820},
    {"stick_down", 1u << 19, 0x5821}, {"stick_right", 1u << 20, 0x5822},
    {"stick_left", 1u << 21, 0x5823},
    // Thumbstick clicks (the camera change is on one of them).
    {"ls", 0x0040, 0x5816},     {"rs", 0x0080, 0x5817},
};

struct Step {
  double start;
  double end;
  uint32_t bits;
};

bool Number(std::string_view text, double& value) {
  const std::string copy(text);
  char* end = nullptr;
  value = std::strtod(copy.c_str(), &end);
  return !copy.empty() && end == copy.c_str() + copy.size() && value >= 0.0;
}

std::vector<Step> Parse(std::string_view text) {
  std::vector<Step> steps;
  size_t i = 0;
  const auto separator = [](char c) { return c == ' ' || c == ',' || c == ';'; };
  while (i < text.size()) {
    while (i < text.size() && separator(text[i])) ++i;
    size_t j = i;
    while (j < text.size() && !separator(text[j])) ++j;
    if (j == i) break;
    const std::string_view step = text.substr(i, j - i);
    i = j;
    const size_t colon = step.find(':');
    const std::string_view times = step.substr(0, colon);
    const std::string_view name = colon == std::string_view::npos ? std::string_view() : step.substr(colon + 1);
    const Button* button = nullptr;
    for (const Button& b : kButtons) {
      if (b.name == name) button = &b;
    }
    const size_t dash = times.find('-');
    double start = 0.0, end = 0.0;
    const bool ok = button && Number(times.substr(0, dash), start) &&
                    (dash == std::string_view::npos ? (end = start + kPress, true)
                                                    : (Number(times.substr(dash + 1), end) && end > start));
    if (!ok) {
      REXLOG_WARN("[teste] script pad: invalid step \"{}\", ignored", step);
      continue;
    }
    steps.push_back({start, end, button->bits});
  }
  return steps;
}

class ScriptPad final : public in::InputDriver {
 public:
  explicit ScriptPad(std::vector<Step> steps)
      : InputDriver(nullptr, 0), steps_(std::move(steps)), start_(Clock::now()) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kScriptPad;
    info.name = "Test script pad";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t packet = 0;
    const uint32_t bits = Update(packet);
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet;
      out_state->gamepad.buttons = uint16_t(bits & 0xFFFF);
      if (bits & (1u << 18)) out_state->gamepad.thumb_ly = 30000;
      if (bits & (1u << 19)) out_state->gamepad.thumb_ly = -30000;
      if (bits & (1u << 20)) out_state->gamepad.thumb_lx = 30000;
      if (bits & (1u << 21)) out_state->gamepad.thumb_lx = -30000;
      out_state->gamepad.left_trigger = (bits & (1u << 16)) ? 0xFF : 0;
      out_state->gamepad.right_trigger = (bits & (1u << 17)) ? 0xFF : 0;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags, in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kScriptPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags, in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (id != kScriptPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    uint32_t packet = 0;
    Update(packet);
    std::lock_guard<std::mutex> lock(mutex_);
    if (keys_.empty()) return X_ERROR_EMPTY;
    if (out_keystroke) *out_keystroke = keys_.front();
    keys_.pop_front();
    return X_ERROR_SUCCESS;
  }

 private:
  uint32_t Update(uint32_t& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    const double t = std::chrono::duration<double>(Clock::now() - start_).count();
    uint32_t bits = 0;
    for (const Step& s : steps_) {
      if (t >= s.start && t < s.end) bits |= s.bits;
    }
    if (bits != current_) {
      for (const Button& b : kButtons) {
        const bool before = (current_ & b.bits) != 0;
        const bool now = (bits & b.bits) != 0;
        if (before == now) continue;
        in::X_INPUT_KEYSTROKE key{};
        key.virtual_key = b.vk;
        key.flags = uint16_t(now ? in::X_INPUT_KEYSTROKE_KEYDOWN : in::X_INPUT_KEYSTROKE_KEYUP);
        if (keys_.size() < 64) keys_.push_back(key);
        REXLOG_INFO("[teste] script pad: {} {} at {:.1f} s", b.name, now ? "down" : "up", t);
      }
      current_ = bits;
      ++packet_;
    }
    packet = packet_;
    return bits;
  }

  std::vector<Step> steps_;
  Clock::time_point start_;
  std::mutex mutex_;
  uint32_t current_ = 0;
  uint32_t packet_ = 1;
  std::deque<in::X_INPUT_KEYSTROKE> keys_;
};

}  // namespace

void EnvolverEntrada(rex::RuntimeConfig& config) {
  const std::string script = REXCVAR_GET(fh2_teste_botoes);
  if (script.empty() || !config.input_factory) return;
  std::vector<Step> steps = Parse(script);
  if (steps.empty()) {
    REXLOG_WARN("[teste] script pad: \"{}\" has no valid steps", script);
    return;
  }
  auto base = config.input_factory;
  config.input_factory = [base, steps](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      // The default factory (ui/rex_app.cpp) returns a rex::input::InputSystem.
      static_cast<in::InputSystem*>(system.get())->AddDriver(std::make_unique<ScriptPad>(steps));
      REXLOG_INFO("[teste] script pad with {} steps", steps.size());
    }
    return system;
  };
}

}  // namespace fh2::teste
