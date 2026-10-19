/* virelai/libc/stdlib.c — conversions, a small mmap-backed allocator,
 * process glue. */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>

#include "../sys.h"

int errno;

/* ------------------------------------------------------------------ */
/* Numeric conversions                                                 */
/* ------------------------------------------------------------------ */

int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }

static int64_t strtol_i(const char *s, char **end, int base, int is_signed);

long strtol(const char *s, char **end, int base)
{
    return (long)strtol_i(s, end, base, 1);
}

unsigned long strtoul(const char *s, char **end, int base)
{
    return (unsigned long)strtol_i(s, end, base, 0);
}

static int digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

/* shared integer scanner: sign, optional 0x/0 prefix for base 0/16/8 */
static int64_t strtol_i(const char *s, char **end, int base, int is_signed)
{
    const char *p = s;
    int neg = 0;
    uint64_t v = 0;
    int any = 0;

    while (isspace((unsigned char)*p)) p++;
    if (is_signed && (*p == '+' || *p == '-')) {
        if (*p == '-') neg = 1;
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    for (;;) {
        int d = digit_val((unsigned char)*p);
        if (d < 0 || d >= base) break;
        v = v * (uint64_t)base + (uint64_t)d;
        p++;
        any = 1;
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0;
    }
    if (end) *end = (char *)p;
    if (neg) return -(int64_t)v;
    return (int64_t)v;
}

double strtod(const char *s, char **end)
{
    const char *p = s;
    double v = 0.0;
    double frac_scale = 1.0;
    int neg = 0, any = 0;
    int exp_neg = 0, exp_val = 0;

    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') { if (*p == '-') neg = 1; p++; }
    while (isdigit((unsigned char)*p)) {
        v = v * 10.0 + (*p - '0');
        p++; any = 1;
    }
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            frac_scale *= 0.1;
            v += frac_scale * (*p - '0');
            p++; any = 1;
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0.0;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') { if (*p == '-') exp_neg = 1; p++; }
        while (isdigit((unsigned char)*p)) {
            exp_val = exp_val * 10 + (*p - '0');
            p++;
        }
        while (exp_val-- > 0) v = exp_neg ? v / 10.0 : v * 10.0;
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

double atof(const char *s) { return strtod(s, NULL); }

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

/* ------------------------------------------------------------------ */
/* rand — LCG, deterministic                                            */
/* ------------------------------------------------------------------ */

static uint32_t rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 16) & 0x7fff);
}

void srand(unsigned seed) { rand_state = seed ? seed : 1; }

/* ------------------------------------------------------------------ */
/* exit                                                                */
/* ------------------------------------------------------------------ */

static void (*atexit_fns[32])(void);
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (atexit_count >= 32) return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int status)
{
    while (atexit_count > 0) atexit_fns[--atexit_count]();
    vl_exit(status);
}

void abort(void) { vl_exit(134); }

/* ------------------------------------------------------------------ */
/* environment / system                                                */
/* ------------------------------------------------------------------ */

char *getenv(const char *name)
{
    (void)name;
    return NULL;
}

int system(const char *cmd)
{
    (void)cmd;
    return -1;
}

/* ------------------------------------------------------------------ */
/* The heap: a first-fit free list grown by anonymous private mmap.    */
/*                                                                     */
/* Each mmap'd chunk is headed by a chunk descriptor; blocks inside a  */
/* chunk carry a header {size (bytes incl. header) | flags}. The free   */
/* list links free blocks. Split on alloc, coalesce-on-free within a   */
/* chunk.                                                              */
/* ------------------------------------------------------------------ */

#define VL_ALLOC_ALIGN 16

typedef struct blk {
    uint64_t size;   /* bytes, header included; low bits = flags         */
    struct blk *next; /* free-list link (valid while free)               */
} blk_t;

/* flag bits in blk.size */
#define BLK_FREE  1ULL
#define BLK_END   2ULL   /* final block of its chunk                     */

#define BLK_SIZE(b)  ((b)->size & ~(BLK_FREE | BLK_END))

typedef struct chunk {
    struct chunk *next;
    uint64_t bytes;  /* mmap length                                     */
} chunk_t;

static blk_t   *free_list;
static chunk_t *chunks;

static void *heap_grow(uint64_t need)
{
    uint64_t n = (need + sizeof(chunk_t) + 4095) & ~4095ULL;
    if (n < 65536) n = 65536;
    void *va = vl_mmap(0, n, VL_PROT_READ | VL_PROT_WRITE,
                       VL_MAP_ANONYMOUS | VL_MAP_PRIVATE);
    if (!va) return NULL;
    chunk_t *c = va;
    c->next = chunks;
    c->bytes = n;
    chunks = c;
    blk_t *b = (blk_t *)(c + 1);
    b->size = (n - sizeof(chunk_t)) | BLK_FREE | BLK_END;
    b->next = free_list;
    free_list = b;
    return b;
}

/* Coalesce a freed block with its free, address-adjacent successor within
 * the same chunk. Only forward merge is needed for a single bump region
 * plus tail block — two chained merges cover the general case poorly but
 * adequately here; the engine's allocation pattern is long-lived blocks. */
static void coalesce(blk_t *b)
{
    for (;;) {
        blk_t *nb = (blk_t *)((unsigned char *)b + BLK_SIZE(b));
        if ((b->size & BLK_END) || !(nb->size & BLK_FREE)) return;
        /* unlink nb from free list */
        blk_t **pp = &free_list;
        while (*pp && *pp != nb) pp = &(*pp)->next;
        if (*pp) *pp = nb->next;
        b->size = (BLK_SIZE(b) + BLK_SIZE(nb)) |
                  (b->size & BLK_FREE) | (nb->size & BLK_END);
    }
}

void *malloc(size_t nbytes)
{
    uint64_t need = (nbytes + sizeof(blk_t) + VL_ALLOC_ALIGN - 1) &
                    ~(uint64_t)(VL_ALLOC_ALIGN - 1);
    blk_t *prev = NULL, *b = free_list;

    while (b) {
        if (BLK_SIZE(b) >= need) goto found;
        prev = b;
        b = b->next;
    }
    if (!heap_grow(need)) return NULL;
    b = free_list;   /* new block sits at the head */
    prev = NULL;

found:
    if (BLK_SIZE(b) >= need + sizeof(blk_t) + VL_ALLOC_ALIGN) {
        /* split: b keeps `need` bytes; the remainder nb stays free in
         * b's list slot and inherits the chunk-end flag. */
        blk_t *nb = (blk_t *)((unsigned char *)b + need);
        nb->size = (BLK_SIZE(b) - need) | BLK_FREE | (b->size & BLK_END);
        nb->next = b->next;
        b->size = need;
        if (prev) prev->next = nb; else free_list = nb;
    } else {
        /* whole block: allocated, FREE cleared, END preserved. */
        b->size = BLK_SIZE(b) | (b->size & BLK_END);
        if (prev) prev->next = b->next; else free_list = b->next;
    }
    return b + 1;
}

void free(void *p)
{
    if (!p) return;
    blk_t *b = ((blk_t *)p) - 1;
    b->size |= BLK_FREE;
    b->next = free_list;
    free_list = b;
    coalesce(b);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t n = nmemb * size;
    if (nmemb && n / nmemb != size) return NULL;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (!n) { free(p); return NULL; }
    blk_t *b = ((blk_t *)p) - 1;
    uint64_t usable = BLK_SIZE(b) - sizeof(blk_t);
    if (n <= usable) return p;
    void *np = malloc(n);
    if (!np) return NULL;
    memcpy(np, p, usable);
    free(p);
    return np;
}

/* ------------------------------------------------------------------ */
/* assert                                                              */
/* ------------------------------------------------------------------ */

void __virelai_assert_fail(const char *expr, const char *file, int line)
{
    fprintf(stderr, "assert failed: %s (%s:%d)\n", expr, file, line);
    exit(134);
}
