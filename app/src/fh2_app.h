// fh2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/rex_app.h>

#include <rex/ui/presenter.h>

#include "fh2_captura.h"
#include "fh2_teste_entrada.h"

#if REX_PLATFORM_ANDROID
// android/app/src/main/cpp/android_rendimiento.cpp: keeps the game threads off the slow cores.
extern "C" void fh2_android_nucleos_grandes_aplicar();
#endif

class Fh2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fh2App>(new Fh2App(ctx, "fh2",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // The game needs the console's cache partition: after the first race it creates
    // cache:\replay_stream (rewind / replays), and without the partition it hangs on the loading
    // screen reading a null pointer. Mounted on user/cache_partition.
    rex::cvar::SetFlagByName("mount_cache", "true");
#if REX_PLATFORM_ANDROID
    fh2_android_nucleos_grandes_aplicar();
#endif
    fh2::teste::EnvolverEntrada(config);
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
    // PNG captures of the game image for unattended tests (fh2_captura_cada_s; off by default).
    fh2::captura::Arrancar([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  void OnShutdown() override { fh2::captura::Parar(); }
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
