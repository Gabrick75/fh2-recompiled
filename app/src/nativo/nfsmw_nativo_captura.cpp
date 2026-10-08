// nfsmw - PNG captures of the game image (see nfsmw_nativo_captura.h).
//
// The PNG is written uncompressed (deflate "stored" blocks): the SDK does not
// include stb_image_write and this is enough for testing. A 1280x720 capture
// takes about 2.8 MB.

#include "nfsmw_nativo_captura.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

REXCVAR_DEFINE_INT32(nfsmw_captura_cada_s, 0, "NFSMW",
                     "Guardar una captura PNG de la imagen del juego cada N segundos en capturas/ "
                     "junto al ejecutable (0 = nunca; solo para pruebas)")
    .range(0, 3600);
REXCVAR_DEFINE_INT32(nfsmw_captura_max, 20, "NFSMW", "Maximo de capturas por ejecucion")
    .range(1, 1000);

namespace nfsmw::captura {
namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_parar = false;
std::thread g_hilo;

uint32_t Crc32(const uint8_t* datos, size_t n, uint32_t crc) {
  static const std::array<uint32_t, 256> tabla = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) {
    crc = tabla[(crc ^ datos[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

void PonerBe32(std::vector<uint8_t>& salida, uint32_t valor) {
  salida.push_back(uint8_t(valor >> 24));
  salida.push_back(uint8_t(valor >> 16));
  salida.push_back(uint8_t(valor >> 8));
  salida.push_back(uint8_t(valor));
}

void Chunk(std::vector<uint8_t>& salida, const char tipo[4], const std::vector<uint8_t>& datos) {
  PonerBe32(salida, uint32_t(datos.size()));
  const size_t inicio_tipo = salida.size();
  salida.insert(salida.end(), tipo, tipo + 4);
  salida.insert(salida.end(), datos.begin(), datos.end());
  const uint32_t crc =
      Crc32(salida.data() + inicio_tipo, salida.size() - inicio_tipo, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
  PonerBe32(salida, crc);
}

// RGBX (R8 G8 B8 X8, like rex::ui::RawImage) to 8-bit RGB PNG.
std::vector<uint8_t> CodificarPng(const rex::ui::RawImage& imagen) {
  const uint32_t ancho = imagen.width;
  const uint32_t alto = imagen.height;
  std::vector<uint8_t> crudo;
  crudo.reserve(size_t(alto) * (size_t(ancho) * 3 + 1));
  for (uint32_t y = 0; y < alto; ++y) {
    crudo.push_back(0);  // sin filtro
    const uint8_t* fila = imagen.data.data() + size_t(y) * imagen.stride;
    for (uint32_t x = 0; x < ancho; ++x) {
      crudo.push_back(fila[x * 4]);
      crudo.push_back(fila[x * 4 + 1]);
      crudo.push_back(fila[x * 4 + 2]);
    }
  }

  // zlib with "stored" blocks and Adler-32 at the end.
  std::vector<uint8_t> zlib = {0x78, 0x01};
  size_t pos = 0;
  do {
    const size_t trozo = std::min<size_t>(65535, crudo.size() - pos);
    const bool ultimo = pos + trozo == crudo.size();
    zlib.push_back(ultimo ? 1 : 0);
    zlib.push_back(uint8_t(trozo));
    zlib.push_back(uint8_t(trozo >> 8));
    zlib.push_back(uint8_t(~trozo));
    zlib.push_back(uint8_t(~trozo >> 8));
    zlib.insert(zlib.end(), crudo.begin() + pos, crudo.begin() + pos + trozo);
    pos += trozo;
  } while (pos < crudo.size());
  uint32_t a = 1, b = 0;
  for (uint8_t byte : crudo) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PonerBe32(zlib, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PonerBe32(ihdr, ancho);
  PonerBe32(ihdr, alto);
  ihdr.push_back(8);  // bits per channel
  ihdr.push_back(2);  // RGB
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // filtro adaptativo
  ihdr.push_back(0);  // sin entrelazado
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", zlib);
  Chunk(png, "IEND", {});
  return png;
}

void Bucle(std::function<rex::ui::Presenter*()> obtener_presentador, int cada_s, int maximo) {
  const std::filesystem::path carpeta = rex::filesystem::GetExecutableFolder() / "capturas";
  const auto inicio = std::chrono::steady_clock::now();
  for (int n = 1; n <= maximo; ++n) {
    {
      std::unique_lock<std::mutex> cerrojo(g_mutex);
      if (g_cv.wait_for(cerrojo, std::chrono::seconds(cada_s), [] { return g_parar; })) {
        return;
      }
    }
    rex::ui::Presenter* presentador = obtener_presentador ? obtener_presentador() : nullptr;
    rex::ui::RawImage imagen;
    if (!presentador || !presentador->CaptureGuestOutput(imagen) || !imagen.width ||
        !imagen.height) {
      REXLOG_WARN("[captura] {}: no hay imagen del juego que capturar", n);
      continue;
    }
    std::error_code ec;
    std::filesystem::create_directories(carpeta, ec);
    const auto segundos = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now() - inicio)
                              .count();
    char nombre[64];
    std::snprintf(nombre, sizeof(nombre), "captura_%03d_%04llds.png", n,
                  static_cast<long long>(segundos));
    const std::filesystem::path ruta = carpeta / nombre;
    const std::vector<uint8_t> png = CodificarPng(imagen);
    std::ofstream fichero(ruta, std::ios::binary);
    fichero.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!fichero) {
      REXLOG_WARN("[captura] No se pudo escribir {}", ruta.string());
      continue;
    }
    REXLOG_INFO("[captura] {} ({}x{})", ruta.string(), imagen.width, imagen.height);
  }
}

}  // namespace

void Arrancar(std::function<rex::ui::Presenter*()> obtener_presentador) {
  const int cada_s = REXCVAR_GET(nfsmw_captura_cada_s);
  if (cada_s <= 0 || g_hilo.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    g_parar = false;
  }
  g_hilo = std::thread(Bucle, std::move(obtener_presentador), cada_s,
                       int(REXCVAR_GET(nfsmw_captura_max)));
  REXLOG_INFO("[captura] Una captura cada {} s (maximo {})", cada_s,
              int(REXCVAR_GET(nfsmw_captura_max)));
}

void Parar() {
  {
    std::lock_guard<std::mutex> cerrojo(g_mutex);
    g_parar = true;
  }
  g_cv.notify_all();
  if (g_hilo.joinable()) {
    g_hilo.join();
  }
}

}  // namespace nfsmw::captura
