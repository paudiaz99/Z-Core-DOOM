/*
 * nolibc.c
 *
 * Minimal libc replacement for DOOM on Z-Core.
 *
 * The riscv32-unknown-elf toolchain's precompiled newlib contains
 * RISC-V C-extension (compressed) instructions.  Z-Core is RV32IM
 * only, so any compressed instruction causes an illegal-instruction
 * trap.  This file provides every libc/libgcc symbol DOOM needs,
 * compiled with -march=rv32im_zicsr so no compressed code is linked.
 *
 * Build with -nostdlib (no newlib, no libgcc).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>

#include "config.h"

/* Declare mini_vsnprintf directly instead of including mini-printf.h,
 * because that header #defines snprintf→mini_snprintf which would
 * collide with mini-printf.c's own definition under LTO. */
int mini_vsnprintf(char *buffer, unsigned int buffer_len, const char *fmt, va_list va);

/* forward decl from libc_backend.c (underscore-prefixed) */
extern int   _open(const char *pathname, int flags);
extern int   _close(int fd);
extern ssize_t _read(int fd, void *buf, size_t nbyte);
extern ssize_t _write(int fd, const void *buf, size_t nbyte);
extern off_t _lseek(int fd, off_t offset, int whence);
extern int   _fstat(int fd, void *statbuf);
extern int   _stat(const char *filename, void *statbuf);
extern int   _isatty(int fd);
extern void *_sbrk(intptr_t increment);

/* ================================================================
 * Compiler / runtime glue
 * ================================================================ */

/* errno */
static int _errno_val;
int *__errno(void) { return &_errno_val; }

/*
 * _impure_ptr — newlib headers expand stdout/stdin/stderr through
 * this pointer.  We set it to a zeroed buffer large enough that
 * offset-based accesses (e.g. _REENT->_stdout) read NULL rather
 * than faulting on a NULL base.  Our printf/fprintf implementations
 * never dereference FILE pointers, so NULL is safe.
 */
static char _reent_buf[1024] __attribute__((aligned(4)));
void *_impure_ptr = _reent_buf;

/*
 * _ctype_ — character-classification table for newlib ctype.h macros.
 * Index 0 is for EOF (-1); indices 1..256 map to chars 0..255.
 */
#define _U  0x01  /* upper */
#define _L  0x02  /* lower */
#define _N  0x04  /* digit */
#define _S  0x08  /* whitespace */
#define _P  0x10  /* punctuation */
#define _C  0x20  /* control */
#define _X  0x40  /* hex letter */
#define _B  0x80  /* blank (space/tab) */

const unsigned char _ctype_[257] = {
    0,                                                              /* EOF */
    _C,_C,_C,_C,_C,_C,_C,_C,_C,_C|_S|_B,_C|_S,_C|_S,_C|_S,_C|_S,_C,_C, /* 0-15 */
    _C,_C,_C,_C,_C,_C,_C,_C,_C,_C,_C,_C,_C,_C,_C,_C,             /* 16-31 */
    _S|_B,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,_P,         /* 32-47 */
    _N,_N,_N,_N,_N,_N,_N,_N,_N,_N,_P,_P,_P,_P,_P,_P,             /* 48-63 */
    _P,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U|_X,_U,_U,_U,_U,_U,_U,_U,_U,_U, /* 64-79 */
    _U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_U,_P,_P,_P,_P,_P,            /* 80-95 */
    _P,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L|_X,_L,_L,_L,_L,_L,_L,_L,_L,_L, /* 96-111 */
    _L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_L,_P,_P,_P,_P,_C,            /* 112-127 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 128-159 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 160-191 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 192-223 */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 224-255 */
};

#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B


/* ================================================================
 * Memory operations
 * ================================================================ */

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    /* Word-aligned fast path — essential for SDRAM correctness.
     * The AXI-Lite ↔ SDRAM bridge is designed for 32-bit transactions;
     * byte-by-byte access can hit CDC edge cases. */
    if (((uintptr_t)d & 3) == 0 && ((uintptr_t)s & 3) == 0) {
        uint32_t *d32 = (uint32_t *)d;
        const uint32_t *s32 = (const uint32_t *)s;
        while (n >= 4) {
            *d32++ = *s32++;
            n -= 4;
        }
        d = (uint8_t *)d32;
        s = (const uint8_t *)s32;
    }
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = s;
    /* Word-aligned fast path for zero-fill and uniform fill */
    if (((uintptr_t)p & 3) == 0) {
        uint32_t c4 = (uint8_t)c;
        c4 |= c4 << 8;
        c4 |= c4 << 16;
        uint32_t *p32 = (uint32_t *)p;
        while (n >= 4) {
            *p32++ = c4;
            n -= 4;
        }
        p = (uint8_t *)p32;
    }
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *p = a;
    const uint8_t *q = b;
    while (n--) {
        unsigned x = *p++ - *q++;
        if (x) return (int)x;
    }
    return 0;
}


/* ================================================================
 * String operations
 * ================================================================ */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

static int _tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && _tolower((unsigned char)*a) == _tolower((unsigned char)*b))
    { a++; b++; }
    return _tolower((unsigned char)*a) - _tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && _tolower((unsigned char)*a) == _tolower((unsigned char)*b))
    { a++; b++; n--; }
    return n ? _tolower((unsigned char)*a) - _tolower((unsigned char)*b) : 0;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst;
    while (*d) d++;
    while ((*d++ = *src++)) ;
    return dst;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}


/* ================================================================
 * Math
 * ================================================================ */

int abs(int x) { return x < 0 ? -x : x; }


/* ================================================================
 * 64-bit division (replaces libgcc __divdi3 / __udivdi3)
 * ================================================================ */

typedef unsigned long long uint64;
typedef long long int64;

uint64 __udivdi3(uint64 num, uint64 den)
{
    uint64 quot = 0, qbit = 1;
    if (den == 0) return 0; /* div by zero → 0 */
    while ((int64)den >= 0 && den < num) { den <<= 1; qbit <<= 1; }
    while (qbit) {
        if (num >= den) { num -= den; quot += qbit; }
        den >>= 1; qbit >>= 1;
    }
    return quot;
}

int64 __divdi3(int64 num, int64 den)
{
    int neg = 0;
    if (num < 0) { num = -num; neg = !neg; }
    if (den < 0) { den = -den; neg = !neg; }
    uint64 q = __udivdi3((uint64)num, (uint64)den);
    return neg ? -(int64)q : (int64)q;
}

uint64 __umoddi3(uint64 num, uint64 den)
{
    return num - __udivdi3(num, den) * den;
}

int64 __moddi3(int64 num, int64 den)
{
    return num - __divdi3(num, den) * den;
}


/* ================================================================
 * Malloc / free / realloc / calloc
 *
 * Simple first-fit allocator using _sbrk() for heap extension.
 * ================================================================ */

struct blk {
    uint32_t size;       /* data size (not including header) */
    uint32_t free;       /* 1 = free */
    struct blk *next;    /* next block in list */
};

#define BLK_HDR sizeof(struct blk)
#define ALIGN4(x) (((x) + 3) & ~3)

static struct blk *heap_list = NULL;

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    size = ALIGN4(size);

    /* First-fit search */
    struct blk *cur = heap_list;
    while (cur) {
        if (cur->free && cur->size >= size) {
            /* Split block if there's room for another block */
            if (cur->size >= size + BLK_HDR + 8) {
                struct blk *nb = (struct blk *)((uint8_t *)(cur + 1) + size);
                nb->size = cur->size - size - BLK_HDR;
                nb->free = 1;
                nb->next = cur->next;
                cur->size = size;
                cur->next = nb;
            }
            cur->free = 0;
            return (void *)(cur + 1);
        }
        cur = cur->next;
    }

    /* Extend heap */
    size_t total = BLK_HDR + size;
    /* Ask for at least 4 KB at a time to reduce sbrk calls */
    size_t ask = total < 4096 ? 4096 : total;
    struct blk *nb = (struct blk *)_sbrk(ask);
    if ((intptr_t)nb == -1) return NULL;
    nb->size = ask - BLK_HDR;
    nb->free = 0;
    nb->next = NULL;

    /* Append to list */
    if (!heap_list) {
        heap_list = nb;
    } else {
        cur = heap_list;
        while (cur->next) cur = cur->next;
        cur->next = nb;
    }

    /* Split off the remainder if large enough */
    if (nb->size >= size + BLK_HDR + 8) {
        struct blk *rem = (struct blk *)((uint8_t *)(nb + 1) + size);
        rem->size = nb->size - size - BLK_HDR;
        rem->free = 1;
        rem->next = nb->next;
        nb->size = size;
        nb->next = rem;
    }

    return (void *)(nb + 1);
}

void free(void *ptr)
{
    if (!ptr) return;
    struct blk *b = (struct blk *)ptr - 1;
    b->free = 1;

    /* Coalesce with next block if also free */
    if (b->next && b->next->free) {
        b->size += BLK_HDR + b->next->size;
        b->next = b->next->next;
    }
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    struct blk *b = (struct blk *)ptr - 1;
    if (b->size >= size) return ptr;  /* fits already */

    void *np = malloc(size);
    if (np) {
        memcpy(np, ptr, b->size);
        free(ptr);
    }
    return np;
}


/* ================================================================
 * POSIX wrappers  (DOOM calls open/read/write/...; libc_backend
 *                  provides _open/_read/_write/... )
 * ================================================================ */

int open(const char *pathname, int flags, ...)  { return _open(pathname, flags); }
int close(int fd)                               { return _close(fd); }
ssize_t read(int fd, void *buf, size_t n)      { return _read(fd, buf, n); }
ssize_t write(int fd, const void *buf, size_t n) { return _write(fd, buf, n); }
off_t lseek(int fd, off_t offset, int whence)  { return _lseek(fd, offset, whence); }
int stat(const char *path, void *sb)            { return _stat(path, sb); }
int fstat(int fd, void *sb)                     { return _fstat(fd, sb); }
int isatty(int fd)                              { return _isatty(fd); }


/* ================================================================
 * Stdio — minimal FILE-based I/O
 *
 * DOOM uses printf/fprintf for debug output and fopen for config
 * file access (which we don't support).  All output goes to UART.
 * ================================================================ */

/*
 * We cannot redefine FILE (newlib headers own that type), but we
 * never dereference FILE pointers — we just use the fd stored at
 * a known location.  For our fake FILE objects we store the fd
 * as the first int in the struct.
 */

/*
 * Fake FILE pool.
 *
 * newlib's feof/ferror are MACROS that access _flags at byte offset 12
 * of the real struct __sFILE (RV32 layout: _p[4] _r[4] _w[4] _flags[2]).
 * Our fake_file must match that layout so the macros work correctly.
 *
 * We store the POSIX fd in _file (offset 14), matching newlib's convention.
 *
 * NOTE: start.S goes to main() directly — no .init_array walk — so
 * constructors are never called.  Use static initializers instead.
 *
 * __SEOF (0x0020): newlib's "found EOF" flag, checked by feof() macro.
 */
#define __SEOF  0x0020

struct fake_file {
    void  *_p;      /* offset  0 (4B): buffer ptr    — unused, NULL      */
    int    _r;      /* offset  4 (4B): read space     — unused, 0         */
    int    _w;      /* offset  8 (4B): write space    — unused, 0         */
    short  _flags;  /* offset 12 (2B): stdio flags    — we set __SEOF here*/
    short  _file;   /* offset 14 (2B): POSIX fd       — our fd stored here*/
    char   pad[240];
};

static struct fake_file _ffiles[8] = {
    {NULL, 0, 0, 0, 0}, /* stdin:  fd=0 */
    {NULL, 0, 0, 0, 1}, /* stdout: fd=1 */
    {NULL, 0, 0, 0, 2}, /* stderr: fd=2 */
};
static int _ffiles_used = 3; /* entries 0-2 reserved for stdin/stdout/stderr */

static int _file_fd(void *fp)
{
    if (!fp) return 1; /* NULL → stdout */
    return (int)((struct fake_file *)fp)->_file;
}

/* --- printf family --- */

static char _printf_buf[256];

int printf(const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = mini_vsnprintf(_printf_buf, sizeof(_printf_buf), fmt, va);
    va_end(va);
    _write(1, _printf_buf, n);
    return n;
}

int fprintf(void *stream, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = mini_vsnprintf(_printf_buf, sizeof(_printf_buf), fmt, va);
    va_end(va);
    int fd = _file_fd(stream);
    _write(fd, _printf_buf, n);
    return n;
}

int vfprintf(void *stream, const char *fmt, va_list va)
{
    int n = mini_vsnprintf(_printf_buf, sizeof(_printf_buf), fmt, va);
    int fd = _file_fd(stream);
    _write(fd, _printf_buf, n);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = mini_vsnprintf(buf, 0x7fffffff, fmt, va);
    va_end(va);
    return n;
}

int snprintf(char *buf, size_t sz, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int n = mini_vsnprintf(buf, sz, fmt, va);
    va_end(va);
    return n;
}

/* --- sscanf (minimal: %d, %i, %x, %s, %c) --- */

static int _parse_int(const char **sp, int base)
{
    const char *s = *sp;
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; }
        else base = 10;
    }
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    int val = 0, got = 0;
    while (*s) {
        int d = -1;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        if (d < 0 || d >= base) break;
        val = val * base + d;
        got = 1;
        s++;
    }
    *sp = s;
    if (!got) return 0;
    return neg ? -val : val;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    int count = 0;
    const char *s = str;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == 'd' || *fmt == 'i') {
                int *p = va_arg(va, int *);
                *p = _parse_int(&s, (*fmt == 'd') ? 10 : 0);
                count++;
                fmt++;
            } else if (*fmt == 'x') {
                int *p = va_arg(va, int *);
                *p = _parse_int(&s, 16);
                count++;
                fmt++;
            } else if (*fmt == 's') {
                char *p = va_arg(va, char *);
                while (*s == ' ' || *s == '\t') s++;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n')
                    *p++ = *s++;
                *p = '\0';
                count++;
                fmt++;
            } else if (*fmt == 'c') {
                char *p = va_arg(va, char *);
                *p = *s++;
                count++;
                fmt++;
            } else {
                fmt++; /* skip unknown */
            }
        } else if (*fmt == ' ') {
            while (*s == ' ' || *s == '\t') s++;
            fmt++;
        } else {
            if (*s == *fmt) s++;
            fmt++;
        }
    }

    va_end(va);
    return count;
}

int fscanf(void *stream, const char *fmt, ...)
{
    (void)stream;
    (void)fmt;
    return 0;  /* stub — config file reads get defaults */
}

/* --- fopen / fclose / fflush / feof / setbuf --- */

void *fopen(const char *path, const char *mode)
{
    (void)mode;
    /* Config/save paths are not present; never open SDRAM WAD as a FILE. */
    if (!path || strcmp(path, "doom1.wad") != 0)
        return NULL;
    int fd = _open(path, 0);
    if (fd < 0) return NULL;

    if (_ffiles_used >= 8) { _close(fd); return NULL; }
    int idx = _ffiles_used++;
    struct fake_file *f = &_ffiles[idx];
    f->_p     = NULL;
    f->_r     = 0;
    f->_w     = 0;
    /* Always signal EOF — FILE*-based reading is not implemented.
     * WAD access uses POSIX open()/read() directly.  Setting __SEOF
     * here ensures M_LoadDefaults' while(!feof(f)) loop exits
     * immediately rather than spinning on the fscanf stub. */
    f->_flags = __SEOF;
    f->_file  = (short)fd;

    return f;
}

int fclose(void *stream)
{
    if (!stream) return -1;
    int fd = _file_fd(stream);
    return _close(fd);
}

int fflush(void *stream)
{
    (void)stream;
    return 0;
}

/* feof() is a macro in newlib's stdio.h that checks _flags & __SEOF.
 * We provide a function too in case anyone calls it without the header. */
int feof(void *stream)
{
    if (!stream) return 1;
    return (((struct fake_file *)stream)->_flags & __SEOF) ? 1 : 0;
}

void setbuf(void *stream, char *buf)
{
    (void)stream;
    (void)buf;
}


/* ================================================================
 * Program termination
 * ================================================================ */

void exit(int status)
{
    (void)status;
    /* Spin forever — _exit label in start.S also does this */
    while (1) ;
}


/* ================================================================
 * Misc stubs
 * ================================================================ */

int atoi(const char *s)
{
    const char *p = s;
    return _parse_int(&p, 10);
}

long strtol(const char *nptr, char **endptr, int base)
{
    const char *s = nptr;
    long val = _parse_int(&s, base);
    if (endptr) *endptr = (char *)s;
    return val;
}

/* getenv — DOOM checks for HOME etc. */
char *getenv(const char *name)
{
    (void)name;
    return NULL;
}
