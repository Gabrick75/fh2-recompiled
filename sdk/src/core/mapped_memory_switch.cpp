/**
 * @file        rex/core/mapped_memory_switch.cpp
 * @brief       MappedMemory for Horizon: no file mmap, the file is read into RAM
 *
 * Why it cannot be done as on POSIX
 *
 * Horizon has no file mmap. mapped_memory_posix.cpp mmaps the whole file and
 * returns the pointer; here the only honest way to hand out a pointer to the
 * data is to read it into memory.
 *
 * Who uses it (measured)
 *
 *   user_module.cpp         default.xex, tens of MB: no problem
 *   host_path_entry.cpp     loose files of the extracted game
 *   stfs_container_device   4 bytes of header
 *   vfs_dump.cpp            debugging
 *   disc_image_device.cpp   the whole ISO
 *
 * The last one is the problem. An Xbox 360 ISO is ~7 GB and the console has
 * 3.2 GB of RAM. It does not fit, and there is no way to make it fit. That is why
 * there is a cap, and the error message says what to do: use the extracted game
 * (game_root folder, which the launcher already accepts) instead of the ISO.
 *
 * The game itself does not come through here to read its data: its reads go
 * through File::ReadSync, which is pread. This only affects callers that ask for
 * a pointer.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <cstdio>
#include <cstring>
#include <memory>

#include <malloc.h>
#include <unistd.h>

#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

namespace rex::memory {

namespace {

/*
 * Nothing above this size is attempted: guest memory alone already takes 512 MB
 * of physical memory plus the game heap, and a file that ate the rest would bring
 * the console down far from here and without a clue. Better to fail with a message.
 */
constexpr size_t kMaxInMemoryFile = 256 * 1024 * 1024;

constexpr size_t kPage = 0x1000;

class SwitchMappedMemory : public MappedMemory {
 public:
  SwitchMappedMemory(uint8_t* data, size_t size, FILE* file, Mode mode, size_t offset)
      : MappedMemory(data, size), file_(file), mode_(mode), offset_(offset) {}

  ~SwitchMappedMemory() override { Close(0); }

  void Close(uint64_t truncate_size) override {
    if (data_) {
      // On POSIX the shared mapping writes back by itself; here it has to be flushed.
      if (mode_ == Mode::kReadWrite && file_) {
        WriteBack();
      }
      free(data_);
      data_ = nullptr;
    }
    if (file_) {
      if (truncate_size) {
        fflush(file_);
        ftruncate(fileno(file_), static_cast<off_t>(truncate_size));
      }
      fclose(file_);
      file_ = nullptr;
    }
  }

  void Flush() override {
    if (mode_ == Mode::kReadWrite && file_ && data_) {
      WriteBack();
    }
  }

 private:
  void WriteBack() {
    if (fseeko(file_, static_cast<off_t>(offset_), SEEK_SET) == 0) {
      fwrite(data_, 1, size_, file_);
      fflush(file_);
    }
  }

  FILE* file_;
  Mode mode_;
  size_t offset_;
};

}  // namespace

std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  const std::string ruta = path.string();
  FILE* f = fopen(ruta.c_str(), mode == Mode::kRead ? "rb" : "r+b");
  if (!f) {
    return nullptr;
  }

  size_t len = length;
  if (!len) {
    if (fseeko(f, 0, SEEK_END) != 0) {
      fclose(f);
      return nullptr;
    }
    const off_t fin = ftello(f);
    if (fin < 0 || static_cast<size_t>(fin) <= offset) {
      fclose(f);
      return nullptr;
    }
    len = static_cast<size_t>(fin) - offset;
  }

  // mmap with length 0 fails on POSIX; this mimics it.
  if (len == 0) {
    fclose(f);
    return nullptr;
  }

  if (len > kMaxInMemoryFile) {
    REXLOG_ERROR("MappedMemory: '{}' pide {} MB y en Switch no hay mmap de ficheros, asi que "
                 "habria que leerlo entero a RAM. Si es la ISO del juego, usa los ficheros "
                 "EXTRAIDOS (carpeta game_root) en su lugar.",
                 ruta, len >> 20);
    fclose(f);
    return nullptr;
  }

  const size_t reservado = (len + kPage - 1) & ~(kPage - 1);
  uint8_t* datos = static_cast<uint8_t*>(memalign(kPage, reservado));
  if (!datos) {
    REXLOG_ERROR("MappedMemory: sin memoria para leer '{}' ({} MB)", ruta, len >> 20);
    fclose(f);
    return nullptr;
  }

  size_t leido = 0;
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0) {
    leido = fread(datos, 1, len, f);
  }
  // Reading past the end with mmap gives zeros up to the end of the page; here
  // the tail is zero-filled the same way instead of being left as garbage.
  if (leido < reservado) {
    std::memset(datos + leido, 0, reservado - leido);
  }

  // For reading the file is no longer needed; for writing it is kept for the flush.
  if (mode == Mode::kRead) {
    fclose(f);
    f = nullptr;
  }

  return std::make_unique<SwitchMappedMemory>(datos, len, f, mode, offset);
}

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& path, size_t chunk_size, bool low_address_space) {
  (void)path;
  (void)chunk_size;
  (void)low_address_space;
  // Not implemented on POSIX either.
  return nullptr;
}

}  // namespace rex::memory

#endif  // REX_PLATFORM_SWITCH
