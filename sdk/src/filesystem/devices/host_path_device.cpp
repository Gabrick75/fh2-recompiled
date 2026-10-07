/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>

#include <algorithm>
#include <atomic>
#include <chrono>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/string/utf8.h>

namespace rex::filesystem {

namespace {
// How much SD work the path lookup is saving. The [io] summary in xboxkrnl_io.cpp writes it to
// the log. Relaxed on purpose: they are counters, they do not synchronize anything.
std::atomic<uint64_t> g_misses_en_seco{0};
std::atomic<uint64_t> g_stats_en_sd{0};
std::atomic<uint64_t> g_barridos_en_sd{0};
std::atomic<uint64_t> g_entradas_en_arbol{0};
}  // namespace

EstadisticasRutas LeerEstadisticasRutas() {
  EstadisticasRutas e;
  e.misses_en_seco = g_misses_en_seco.load(std::memory_order_relaxed);
  e.stats_en_sd = g_stats_en_sd.load(std::memory_order_relaxed);
  e.barridos_en_sd = g_barridos_en_sd.load(std::memory_order_relaxed);
  e.entradas_en_arbol = g_entradas_en_arbol.load(std::memory_order_relaxed);
  return e;
}

HostPathDevice::HostPathDevice(const std::string_view mount_path,
                               const std::filesystem::path& host_path, bool read_only,
                               bool allow_share_delete)
    : Device(mount_path),
      name_("STFS"),
      host_path_(host_path),
      read_only_(read_only),
      allow_share_delete_(allow_share_delete) {}

HostPathDevice::~HostPathDevice() = default;

bool HostPathDevice::Initialize() {
  if (!std::filesystem::exists(host_path_)) {
    if (!read_only_) {
      // Create the path.
      std::filesystem::create_directories(host_path_);
    } else {
      REXFS_ERROR("Host path does not exist");
      return false;
    }
  }

  auto root_entry = new HostPathEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  /*
   * This walks the whole tree with one stat per file. On the Switch SD that is not free, so it is
   * measured here; it is also the basis for everything else, because from here on a path that is not
   * in memory does not exist (see ResolvePath). If the entry count looks wrong or it takes long, it
   * shows here.
   */
  const auto antes = std::chrono::steady_clock::now();
  const uint64_t entradas_antes = g_entradas_en_arbol.load(std::memory_order_relaxed);
  PopulateEntry(root_entry);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - antes).count();
  REXFS_INFO("[io] montado '{}' en {}: {} entradas en {:.1f} ms ({})",
             rex::path_to_utf8(host_path_), mount_path_,
             g_entradas_en_arbol.load(std::memory_order_relaxed) - entradas_antes, ms,
             read_only_ ? "solo lectura" : "escritura");

  return true;
}

void HostPathDevice::Dump(string::StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

/*
 * A failing open used to cost 32 ms.
 *
 * A console log had 148 lines "[NtCreateFile] FAILED ... 0xc000000f". In the bursts where the game
 * asks for files that do not exist, one after another with nothing in between, the gap between two
 * consecutive lines of the same thread is 32-33 ms exactly (see the D:\sound\IG_Global\*.abk block,
 * fourteen in a row). That time was not the game's: it was ours.
 *
 * Why. When the in-memory tree did not have the entry, this method went to the SD: first a stat of
 * the exact name and, if that failed, ListFiles() of the whole parent directory. And ListFiles does a
 * stat per entry (filesystem_posix.cpp). A directory with fifty files = fifty ioctls to the SD. Per
 * open. And without keeping anything: next time, start over.
 *
 * Worst of all: VirtualFileSystem::ResolvePath calls this with the global critical region held. That
 * is the one mutex.h itself describes as "disabling interrupts in the guest ... no IO!". So those
 * 32 ms did not stop only the thread opening the file: they stopped all of them. Hence the frames of
 * 200-850 ms that coincide with the bursts of failures.
 *
 * The fix. Initialize() already walks the game's whole tree with PopulateEntry, so for every
 * directory that exists all its children are known; and GetChild already compares case-insensitively.
 * If a directory is marked complete and GetChild fails, the file does not exist, full stop: no stat
 * and no enumeration.
 *
 * Only on read-only devices (the game data). Writable ones (saves and profiles) behave exactly as
 * before, because there the contents do change underneath us.
 */
Entry* HostPathDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  auto* resolved = root_entry_->ResolvePath(path);
  if (resolved) {
    return resolved;
  }

  // Fallback to a lazy case-insensitive host lookup when an entry is missing
  // from the in-memory tree (for example because casing differs on Linux).
  auto* current_entry = static_cast<HostPathEntry*>(root_entry_.get());
  for (const auto& part : rex::string::utf8_split_path(path)) {
    if (part.empty()) {
      continue;
    }

    auto* child = current_entry->GetChild(part);
    if (!child) {
      // The directory was already fully enumerated and the name is not there: it does not exist. No SD access.
      if (read_only_ && current_entry->children_complete()) {
        g_misses_en_seco.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }

      // Stat the exact name first: enumerating a directory of hundreds of
      // archives costs milliseconds a walk. Only casing mismatch reaches
      // the scan below.
      const auto exact_path = current_entry->host_path() / rex::to_path(part);
      rex::filesystem::FileInfo exact_info;
      g_stats_en_sd.fetch_add(1, std::memory_order_relaxed);
      if (rex::filesystem::GetInfo(exact_path, &exact_info)) {
        auto* exact_child = HostPathEntry::Create(this, current_entry, exact_path, exact_info);
        if (!exact_child) {
          return nullptr;
        }
        current_entry->children_.push_back(std::unique_ptr<Entry>(exact_child));
        current_entry = static_cast<HostPathEntry*>(exact_child);
        continue;
      }

      /*
       * Careful: the directory is not marked complete here even though it was just enumerated. This
       * path only adds the matching entry to children_, not the others, so children_ would still be
       * incomplete and marking it complete would make lookups of its siblings fail falsely. The mark
       * is set by PopulateEntry, which does add them all.
       */
      g_barridos_en_sd.fetch_add(1, std::memory_order_relaxed);
      auto child_infos = rex::filesystem::ListFiles(current_entry->host_path());
      auto match = std::find_if(child_infos.begin(), child_infos.end(), [&](const auto& info) {
        return rex::string::utf8_equal_case(rex::path_to_utf8(info.name), part);
      });
      if (match == child_infos.end()) {
        return nullptr;
      }

      auto new_child = HostPathEntry::Create(this, current_entry,
                                             current_entry->host_path() / match->name, *match);
      if (!new_child) {
        return nullptr;
      }
      child = new_child;
      current_entry->children_.push_back(std::unique_ptr<Entry>(new_child));
    }

    current_entry = static_cast<HostPathEntry*>(child);
  }

  return current_entry;
}

void HostPathDevice::PopulateEntry(HostPathEntry* parent_entry) {
  auto child_infos = rex::filesystem::ListFiles(parent_entry->host_path());
  for (auto& child_info : child_infos) {
    auto child = HostPathEntry::Create(this, parent_entry,
                                       parent_entry->host_path() / child_info.name, child_info);
    parent_entry->children_.push_back(std::unique_ptr<Entry>(child));
    g_entradas_en_arbol.fetch_add(1, std::memory_order_relaxed);

    if (child_info.type == rex::filesystem::FileInfo::Type::kDirectory) {
      PopulateEntry(child);
    }
  }
  /*
   * This directory has been read in full: whatever is not in children_ does not exist on disk.
   *
   * Only if it returned something. ListFiles returns an empty list both when the directory is empty
   * and when opendir failed, and the two cannot be told apart from here. Marking a directory
   * complete after a failed opendir would make its files invisible forever, so in that case it is
   * left unmarked and that directory keeps the old slow lookup. An empty one has nothing inside to
   * look up, so it costs nothing.
   */
  if (!parent_entry->children_.empty()) {
    parent_entry->children_complete_ = true;
  }
}

}  // namespace rex::filesystem
