/*
 * libelf stubs.
 *
 * Mesa's libvulkan.a pulls in nv_cubin.c.o, which parses NVIDIA CUBIN files with
 * libelf. devkitPro does not ship libelf and that path never runs on Switch, so
 * satisfying the linker is enough.
 *
 * If it were ever really called, elf_version() returning 0 (unsupported version)
 * makes the libelf caller abort cleanly instead of carrying on.
 */
#include <stddef.h>

unsigned int elf_version(unsigned int v) { (void)v; return 0; }
void *elf_memory(char *image, size_t size) { (void)image; (void)size; return NULL; }
int elf_end(void *elf) { (void)elf; return 0; }
int elf_kind(void *elf) { (void)elf; return 0; }
void *elf64_getehdr(void *elf) { (void)elf; return NULL; }
void *elf64_getshdr(void *scn) { (void)scn; return NULL; }
void *elf_getscn(void *elf, size_t idx) { (void)elf; (void)idx; return NULL; }
void *elf_nextscn(void *elf, void *scn) { (void)elf; (void)scn; return NULL; }
void *elf_getdata(void *scn, void *data) { (void)scn; (void)data; return NULL; }
int elf_getshdrstrndx(void *elf, size_t *dst) { (void)elf; if (dst) *dst = 0; return -1; }
char *elf_strptr(void *elf, size_t sec, size_t off) { (void)elf; (void)sec; (void)off; return NULL; }
int elf_errno(void) { return 0; }
const char *elf_errmsg(int err) { (void)err; return "libelf no disponible en Switch"; }
