/**
 * @file        rex/core/switch_libc_supplement.c
 * @brief       POSIX functions that newlib declares but libnx does not implement
 *
 * Why it exists
 *
 * Found by linking the SDK's real rex::memory::Memory against rexcore for
 * Switch: everything resolves except six libc functions. newlib declares them in
 * its headers, so the code compiles without a warning; but nobody defines them,
 * and the failure only shows up at link time.
 *
 * The newlib headers are included on purpose: if any signature here does not
 * match the declared one, this file stops compiling instead of linking wrongly.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include <switch.h>

/*
 * --- pread / pwrite ------------------------------------------------------
 *
 * newlib does not have them and Horizon offers no positioned read on a newlib
 * descriptor. They are emulated with lseek + read/write, which is not atomic:
 * between the lseek and the read another thread could move the position of the
 * same descriptor. That is why each descriptor has its own lock, spread over 64
 * stripes, and the position is restored at the end, as pread guarantees.
 *
 * The game reads its data through here (File::ReadSync), from several threads. A
 * single global lock would serialize all of the game's disk reads and would show
 * up as stutter when loading zones; with stripes, two different files do not
 * wait for each other.
 *
 * Known limitation: it only protects against other pread/pwrite calls. A direct
 * read() or lseek() on the same descriptor from another thread at the same time
 * could still clobber it. The SDK does not mix both styles on one file.
 */

#define REX_FRANJAS 64
static Mutex g_franjas[REX_FRANJAS]; /* zeroed = unlocked */

static Mutex* Franja(int fd) {
    return &g_franjas[(unsigned)fd % REX_FRANJAS];
}

ssize_t pread(int fd, void* buf, size_t nbytes, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    Mutex* m = Franja(fd);
    mutexLock(m);
    ssize_t r = -1;
    const off_t antes = lseek(fd, 0, SEEK_CUR);
    if (antes >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
        r = read(fd, buf, nbytes);
        const int e = errno;
        lseek(fd, antes, SEEK_SET);
        errno = e;
    }
    mutexUnlock(m);
    return r;
}

ssize_t pwrite(int fd, const void* buf, size_t nbytes, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }
    Mutex* m = Franja(fd);
    mutexLock(m);
    ssize_t r = -1;
    const off_t antes = lseek(fd, 0, SEEK_CUR);
    if (antes >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
        r = write(fd, buf, nbytes);
        const int e = errno;
        lseek(fd, antes, SEEK_SET);
        errno = e;
    }
    mutexUnlock(m);
    return r;
}

/*
 * --- users ---------------------------------------------------------------
 *
 * Horizon has no POSIX users. filesystem_posix.cpp queries them to find the
 * home folder; the answer is "does not exist", which is what POSIX defines for
 * an entry that is not found (0 and *result set to NULL), and the caller falls
 * back to its alternative.
 */

uid_t getuid(void) {
    return 0;
}

int getpwuid_r(uid_t uid, struct passwd* pwd, char* buf, size_t buflen, struct passwd** result) {
    (void)uid;
    (void)pwd;
    (void)buf;
    (void)buflen;
    if (result) {
        *result = NULL;
    }
    return 0;
}

/*
 * --- creat ---------------------------------------------------------------
 * POSIX defines it as exactly this.
 */

int creat(const char* path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

/*
 * --- thread names --------------------------------------------------------
 * Horizon does not keep thread names on a retail console; they are only useful
 * to a debugger. The call is accepted and does nothing.
 */

/*
 * The name is recorded, though, for the profiler (switch_perf.cpp), which puts
 * it in its report. Only the calling thread's own name: the SDK names each thread
 * from inside it, and here there is no direct way to go from a pthread_t to its
 * handle.
 */
void RexSwitchPerfSetThreadName(u32 handle, const char* name);

int pthread_setname_np(pthread_t thread, const char* name) {
    if (pthread_equal(thread, pthread_self()))
        RexSwitchPerfSetThreadName(threadGetCurHandle(), name);
    return 0;
}

/* --- getentropy ------------------------------------------------------------
 *
 * newlib's getentropy only forwards to _getentropy_r, the syscall a platform is
 * expected to provide, and libnx does not. It surfaced linking the full
 * runtime through CMake: the manual probes did not pull in the object that
 * calls it (std::random_device, FFmpeg's arc4random). libnx's randomGet is a
 * ChaCha generator seeded from the kernel's entropy and needs no service.
 * getentropy's contract caps a request at 256 bytes. */

#include <sys/reent.h>

int _getentropy_r(struct _reent* r, void* buf, size_t len) {
    if (len > 256) {
        r->_errno = EIO;
        return -1;
    }
    randomGet(buf, len);
    return 0;
}

/* --- timegm ----------------------------------------------------------------
 *
 * newlib has mktime, which interprets struct tm in the local time zone, but not
 * timegm, the UTC inverse of gmtime. The STFS reader (stfs_xbox.h) converts
 * FAT timestamps with it. Days from the civil calendar (Howard Hinnant's
 * algorithm), valid for any proleptic Gregorian date. */

time_t timegm(struct tm* tm) {
    long long year = (long long)tm->tm_year + 1900;
    long long month = tm->tm_mon; /* 0-11, may be out of range */
    year += month / 12;
    month %= 12;
    if (month < 0) {
        month += 12;
        year -= 1;
    }
    const long long m = month + 1; /* 1-12 */
    const long long y = year - (m <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + (long long)tm->tm_hour * 3600 + (long long)tm->tm_min * 60 +
                    tm->tm_sec);
}

/*
 * --- assert --------------------------------------------------------------
 *
 * newlib's __assert_func writes to stderr and calls abort(). In an NRO stderr
 * goes nowhere and abort() returns to the HOME menu without a word: that is how
 * the first boot of the SDK looked, dying in an assert_zero in clock_posix.cpp
 * during static constructors, with no log and nothing on screen.
 *
 * This version writes the failure to <NRO folder>/logs/rex/rex_assert.log and
 * ends with diagAbortWithResult, which instead of a silent exit shows error
 * 2345-0048 and generates a crash report. Defining it here is enough: nobody uses
 * __assert, the other symbol in libc_a-assert.o, so that object never reaches
 * the link.
 */

#include <assert.h>
#include <stdio.h>

const char* RexSwitchLogDir(void);

void __assert_func(const char* file, int line, const char* func, const char* expr) {
    static _Atomic int dentro = 0;
    if (dentro++ == 0) { /* an assert inside fopen does not re-enter */
        /* in <NRO folder>/logs/rex/ (switch_crash_hooks.c) */
        char ruta[FS_MAX_PATH];
        snprintf(ruta, sizeof(ruta), "%srex_assert.log", RexSwitchLogDir());
        FILE* f = fopen(ruta, "a");
        if (f) {
            fprintf(f, "assert fallido: %s\n  en %s:%d (%s)\n", expr ? expr : "?",
                    file ? file : "?", line, func ? func : "?");
            fclose(f);
        }
    }
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}
