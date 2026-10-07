/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay & Rien Gupta, 2026 - Adapted for ReXGlue runtime (POSIX + macOS)
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <iostream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <rex/assert.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform/env.h>
#include <rex/string.h>

#include <dirent.h>
#include <ftw.h>
#include <libgen.h>
#include <pwd.h>

// macOS off_t is 64-bit with no *64 large-file variants; Linux keeps the
// explicit *64 forms for legacy 32-bit off_t distributions.
#if defined(__APPLE__) || defined(__SWITCH__)
using rex_off64_t = off_t;
#define rex_fseeko64 fseeko
#define rex_ftello64 ftello
#define rex_ftruncate64 ftruncate
#else
using rex_off64_t = off64_t;
#define rex_fseeko64 fseeko64
#define rex_ftello64 ftello64
#define rex_ftruncate64 ftruncate64
#endif

namespace rex {

std::string path_to_utf8(const std::filesystem::path& path) {
  return path.string();
}

std::u16string path_to_utf16(const std::filesystem::path& path) {
  return rex::string::to_utf16(path.string());
}

std::filesystem::path to_path(const std::string_view source) {
  return source;
}

std::filesystem::path to_path(const std::u16string_view source) {
  return rex::string::to_utf8(source);
}

namespace filesystem {

#if defined(__SWITCH__)
// libnx stores the arguments hbloader passes here. They are not declared in its
// headers, but they are libnx.a symbols. C linkage: the namespace does not matter.
extern "C" int __system_argc;
extern "C" char** __system_argv;
#endif

std::filesystem::path GetExecutablePath() {
#if defined(__APPLE__)
  // Darwin has no /proc; query the executable path via the dyld API. The first
  // call reports the required buffer size.
  uint32_t executable_path_size = 0;
  _NSGetExecutablePath(nullptr, &executable_path_size);
  if (!executable_path_size) {
    return {};
  }

  std::string executable_path(executable_path_size, '\0');
  if (_NSGetExecutablePath(executable_path.data(), &executable_path_size) != 0) {
    return {};
  }

  if (!executable_path.empty() && executable_path.back() == '\0') {
    executable_path.pop_back();
  }

  std::error_code ec;
  std::filesystem::path canonical_path = std::filesystem::weakly_canonical(executable_path, ec);
  return ec ? std::filesystem::path(executable_path) : canonical_path;
#elif defined(__SWITCH__)
  // Horizon has no /proc. The Linux branch would compile anyway and silently return
  // empty. hbloader passes the NRO path as argv[0], for example
  // sdmc:/switch/nfsmw/nfsmw.nro, and libnx stores it in __system_argv.
  //
  // It is returned without the "sdmc:". libstdc++ does not know newlib's devices:
  // to it, "sdmc:/switch/nfsmw" has no root, so it is relative, and
  // std::filesystem::absolute prepends the current directory. The result is
  // "sdmc:/switch/nfsmw/sdmc:/switch/nfsmw/game_root", stat fails with an error
  // other than "does not exist" and exists() throws filesystem_error. That is how
  // NFSMW's first boot died, in Runtime::SetupVfs, with nothing in the log.
  // "/switch/..." does have a root, and newlib resolves it on the default device,
  // which is the SD. It is only stripped if that form can be queried.
  if (__system_argc > 0 && __system_argv && __system_argv[0] && __system_argv[0][0]) {
    std::string path = __system_argv[0];
    constexpr std::string_view kSdmc = "sdmc:/";
    if (std::string_view(path).starts_with(kSdmc)) {
      std::string rooted = path.substr(kSdmc.size() - 1);
      struct stat st;
      if (stat(rooted.c_str(), &st) == 0) {
        return std::filesystem::path(rooted);
      }
    }
    return std::filesystem::path(path);
  }
  return {};
#else
  char buff[FILENAME_MAX] = "";
  readlink("/proc/self/exe", buff, FILENAME_MAX);
  std::string s(buff);
  return s;
#endif
}

std::filesystem::path GetExecutableFolder() {
#if defined(__ANDROID__)
  // The process is app_process under /system/bin, which the app can neither read files from nor write to.
  // The Activity names the app's own folder (settings, shader library, caches, logs) before starting.
  if (auto carpeta = rex::platform::env::get("REX_APP_FOLDER"); carpeta && !carpeta->empty()) {
    return std::filesystem::path(*carpeta);
  }
#endif
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetUserFolder() {
#if defined(__SWITCH__)
  // Horizon has no XDG_DATA_HOME, no HOME and no user database. The branch below
  // would end up in pw->pw_dir with pw NULL and kill the process at startup:
  // rex_app.cpp calls it to decide the data folder. On Switch the data goes next
  // to the NRO, as in the portable PC version.
  auto exe_dir = GetExecutableFolder();
  return exe_dir.empty() ? std::filesystem::path("/switch") : exe_dir;
#else
  // get preferred data home
  if (auto xdg = rex::platform::env::get("XDG_DATA_HOME")) {
    return std::filesystem::path(*xdg);
  }

  // if XDG_DATA_HOME not set, fallback to HOME directory
  if (auto home = rex::platform::env::get("HOME")) {
    return std::filesystem::path(*home) / ".local" / "share";
  }

  // if HOME not set, fall back to passwd entry
  struct passwd pw1;
  struct passwd* pw;
  char buf[4096];  // could potentionally lower this
  getpwuid_r(getuid(), &pw1, buf, sizeof(buf), &pw);
  assert(&pw1 == pw);  // sanity check
  return std::filesystem::path(pw->pw_dir) / ".local" / "share";
#endif
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return fopen(path.c_str(), std::string(mode).c_str());
}

bool Seek(FILE* file, int64_t offset, int origin) {
  return rex_fseeko64(file, rex_off64_t(offset), origin) == 0;
}

int64_t Tell(FILE* file) {
  return int64_t(rex_ftello64(file));
}

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (fflush(file)) {
    return false;
  }
  int64_t position = Tell(file);
  if (position < 0) {
    return false;
  }
  if (rex_ftruncate64(fileno(file), rex_off64_t(length))) {
    return false;
  }
  if (uint64_t(position) > length) {
    if (!Seek(file, 0, SEEK_END)) {
      return false;
    }
  }
  return true;
}

static int removeCallback(const char* fpath, const struct stat* sb, int typeflag,
                          struct FTW* ftwbuf) {
  int rv = remove(fpath);
  return rv;
}

static uint64_t convertUnixtimeToWinFiletime(time_t unixtime) {
  // Linux uses number of seconds since 1/1/1970, and Windows uses
  // number of nanoseconds since 1/1/1601
  // so we convert linux time to nanoseconds and then add the number of
  // nanoseconds from 1601 to 1970
  // see https://msdn.microsoft.com/en-us/library/ms724228
  uint64_t filetime = filetime = (unixtime * 10000000) + 116444736000000000;
  return filetime;
}

bool CreateEmptyFile(const std::filesystem::path& path) {
  int file = creat(path.c_str(), 0774);
  if (file >= 0) {
    close(file);
    return true;
  }
  return false;
}

class PosixFileHandle : public FileHandle {
 public:
  PosixFileHandle(std::filesystem::path path, int handle)
      : FileHandle(std::move(path)), handle_(handle) {}
  ~PosixFileHandle() override {
    close(handle_);
    handle_ = -1;
  }
  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    ssize_t out = pread(handle_, buffer, buffer_length, file_offset);
    if (out < 0) {
      *out_bytes_read = 0;
      return false;
    }
    *out_bytes_read = static_cast<size_t>(out);
    return true;
  }
  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    ssize_t out = pwrite(handle_, buffer, buffer_length, file_offset);
    if (out < 0) {
      *out_bytes_written = 0;
      return false;
    }
    *out_bytes_written = static_cast<size_t>(out);
    return true;
  }
  bool SetLength(size_t length) override { return ftruncate(handle_, length) >= 0 ? true : false; }
  void Flush() override { fsync(handle_); }

 private:
  int handle_ = -1;
};

std::unique_ptr<FileHandle> FileHandle::OpenExisting(const std::filesystem::path& path,
                                                     uint32_t desired_access,
                                                     bool /*allow_share_delete*/) {
  // POSIX allows unlinking/replacing an open file, so there is no share-delete
  // analog to thread through here.
  // O_RDONLY/O_WRONLY/O_RDWR are an enumeration in the O_ACCMODE bits, not
  // independent flags. kGenericExecute grants neither right, matching
  // GENERIC_EXECUTE on the Windows path.
  constexpr uint32_t kReadRights =
      FileAccess::kGenericRead | FileAccess::kGenericAll | FileAccess::kFileReadData;
  constexpr uint32_t kWriteRights = FileAccess::kGenericWrite | FileAccess::kGenericAll |
                                    FileAccess::kFileWriteData | FileAccess::kFileAppendData;

  const bool want_read = (desired_access & kReadRights) != 0;
  const bool want_write = (desired_access & kWriteRights) != 0;

  int open_access = O_RDONLY;
  if (want_read && want_write) {
    open_access = O_RDWR;
  } else if (want_write) {
    open_access = O_WRONLY;
  }

  // No O_APPEND for kFileAppendData: writes go through pwrite with an explicit
  // offset, which O_APPEND would override.
  int handle = open(path.c_str(), open_access);
  if (handle == -1) {
    // TODO(benvanik): pick correct response.
    return nullptr;
  }
  return std::make_unique<PosixFileHandle>(path, handle);
}

bool GetInfo(const std::filesystem::path& path, FileInfo* out_info) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      out_info->type = FileInfo::Type::kDirectory;
      out_info->total_size = 0;
    } else {
      out_info->type = FileInfo::Type::kFile;
      out_info->total_size = st.st_size;
    }
    out_info->path = path.parent_path();
    out_info->name = path.filename();
    out_info->create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    out_info->access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    out_info->write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    return true;
  }
  return false;
}

#if REX_PLATFORM_ANDROID
bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  // SAF access requires a Java ContentResolver and an app-owned JNI bridge.
  // The Android app currently imports selected trees into private POSIX storage.
  (void)uri;
  (void)mode;
  errno = ENOSYS;
  return -1;
}
#endif

std::vector<FileInfo> ListFiles(const std::filesystem::path& path) {
  std::vector<FileInfo> result;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }

  while (auto ent = readdir(dir)) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    FileInfo info;

    info.name = ent->d_name;
    struct stat st;
    stat((path / info.name).c_str(), &st);
    info.create_timestamp = convertUnixtimeToWinFiletime(st.st_ctime);
    info.access_timestamp = convertUnixtimeToWinFiletime(st.st_atime);
    info.write_timestamp = convertUnixtimeToWinFiletime(st.st_mtime);
    info.path = path;
    if (ent->d_type == DT_DIR) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = st.st_size;
    }
    result.push_back(info);
  }
  closedir(dir);
  return result;
}

}  // namespace filesystem
}  // namespace rex
