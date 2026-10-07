/**
 * @file        rex/core/memory_switch.cpp
 * @brief       SDK memory API (rex::memory) on top of the Horizon guest memory core
 *
 * This is a thin adapter. All the real work (reservation, commit, mirrors,
 * protection by unmapping) is in guest_memory_switch.cpp, which was tested on
 * the console on its own. See that file for the reasons behind the design.
 *
 * Who uses this API
 *
 * Measured with grep over the SDK: only xmemory.cpp (the guest heaps) and one
 * QueryProtect from the MMIO handler. Every address that arrives falls inside
 * the guest window. Anything outside it is handled separately and with a
 * warning, instead of pretending that it works.
 *
 * Three differences from POSIX worth knowing
 *
 * 1. There are no arbitrary fixed addresses. Horizon chooses where the window
 *    goes; xmemory.cpp asks for the base with GetFileMappingBase() instead of
 *    trying 1<<32, 1<<33... as it does on Linux.
 *
 * 2. Read-only does not exist. A page in kReadOnly is left unmapped, so its
 *    reads fault too and the exception handler emulates them. That is the price
 *    of not being able to change permissions on the window (ProcessMem lacks
 *    PermChangeAllowed; measured).
 *
 * 3. Freeing does not return memory to the system. Decommit and Release only
 *    remove access in that view. The backing stays until the mapping is closed.
 *    The same happens on Linux, where the heap lives in shared memory and
 *    madvise(DONTNEED) on a shared mapping does not release the pages. And it is
 *    required: the same backing is mirrored in other views, and releasing it
 *    from one would take it away from all of them.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <cstring>
#include <filesystem>

#include <malloc.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>

#include "guest_memory_switch.h"

namespace rex::memory {

namespace {

constexpr size_t kPage = 0x1000;

/* ReXGlue creates a single mapping: the guest memory one. */
constexpr FileMappingHandle kSwitchMapping = 1;

constexpr size_t AlignUp(size_t v) { return (v + kPage - 1) & ~(kPage - 1); }

RexGmAccess ToGm(PageAccess a) {
  switch (a) {
    case PageAccess::kNoAccess:
      return REX_GM_NONE;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      return REX_GM_READ;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      return REX_GM_WRITE;
  }
  return REX_GM_NONE;
}

PageAccess FromGm(RexGmAccess a) {
  switch (a) {
    case REX_GM_NONE:
      return PageAccess::kNoAccess;
    case REX_GM_READ:
      return PageAccess::kReadOnly;
    case REX_GM_WRITE:
      return PageAccess::kReadWrite;
  }
  return PageAccess::kNoAccess;
}

bool InWindow(void* p, size_t len) {
  const uint8_t* base = RexGmBase();
  if (!base || !p) return false;
  const uintptr_t a = reinterpret_cast<uintptr_t>(p);
  const uintptr_t b = reinterpret_cast<uintptr_t>(base);
  return a >= b && a + len <= b + RexGmSize();
}

}  // namespace

size_t page_size() {
  return kPage;
}

size_t allocation_granularity() {
  return kPage;
}

bool IsWritableExecutableMemorySupported() {
  // ReXGlue is static recompilation: it does not generate code at run time, so it
  // does not need pages that are writable and executable at once. And a Horizon
  // homebrew cannot have them without the code memory dance. Same as macOS.
  return false;
}

/// Base where the guest window ended up. Only exists on Switch: on the other
/// platforms xmemory.cpp picks the address and the system honors it.
void* GetFileMappingBase(FileMappingHandle handle) {
  return handle == kSwitchMapping ? RexGmBase() : nullptr;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  if (length == 0) {
    return nullptr;
  }

  if (!base_address) {
    // No fixed address: ordinary host memory. No use in the SDK asks for it today
    // (measured), but the API allows it and it is trivial to provide.
    const size_t len = AlignUp(length);
    void* p = memalign(kPage, len);
    if (p) {
      std::memset(p, 0, len);
    }
    return p;
  }

  if (!InWindow(base_address, length)) {
    REXLOG_ERROR("AllocFixed en Switch fuera de la ventana del invitado: 0x{:016X} (0x{:X} bytes). "
                 "Horizon no permite fijar direcciones arbitrarias.",
                 reinterpret_cast<uintptr_t>(base_address), length);
    return nullptr;
  }

  if (allocation_type == AllocationType::kReserve) {
    // The whole window was already reserved when the mapping was created. Reserving
    // is a no-op; accessing before committing will fault, just like PROT_NONE on Linux.
    return base_address;
  }

  size_t offset = 0;
  if (!RexGmWindowToOffset(reinterpret_cast<uint64_t>(base_address), &offset)) {
    REXLOG_ERROR("AllocFixed: 0x{:016X} no pertenece a ninguna vista del invitado",
                 reinterpret_cast<uintptr_t>(base_address));
    return nullptr;
  }

  if (!RexGmCommit(offset, length, ToGm(access))) {
    // This is the message that fires if Horizon's mapping ceiling is hit, so it
    // carries everything needed to tell which of the three ran out.
    // The real ceiling is the process limit on mappable memory
    // (LimitableResource_Memory): each chunk is mapped once for the shadow and
    // once more per 360 view. The limit figures come from the profiler
    // (switch_perf.cpp); switch.h cannot be included here without clashing with
    // the SDK types.
    REXLOG_ERROR("AllocFixed: no se pudo confirmar 0x{:X} bytes en 0x{:016X}. Fallo 0x{:08X}; "
                 "respaldo {} MB, mapeado {} MB contando espejos.",
                 length, reinterpret_cast<uintptr_t>(base_address), RexGmLastResult(),
                 RexGmCommittedBytes() >> 20, RexGmMappedBytes() >> 20);
    return nullptr;
  }

  // The core always commits memory as accessible and keeps any existing protection;
  // the requested permission is applied on top. In the common case (read-write on
  // an unprotected range) this returns immediately.
  if (!RexGmProtect(static_cast<uint8_t*>(base_address), length, ToGm(access), nullptr)) {
    REXLOG_ERROR("AllocFixed: confirmado pero no se pudo aplicar el permiso en 0x{:016X} "
                 "(fallo 0x{:08X})",
                 reinterpret_cast<uintptr_t>(base_address), RexGmLastResult());
    return nullptr;
  }
  return base_address;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  if (!base_address) {
    return false;
  }

  if (!InWindow(base_address, length ? length : 1)) {
    // Host memory requested with AllocFixed(nullptr, ...).
    if (deallocation_type == DeallocationType::kRelease) {
      free(base_address);
    }
    return true;
  }

  // Linux does munmap(base, 0) when length is 0, which fails. This mimics it,
  // because the Linux build is the reference known to work.
  if (length == 0) {
    return false;
  }

  // Both Decommit and Release only remove access in this view. See the file
  // header: releasing the backing would remove it from every mirror.
  switch (deallocation_type) {
    case DeallocationType::kDecommit:
    case DeallocationType::kRelease:
      return RexGmProtect(static_cast<uint8_t*>(base_address), length, REX_GM_NONE, nullptr);
    default:
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }
  if (!InWindow(base_address, length)) {
    REXLOG_ERROR("Protect en Switch fuera de la ventana del invitado: 0x{:016X} (0x{:X} bytes)",
                 reinterpret_cast<uintptr_t>(base_address), length);
    return false;
  }

  RexGmAccess old = REX_GM_NONE;
  const bool ok = RexGmProtect(static_cast<uint8_t*>(base_address), length, ToGm(access), &old);
  if (out_old_access) {
    *out_old_access = FromGm(old);
  }
  if (!ok) {
    REXLOG_ERROR("Protect: fallo 0x{:08X} en 0x{:016X} (0x{:X} bytes)", RexGmLastResult(),
                 reinterpret_cast<uintptr_t>(base_address), length);
  }
  return ok;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  access_out = PageAccess::kNoAccess;
  if (!InWindow(base_address, 1)) {
    length = 0;
    return false;
  }

  // The input length is used as the walk limit. The only caller (MMIO) passes one
  // page and only looks at the permission.
  size_t len = length;
  RexGmAccess a = REX_GM_NONE;
  if (!RexGmQueryProtect(static_cast<uint8_t*>(base_address), &len, &a)) {
    length = 0;
    return false;
  }
  length = len;
  access_out = FromGm(a);
  return true;
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
  (void)path;
  (void)access;
  (void)commit;
  // Horizon has no named shared memory and no sparse memory. The "file" is the
  // window of the guest memory core, and it is reserved here, empty. Real memory
  // arrives with AllocFixed(kCommit).
  if (!RexGmBase() && !RexGmInit(AlignUp(length))) {
    REXLOG_ERROR("No se pudo reservar la ventana del invitado de 0x{:X} bytes. "
                 "Hace falta title takeover: en modo applet no hay espacio.",
                 length);
    return kFileMappingHandleInvalid;
  }
  return kSwitchMapping;
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  (void)path;
  if (handle == kSwitchMapping) {
    RexGmShutdown();
  }
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  (void)access;
  if (handle != kSwitchMapping || !base_address || (file_offset % kPage) != 0) {
    return nullptr;
  }
  if (!InWindow(base_address, length)) {
    return nullptr;
  }
  if (!RexGmAddView(static_cast<uint8_t*>(base_address), file_offset, length)) {
    REXLOG_ERROR("MapFileView: no se pudo registrar la vista 0x{:016X} (0x{:X} bytes desde 0x{:X}); "
                 "fallo 0x{:08X}",
                 reinterpret_cast<uintptr_t>(base_address), length, file_offset,
                 RexGmLastResult());
    return nullptr;
  }
  return base_address;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  (void)base_address;
  (void)length;
  // The core does not remove individual views: they are all removed together when
  // the mapping is closed. xmemory.cpp only tears down views when it is destroyed,
  // right before CloseFileMappingHandle, so the result is the same.
  return handle == kSwitchMapping;
}

}  // namespace rex::memory

#endif  // REX_PLATFORM_SWITCH
