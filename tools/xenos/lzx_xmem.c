// Decompresses an LZX stream (libmspack, the library the SDK uses for XEX files) whose XMem frame headers were
// already removed (extrair_fxobj_pistas.py does that): FH2's zip entries with method 21 use a 128 KB window.
//
//   lzx_xmem <input> <uncompressed size> <output>
// Tries window sizes 2^15 to 2^21 and writes the first that gives exactly the expected size.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mspack.h>
#include <lzx.h>
typedef struct { struct mspack_system sys; } S;
typedef struct { unsigned char* b; long n, o; } F;
static int rd(struct mspack_file* f, void* p, int c) { F* m = (F*)f; long r = m->n - m->o; if (c < r) r = c; memcpy(p, m->b + m->o, r); m->o += r; return (int)r; }
static int wr(struct mspack_file* f, void* p, int c) { F* m = (F*)f; long r = m->n - m->o; if (c < r) r = c; memcpy(m->b + m->o, p, r); m->o += r; return (int)r; }
static void* al(struct mspack_system* s, size_t n) { return calloc(n, 1); }
static void fr(void* p) { free(p); }
static void cp(void* s, void* d, size_t n) { memcpy(d, s, n); }
int raw(unsigned char* in, long inn, unsigned char* out, long outn, int bits) {
  struct mspack_system sys = {0}; sys.read = rd; sys.write = wr; sys.alloc = al; sys.free = fr; sys.copy = cp;
  F fi = {in, inn, 0}, fo = {out, outn, 0};
  struct lzxd_stream* z = lzxd_init(&sys, (struct mspack_file*)&fi, (struct mspack_file*)&fo, bits, 0, 0x8000, outn, 0);
  if (!z) return -1;
  int e = lzxd_decompress(z, outn); lzxd_free(z);
  return e ? -e : (int)fo.o;
}
int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb"); fseek(f, 0, 2); long n = ftell(f); fseek(f, 0, 0);
  unsigned char* in = malloc(n); fread(in, 1, n, f); fclose(f);
  long outn = atol(argv[2]);
  unsigned char* out = malloc(outn);
  for (int bits = 15; bits <= 21; ++bits) {
    int r = raw(in, n, out, outn, bits);
    printf("bruto bits %d: %d\n", bits, r);
    if (r == outn) { FILE* o = fopen(argv[3], "wb"); fwrite(out, 1, outn, o); fclose(o); return 0; }
  }
  printf("primeiros bytes: "); for (int i = 0; i < 16; ++i) printf("%02x", in[i]); printf("\n");
  return 1;
}
