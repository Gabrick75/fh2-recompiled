#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <dlfcn.h>

#include <string>

#include <rex/logging.h>

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
  Close();
  int flags = (mode == SymbolResolution::kImmediate) ? RTLD_NOW : RTLD_LAZY;
  handle_ = dlopen(path.c_str(), flags);
  // Module registries name a library the Windows way ("fh2_Module"), where LoadLibrary adds the
  // extension. On POSIX the file is lib<name>.so (.dylib on macOS), found through the search path.
  if (!handle_ && !path.has_extension() && !path.has_parent_path()) {
#if REX_PLATFORM_MAC
    const std::string platform_name = "lib" + path.string() + ".dylib";
#else
    const std::string platform_name = "lib" + path.string() + ".so";
#endif
    handle_ = dlopen(platform_name.c_str(), flags);
  }
  if (!handle_) {
    REXLOG_ERROR("dlopen({}) failed: {}", path.string(), dlerror());
  }
  return handle_ != nullptr;
}

void DynamicLibrary::Close() {
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_)
    return nullptr;
  return dlsym(handle_, name);
}

}  // namespace rex::platform
