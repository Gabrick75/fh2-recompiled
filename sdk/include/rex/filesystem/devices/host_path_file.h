/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/file.h>

namespace rex::filesystem {

class HostPathEntry;

/*
 * How much is being served from RAM instead of from the SD.
 *
 * aciertos  = game reads answered without going to disk.
 * rellenos  = times the disk had to be read to fill the window.
 * directas  = reads that bypassed the window (large ones, writes, save files).
 * bytes_ram = bytes the game received from the window.
 */
struct EstadisticasVentana {
  uint64_t aciertos = 0;
  uint64_t rellenos = 0;
  uint64_t directas = 0;
  uint64_t bytes_ram = 0;
  uint64_t ventanas_vivas = 0;
};
EstadisticasVentana LeerEstadisticasVentana();

/*
 * Exact-range read cache. See nfsmw_io_rangos_mb in the .cpp.
 *
 * Everything is cumulative since startup, not per interval: there are few events and the total is
 * what matters.
 *
 * aciertos     = large reads answered from RAM, without going to the SD.
 * fallos       = eligible large reads that did have to go to the SD.
 * bytes_ram    = bytes delivered to the game from the cache.
 * bytes_disco  = bytes those same reads brought from the SD.
 * entradas     = ranges alive right now.
 * bytes_vivos  = what those ranges take in the host heap.
 * expulsiones  = ranges dropped because of the cap. If this grows, the cap is too small.
 * sin_memoria  = the cache turned itself off for lack of RAM.
 */
struct EstadisticasRangos {
  uint64_t aciertos = 0;
  uint64_t fallos = 0;
  uint64_t bytes_ram = 0;
  uint64_t bytes_disco = 0;
  uint64_t entradas = 0;
  uint64_t bytes_vivos = 0;
  uint64_t expulsiones = 0;
  uint64_t tope_mb = 0;  // the cvar, so the summary does not have to declare it on its own
  // Why something was not cached. Without this the cache could not be tuned.
  uint64_t bajo_suelo = 0;          // lecturas demasiado pequenas
  uint64_t sobre_techo = 0;         // demasiado grandes
  uint64_t secuenciales = 0;        // the level-load sweep, which is not kept
  uint64_t secuenciales_bytes = 0;
  uint64_t suelo_kb = 0;
  bool sin_memoria = false;
};
EstadisticasRangos LeerEstadisticasRangos();

class HostPathFile : public File {
 public:
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<rex::filesystem::FileHandle> file_handle);
  ~HostPathFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;
  X_STATUS Flush() override;

 private:
  /*
   * Read-ahead window.
   *
   * On Horizon every NtReadFile from the game became a pread() against the SD with no buffering in
   * between: the descriptor is opened with a bare open() (filesystem_posix.cpp), so there is not even the
   * stdio buffer. A file read header by header (the .gin, the .abk, the indices) means dozens of trips to
   * the card to read a few bytes each time.
   *
   * A window is read in one go and whatever falls inside is served from RAM. Files that fit whole in the
   * window end up entirely in memory, which is exactly what was wanted for the small, frequently opened
   * ones.
   *
   * Only on read-only devices. It is never enabled for saves and profiles: there the bytes change under
   * our feet and a stale window would mean a corrupted save.
   *
   * If a refill returns less than requested (end of file, or a short read from the file system, which
   * cannot be told apart from here), the request is answered with the usual direct read; after the
   * fourth in a row the window is turned off for this file. It never returns less than the path without
   * the window would have.
   *
   * The window used to be dead, and it cost something too. Measured (472.9 MB over 1,290 reads): 5 hits
   * out of 1,232 eligible reads = 0.41 %, 0.3 MB served from RAM out of 472.9 MB (0.06 %), and 66
   * refills x 64 KB = 4.1 MB of SD reads spent to serve those 0.3 MB. A net loss.
   *
   * The cause was the threshold: only requests of at most window/4 go into the window, i.e. 16 KB with a
   * 64 KB window. The game's average read is 375 KB; the smallest it does in a loop is 64 KB (it is in
   * the binary itself: the loop at 0x8284FE78 calls NtReadFile with r9 = 0x10000). Not a single game read
   * was under 16 KB, so all of them took the direct path.
   *
   * Fix: the window is now 256 KB, which puts the threshold at 64 KB and catches exactly the game's
   * 64 KB loop; and the cap on live windows drops from 128 to 16 (there were never more than 10), so the
   * RAM ceiling drops from 8 MB to 4 MB. It comes with an audit: if after 8 refills this file has not
   * given at least one hit per refill, the pattern is not sequential and the window is turned off for
   * it; that way it can never again be pure cost in silence.
   */
  bool RellenarVentana(size_t byte_offset, size_t pedido, std::span<uint8_t> buffer,
                       size_t* out_bytes_read, X_STATUS* out_status);

  // Direct read split into chunks. See the comment on nfsmw_io_trozo_mb in the .cpp.
  X_STATUS LeerDirecta(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read);

  std::unique_ptr<rex::filesystem::FileHandle> file_handle_;

  std::vector<uint8_t> ventana_;
  size_t ventana_inicio_ = 0;
  size_t ventana_bytes_ = 0;
  size_t ventana_tam_ = 0;
  uint32_t ventana_cortas_ = 0;
  // Per-file audit: a window that does not hit turns itself off.
  uint32_t ventana_rellenos_ = 0;
  uint32_t ventana_aciertos_ = 0;
  bool ventana_activa_ = false;
  bool ventana_contada_ = false;
  // File id inside the RAM cache (0 = this file is not cached). See
  // nfsmw_io_cache_mb.
  uint32_t cache_id_ = 0;
};

}  // namespace rex::filesystem
