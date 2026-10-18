/**
 * @file        ui/overlay/achievement_toast.cpp
 * @brief       Achievement toast implementation. See achievement_toast.h for details.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_toast.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/flags.h>
#include <rex/system/xcontent.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

REXCVAR_DEFINE_BOOL(achievement_toast_test, false, "UI",
                    "Debug: keep showing a sample achievement toast, to check its size and place on screen");

namespace rex::ui {

namespace {

// The toast title in the game language, like the name under it.
const char* ToastTitle() {
  using rex::system::XLanguage;
  switch (static_cast<XLanguage>(REXCVAR_GET(user_language))) {
    case XLanguage::kGerman:
      return "Erfolg freigeschaltet";
    case XLanguage::kFrench:
      return "Succès déverrouillé";
    case XLanguage::kSpanish:
      return "Logro desbloqueado";
    case XLanguage::kItalian:
      return "Obiettivo sbloccato";
    case XLanguage::kPortuguese:
      return "Conquista desbloqueada";
    case XLanguage::kPolish:
      return "Osiągnięcie odblokowane";
    case XLanguage::kRussian:
      return "Достижение разблокировано";
    case XLanguage::kJapanese:
      return "実績のロックが解除されました";
    case XLanguage::kKorean:
      return "도전 과제 달성";
    case XLanguage::kSChinese:
      return "成就已解锁";
    case XLanguage::kTChinese:
      return "成就已解鎖";
    default:
      return "Achievement Unlocked";
  }
}

}  // namespace

AchievementToastDialog::AchievementToastDialog(ImGuiDrawer* drawer,
                                               ImmediateDrawer* immediate_drawer,
                                               rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), icon_cache_(immediate_drawer, runtime) {
  // Every name at once, so the font atlas is rebuilt once here and not on each unlock.
  drawer->RequestGlyphs(ToastTitle());
  if (runtime && runtime->kernel_state()) {
    for (const auto& achievement : runtime->kernel_state()->achievements().ListAchievements()) {
      drawer->RequestGlyphs(achievement.label);
    }
  }
}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const rex::system::AchievementEvent& event) {
  imgui_drawer()->RequestGlyphs(event.achievement.label);
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::chrono::steady_clock::now()});
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();

  PendingToast toast;
  float age = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      if (!queue_.front().visible_since) {
        queue_.front().visible_since = now;
      }
      age = std::chrono::duration<float>(now - *queue_.front().visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
      } else {
        break;
      }
    }
    if (queue_.empty() && REXCVAR_GET(achievement_toast_test)) {
      rex::system::AchievementEvent prueba;
      prueba.achievement.label = "Sample achievement with a long name to check the line wrap";
      prueba.achievement.gamerscore = 20;
      queue_.push_back({prueba, now});
      queue_.front().visible_since = now;
    }
    if (queue_.empty()) {
      return;
    }
    toast = queue_.front();
  }

  float alpha = 1.0f;
  if (age < 0.3f)
    alpha = age / 0.3f;
  else if (age > kDisplaySeconds - 0.5f)
    alpha = (kDisplaySeconds - age) / 0.5f;
  alpha = std::max(0.0f, std::min(1.0f, alpha));

  /*
   * Sized from the screen, not in fixed pixels. On a phone the logical canvas (DisplaySize: physical pixels over the
   * DPI) is only about 300 lines tall, so the 320x70 toast took a quarter of the screen and, anchored by its top
   * left corner near the bottom edge, ran off it. Here it is about 12 % of the height at most (never larger than on
   * the desktop, never below half size), anchored by the middle of its bottom edge, 3 % above the bottom.
   */
  constexpr float kAltoEscritorio = 70.0f;
  const float escala = std::clamp(io.DisplaySize.y * 0.12f / kAltoEscritorio, 0.5f, 1.0f);
  const float max_width = std::min(400.0f * escala, io.DisplaySize.x * 0.9f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.97f), ImGuiCond_Always,
                          ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(max_width, FLT_MAX));
  const ToastStyle& s = imgui_drawer()->style().toast;
  ImGui::SetNextWindowBgAlpha(s.background_alpha * alpha);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
  // Colors carry the fade envelope; the style holds them at full opacity.
  auto faded = [alpha](ImVec4 c) { return ImVec4(c.x, c.y, c.z, c.w * alpha); };

  const ImVec2 margen(10.0f * escala, 6.0f * escala);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, s.rounding * escala);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, margen);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * escala, 2.0f * escala));
  ImGui::PushStyleColor(ImGuiCol_Text, faded(s.text));
  if (ImGui::Begin("##ach_toast", nullptr, flags)) {
    ImGui::SetWindowFontScale(escala);
    ImmediateTexture* icon = icon_cache_.GetIcon(toast.event.achievement);
    if (icon) {
      ImGui::Image(reinterpret_cast<ImTextureID>(icon), ImVec2(s.icon_size * escala, s.icon_size * escala));
      ImGui::SameLine();
    }

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, faded(s.title));
    ImGui::TextUnformatted(ToastTitle());
    ImGui::PopStyleColor();
    // The name wraps inside the toast's width, with the gamerscore after it.
    ImGui::PushTextWrapPos(max_width - margen.x);
    ImGui::TextWrapped("%s (%dG)", toast.event.achievement.label.c_str(),
                       static_cast<int>(toast.event.achievement.gamerscore));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
  }
  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}

}  // namespace rex::ui
