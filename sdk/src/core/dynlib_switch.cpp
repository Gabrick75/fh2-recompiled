/**
 * @file        rex/core/dynlib_switch.cpp
 * @brief       DynamicLibrary backend for Horizon: there is no dynamic loading
 *
 * An NRO cannot load libraries at run time: dlopen does not exist and everything is
 * linked statically. This backend does not fail loudly; it returns "not loaded" for
 * any path, because the SDK's two callers already handle that case:
 *
 *   - gpu_plugin_loader.cpp, which on Switch must be resolved through static
 *     registration (see rex::system::LoadGpuPlugin)
 *   - the optional RenderDoc and SPIRV-Tools loaders, which make no sense on the
 *     console
 *
 * If a third caller that depends on dlopen is ever added, it will show up here as a
 * Load() that returns false, not as a link error.
 */

#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Nintendo Switch-only");

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  (void)path;
  (void)mode;
  Close();
  return false;
}

void DynamicLibrary::Close() {
  handle_ = nullptr;
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  (void)name;
  return nullptr;
}

}  // namespace rex::platform
