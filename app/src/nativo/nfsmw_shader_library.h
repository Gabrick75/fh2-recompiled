#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace nfsmw::native {

// The original container also identifies constants and interface. The physical
// code alone is not enough. It is looked up before the driver creates its object.
struct Shader {
  std::vector<uint8_t> original;
  std::vector<uint32_t> spirv;
  uint64_t huella = 0;
  bool vertices = false;
};

class BibliotecaShaders {
 public:
  // Transactional load: an invalid file does not replace the library.
  // Pointers returned by Buscar last until the next successful load.
  void Cargar(std::span<const uint8_t> archivo);
  void Cargar(const std::filesystem::path& archivo);
  const Shader* Buscar(std::span<const uint8_t> original) const;
  const std::vector<Shader>& shaders() const { return shaders_; }

 private:
  std::vector<Shader> shaders_;
};

// Versioned local format, little-endian, with integrity checks.
// It contains private data derived from the game; it is not embedded in the executable.
std::vector<uint8_t> EmpaquetarShaders(std::vector<Shader> shaders);

}  // namespace nfsmw::native
