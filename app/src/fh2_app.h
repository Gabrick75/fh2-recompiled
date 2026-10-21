// fh2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/rex_app.h>

#include <rex/ui/presenter.h>

#include "fh2_captura.h"
#include "fh2_teste_entrada.h"
#include "nfsmw_ajustes_graficos.h"
#include "nfsmw_nativo_sistema.h"

#if REX_PLATFORM_ANDROID
// android/app/src/main/cpp/android_rendimiento.cpp: keeps the game threads off the slow cores.
extern "C" void fh2_android_nucleos_grandes_aplicar();
extern "C" void fh2_android_afinidade_iniciar();
#endif

void Fh2PerfilStart();  // fh2_perfil.cpp
void Fh2MundoAplicar(rex::Runtime* runtime);  // fh2_mundo.cpp

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
    // Native renderer (src/nativo, docs/renderizador_nativo.md): with nfsmw_renderizador = nativo the graphics
    // system is its own and the GPU emulation plugin is not loaded (ReXApp only loads it when config.graphics is
    // empty). The default is still the emulation.
    if (nfsmw::nativo::Activo()) {
      config.graphics = nfsmw::nativo::CrearSistemaGrafico();
      // FXAA (nfsmw_antialiasing) and the optional post-processing reach the output pass through these atomics.
      // NFSMW sets them up in its app; without this call the launcher's FXAA never switched on here.
      nfsmw::ajustes::VigilarAjustesEnVivo();
    }
    fh2::teste::EnvolverEntrada(config);
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override {
    Fh2PerfilStart();  // fh2_perfil.cpp, off unless fh2_perfil_s is set
    // Grass, objects, reflections, shadows and crowd at the launcher's factors (fh2_mundo_*), before the game runs.
    Fh2MundoAplicar(runtime());
#if REX_PLATFORM_ANDROID
    fh2_android_afinidade_iniciar();  // android_rendimiento.cpp, off unless fh2_android_afinidade is set
#endif
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
