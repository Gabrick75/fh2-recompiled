// Prototype: FH2 on the ShiftGlue SDK (arcanite24/shiftglue-sdk), to evaluate its Vulkan FH1 native executor on
// FH2. Not part of FH2 Recompiled; built from referencias/fh2-shiftglue.

#include "generated/default/fh2_init.h"

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/ui/presenter.h>

#include "fh2_captura.h"
#include "fh2_teste_entrada.h"

namespace fh2::toque {
void Envolver(rex::RuntimeConfig& config);
}
namespace fh2::medidor {
void Arrancar();
}
#if REX_PLATFORM_ANDROID
extern "C" void fh2_android_nucleos_grandes_aplicar();
extern "C" void fh2_android_afinidade_iniciar();
#endif

class Fh2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fh2App>(new Fh2App(ctx, "fh2", PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    rex::cvar::SetFlagByName("mount_cache", "true");
    fh2::teste::EnvolverEntrada(config);
#if REX_PLATFORM_ANDROID
    fh2_android_nucleos_grandes_aplicar();
    fh2::toque::Envolver(config);
#endif
  }
  void OnPostSetup() override {
    fh2::medidor::Arrancar();
#if REX_PLATFORM_ANDROID
    fh2_android_afinidade_iniciar();
#endif
    fh2::captura::Arrancar([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
  }
  void OnShutdown() override { fh2::captura::Parar(); }
};

REX_DEFINE_APP(fh2, Fh2App::Create)
