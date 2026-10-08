/**
 * @file    nfsmw_hlsl.cpp
 * @brief   Minimal Xbox 360 shader translator to HLSL, for NFS Most Wanted
 *
 * Why XenosRecomp's main.cpp is not used
 *
 * The original does three things: translate to HLSL, compile to DXIL/SPIR-V with the
 * DXC API, and package the result with smol-v and zstd. Of those three, only the
 * first is needed here:
 *
 *  - smol-v and zstd are submodules that are not checked out, and they only compress
 *    the output.
 *  - The DXC API requires the library (dxcompiler), but the Vulkan SDK ships the
 *    dxc.exe executable, which does exactly the same from the command line.
 *
 * So this translates to HLSL and nothing more. dxc.exe does the step to SPIR-V.
 *
 * The other difference, and the important one
 *
 * XenosRecomp assumes the shader container carries the signature 0x102A1100, which is
 * Sonic Unleashed's (2008). NFS Most Wanted is from 2005 and uses 0x102A0E00: an
 * earlier version of Microsoft's shader compiler. nfsmw_contenedor.h normalizes the
 * 2005 tables and validates the control flow.
 *
 * Input
 *
 * Original containers extracted from ZZDATA0.BIN with nfsmw_buscar_contenedores. Old
 * console dumps lack the correct physical part; they are kept for regression and
 * rejected.
 *
 * Usage
 *   nfsmw_hlsl <input folder> <output folder> <shader_common.h>
 */

#include "XenosRecomp/shader_recompiler.h"
#include "nfsmw_contenedor.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <cstring>

namespace {

std::vector<uint8_t> LeerTodo(const std::filesystem::path& ruta) {
  std::vector<uint8_t> datos;
  FILE* f = std::fopen(ruta.string().c_str(), "rb");
  if (!f) {
    return datos;
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    datos.resize(static_cast<size_t>(n));
    if (std::fread(datos.data(), 1, datos.size(), f) != datos.size()) {
      datos.clear();
    }
  }
  std::fclose(f);
  return datos;
}

/*
 * The two known signatures: the 2008 one XenosRecomp expects and the 2005 one Most
 * Wanted uses. They are compared without the low byte, which belongs to the tool itself.
 */
bool FirmaValida(uint32_t flags_be) {
  const uint32_t flags = __builtin_bswap32(flags_be);
  const uint32_t v = flags & 0xFFFFFF00u;
  return v == 0x102A1100u || v == 0x102A0E00u;
}


}  // namespace

int main(int argc, char** argv) try {
  if (argc < 4) {
    std::printf("uso: nfsmw_hlsl <entrada> <salida> <shader_common.h>\n");
    return 1;
  }
  const std::filesystem::path entrada = argv[1];
  const std::filesystem::path salida = argv[2];

  const std::vector<uint8_t> comun = LeerTodo(argv[3]);
  if (comun.empty()) {
    std::printf("no pude leer %s\n", argv[3]);
    return 1;
  }
  const std::string_view include(reinterpret_cast<const char*>(comun.data()), comun.size());

  std::error_code ec;
  if (std::filesystem::exists(salida) && !std::filesystem::is_empty(salida)) {
    std::fprintf(stderr, "la salida debe estar vacia para no mezclar resultados anteriores\n");
    return 1;
  }
  std::filesystem::create_directories(salida, ec);
  if (ec) throw std::runtime_error("no se pudo crear la carpeta de salida");

  size_t total = 0, ok = 0, saltados = 0;
  for (const auto& e : std::filesystem::directory_iterator(entrada)) {
    if (!e.is_regular_file() || e.path().extension() != ".bin") {
      continue;
    }
    const std::vector<uint8_t> datos = LeerTodo(e.path());
    ++total;
    if (datos.size() < 24) {
      std::printf("  %s: archivo ilegible o cabecera truncada\n", e.path().filename().string().c_str());
      ++saltados;
      continue;
    }

    uint32_t flags_be = 0;
    std::memcpy(&flags_be, datos.data(), 4);
    if (!FirmaValida(flags_be)) {
      std::printf("  %-20s firma desconocida 0x%08X\n", e.path().filename().string().c_str(),
                  __builtin_bswap32(flags_be));
      ++saltados;
      continue;
    }

    /* The 2005 one is converted to the 2008 form before translating. */
    const bool es2005 = (__builtin_bswap32(flags_be) & 0xFFFFFF00u) == 0x102A0E00u;
    nfsmw::Flujo flujo;
    std::vector<uint8_t> conv;
    try {
      conv = es2005 ? nfsmw::Convertir2005(datos, flujo) : datos;
    } catch (const std::exception& error) {
      std::printf("  %s: %s\n", e.path().filename().string().c_str(), error.what());
      ++saltados;
      continue;
    }
    if (conv.empty()) {
      std::printf("  %-20s no pude convertir el contenedor\n", e.path().filename().string().c_str());
      ++saltados;
      continue;
    }

    std::printf("  %s: CF %u bytes, %u instrucciones\n", e.path().filename().string().c_str(), flujo.bytes, flujo.instrucciones);
    std::fflush(stdout);
    ShaderRecompiler recompilador;
    try {
      recompilador.recompile(conv.data(), include);
    } catch (const std::exception& error) {
      std::printf("  %s: traduccion rechazada: %s\n", e.path().filename().string().c_str(), error.what());
      ++saltados;
      continue;
    }
    if (recompilador.out.empty()) {
      std::printf("  %-20s no produjo nada\n", e.path().filename().string().c_str());
      ++saltados;
      continue;
    }

    auto destino = salida / e.path().filename();
    destino.replace_extension(".hlsl");
    FILE* f = std::fopen(destino.string().c_str(), "wb");
    if (f) {
      const bool completo = std::fwrite(recompilador.out.data(), 1, recompilador.out.size(), f) == recompilador.out.size();
      const bool cerrado = std::fclose(f) == 0;
      if (completo && cerrado) ++ok;
      else ++saltados;
    } else {
      ++saltados;
    }
  }

  std::printf("\n%zu shaders: %zu traducidos, %zu saltados\n", total, ok, saltados);
  return total > 0 && ok == total ? 0 : 2;
} catch (const std::exception& error) {
  std::fprintf(stderr, "error: %s\n", error.what());
  return 1;
}
