/* virelai/libc/stdio.c — FILE over the vl_file_* seam plus printf/scanf.
 *
 * The kernel's file calls are sequential reads/writes through bounded
 * staging buffers (≤2048 B per call); there is no seek call. FILE is
 * therefore whole-file buffered: fopen loads the target fully into heap,
 * fseek/ftell/fread/fgets/fscanf run on memory, and write-mode flushes
 * the buffer back through chunked vl_file_write on fflush/fclose.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>

#include "../sys.h"

struct FILE {
    int     console;     /* fd 1/2: unbuffered vl_write sink             */
    int     writable;    /* buffer flushes back on fflush/fclose         */
    int     dirty;       /* buffer changed since load                    */
    int     eof;
    int     err;
    int     ungot;       /* -1 or a pushed-back byte                     */
    unsigned char *buf;
    size_t  cap;         /* allocated bytes                              */
    size_t  len;         /* valid bytes                                  */
    size_t  pos;         /* cursor                                       */
    char   *path;        /* owned copy — the write-back target           */
};

static FILE file_pool[24];

static FILE stdio_in  = { .console = 0 };
static FILE stdio_out = { .console = 1 };
static FILE stdio_err = { .console = 2 };

FILE *stdin  = &stdio_in;
FILE *stdout = &stdio_out;
FILE *stderr = &stdio_err;

static FILE *file_alloc(void)
{
    size_t i;
    for (i = 0; i < sizeof(file_pool) / sizeof(file_pool[0]); i++) {
        if (!file_pool[i].buf && !file_pool[i].console &&
            !file_pool[i].path) {
            memset(&file_pool[i], 0, sizeof(FILE));
            file_pool[i].ungot = -1;
            return &file_pool[i];
        }
    }
    return NULL;
}

/* Console output, chunked under the kernel's 256-byte write bound. */
static void console_emit(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        size_t take = n > VL_WRITE_MAX ? VL_WRITE_MAX : n;
        vl_write(fd, p, take);
        p += take;
        n -= take;
    }
}

static int file_grow(FILE *f, size_t want)
{
    if (want <= f->cap) return 0;
    size_t ncap = f->cap ? f->cap * 2 : 8192;
    while (ncap < want) ncap *= 2;
    unsigned char *nb = malloc(ncap);
    if (!nb) return -1;
    if (f->buf) memcpy(nb, f->buf, f->len);
    free(f->buf);
    f->buf = nb;
    f->cap = ncap;
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    FILE *f;
    int readable, writable, append;

    switch (*mode) {
    case 'r': readable = 1; writable = strchr(mode, '+') != NULL; append = 0; break;
    case 'w': readable = strchr(mode, '+') != NULL; writable = 1; append = 0; break;
    case 'a': readable = strchr(mode, '+') != NULL; writable = 1; append = 1; break;
    default: errno = EINVAL; return NULL;
    }

    f = file_alloc();
    if (!f) { errno = ENOMEM; return NULL; }
    f->writable = writable;
    f->path = strdup(path);
    if (!f->path) { errno = ENOMEM; return NULL; }

    if (readable || append) {
        /* Load the whole file into the buffer. */
        int64_t h = vl_file_open(path, strlen(path), VL_O_READ);
        if (h >= 0) {
            for (;;) {
                if (file_grow(f, f->len + VL_FILE_IO_MAX) != 0) {
                    vl_file_close((uint32_t)h);
                    free(f->path);
                    errno = ENOMEM;
                    return NULL;
                }
                int64_t r = vl_file_read((uint32_t)h, f->buf + f->len,
                                         VL_FILE_IO_MAX);
                if (r <= 0) break;
                f->len += (size_t)r;
            }
            vl_file_close((uint32_t)h);
        } else if (!writable) {
            free(f->path);
            errno = ENOENT;
            return NULL;
        }
        /* writable with a missing file: empty buffer, written back on
         * fclose/fflush (CREATE on the write side). */
    }
    if (append) f->pos = f->len;
    return f;
}

static int file_writeback(FILE *f)
{
    if (!f->dirty || !f->writable) return 0;
    int64_t h = vl_file_open(f->path, strlen(f->path),
                             VL_O_WRITE | VL_O_CREATE);
    if (h < 0) { f->err = 1; errno = EIO; return EOF; }
    size_t off = 0;
    while (off < f->len) {
        size_t take = f->len - off;
        if (take > VL_FILE_IO_MAX) take = VL_FILE_IO_MAX;
        int64_t w = vl_file_write((uint32_t)h, f->buf + off, take);
        if (w <= 0) {
            vl_file_close((uint32_t)h);
            f->err = 1;
            errno = EIO;
            return EOF;
        }
        off += (size_t)w;
    }
    vl_file_close((uint32_t)h);
    f->dirty = 0;
    return 0;
}

int fclose(FILE *f)
{
    int rc = 0;
    if (!f) return EOF;
    if (f->console) return 0;
    if (file_writeback(f) != 0) rc = EOF;
    free(f->buf);
    free(f->path);
    memset(f, 0, sizeof(*f));
    return rc;
}

int fflush(FILE *f)
{
    if (!f || f->console) return 0;
    return file_writeback(f);
}

/* ------------------------------------------------------------------ */
/* byte-level read/write on the buffer                                 */
/* ------------------------------------------------------------------ */

static int buf_getc(FILE *f)
{
    if (f->ungot >= 0) {
        int c = f->ungot;
        f->ungot = -1;
        return c;
    }
    if (f->pos >= f->len) { f->eof = 1; return EOF; }
    return f->buf[f->pos++];
}

static int buf_putc(FILE *f, int c)
{
    if (file_grow(f, f->pos + 1) != 0) { f->err = 1; return EOF; }
    f->buf[f->pos++] = (unsigned char)c;
    if (f->pos > f->len) f->len = f->pos;
    f->dirty = 1;
    return (unsigned char)c;
}

int fgetc(FILE *f) { return f->console ? EOF : buf_getc(f); }
int getc(FILE *f)  { return fgetc(f); }

int ungetc(int c, FILE *f)
{
    if (c == EOF) return EOF;
    f->ungot = c & 0xff;
    f->eof = 0;
    return c;
}

int fputc(int c, FILE *f)
{
    if (f->console) {
        unsigned char b = (unsigned char)c;
        console_emit(f->console, &b, 1);
        return c;
    }
    if (!f->writable) { f->err = 1; return EOF; }
    return buf_putc(f, c);
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c)       { return fputc(c, stdout); }

int fputs(const char *s, FILE *f)
{
    size_t n = strlen(s);
    if (f->console) {
        console_emit(f->console, (const unsigned char *)s, n);
        return 0;
    }
    if (!f->writable) { f->err = 1; return EOF; }
    if (fwrite(s, 1, n, f) != n) return EOF;
    return 0;
}

int puts(const char *s)
{
    size_t n = strlen(s);
    console_emit(1, (const unsigned char *)s, n);
    console_emit(1, (const unsigned char *)"\n", 1);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t want, have;
    if (f->console || size == 0) return 0;
    want = size * nmemb;
    have = f->len - f->pos;
    if (want > have) want = have;
    if (want) {
        memcpy(ptr, f->buf + f->pos, want);
        f->pos += want;
    }
    if (f->pos >= f->len) f->eof = 1;
    return want / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t n = size * nmemb;
    if (f->console) {
        console_emit(f->console, ptr, n);
        return nmemb;
    }
    if (!f->writable) { f->err = 1; return 0; }
    if (file_grow(f, f->pos + n) != 0) { f->err = 1; return 0; }
    memcpy(f->buf + f->pos, ptr, n);
    f->pos += n;
    if (f->pos > f->len) f->len = f->pos;
    f->dirty = 1;
    return nmemb;
}

int fseek(FILE *f, long offset, int whence)
{
    long base, np;
    if (f->console) return -1;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (long)f->pos; break;
    case SEEK_END: base = (long)f->len; break;
    default: errno = EINVAL; return -1;
    }
    np = base + offset;
    if (np < 0) { errno = EINVAL; return -1; }
    f->pos = (size_t)np;
    f->eof = 0;
    f->ungot = -1;
    return 0;
}

long ftell(FILE *f)
{
    if (f->console) return -1;
    return (long)f->pos;
}

void rewind(FILE *f)
{
    f->pos = 0;
    f->eof = 0;
    f->err = 0;
    f->ungot = -1;
}

int feof(FILE *f)      { return f->eof; }
int ferror(FILE *f)    { return f->err; }
void clearerr(FILE *f) { f->eof = 0; f->err = 0; }
int fileno(FILE *f)    { return f->console; }

char *fgets(char *s, int size, FILE *f)
{
    int i = 0;
    if (size <= 0) return NULL;
    while (i < size - 1) {
        int c = buf_getc(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}

int remove(const char *path)
{
    int64_t r = vl_file_delete(path, strlen(path));
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

int rename(const char *oldp, const char *newp)
{
    int64_t r = vl_file_rename(oldp, strlen(oldp), newp, strlen(newp));
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
}

void perror(const char *s)
{
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errno));
}

/* ------------------------------------------------------------------ */
/* printf family                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    FILE  *file;      /* NULL → bounded buffer                          */
} emit_t;

static void emit_ch(emit_t *e, char c)
{
    if (e->file) {
        fputc(c, e->file);
    } else if (e->len + 1 < e->cap) {
        e->buf[e->len] = c;
    }
    e->len++;
}

static void emit_str(emit_t *e, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) emit_ch(e, s[i]);
}

static void emit_num(emit_t *e, uint64_t v, int neg, int base, int upper,
                     int width, int prec, char pad, int alt)
{
    char tmp[66];
    int nd = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char prefix[2];
    int plen = 0;

    do {
        tmp[nd++] = digits[v % (uint64_t)base];
        v /= (uint64_t)base;
    } while (v && nd < 66);

    if (alt && base == 16) { prefix[plen++] = '0'; prefix[plen++] = upper ? 'X' : 'x'; }
    if (alt && base == 8)  { prefix[plen++] = '0'; }

    int zeros = prec > nd ? prec - nd : 0;
    int body = nd + zeros + plen + (neg ? 1 : 0);
    int padlen = width > body ? width - body : 0;

    if (pad == '0' && prec < 0) {
        if (neg) emit_ch(e, '-');
        emit_str(e, prefix, (size_t)plen);
        while (padlen-- > 0) emit_ch(e, '0');
    } else {
        while (padlen-- > 0) emit_ch(e, ' ');
        if (neg) emit_ch(e, '-');
        emit_str(e, prefix, (size_t)plen);
    }
    while (zeros-- > 0) emit_ch(e, '0');
    while (nd > 0) emit_ch(e, tmp[--nd]);
}

static void emit_double(emit_t *e, double v, int prec)
{
    if (prec < 0) prec = 6;
    if (v < 0) { emit_ch(e, '-'); v = -v; }
    uint64_t ip = (uint64_t)v;
    emit_num(e, ip, 0, 10, 0, 0, -1, ' ', 0);
    emit_ch(e, '.');
    double frac = v - (double)ip;
    while (prec-- > 0) {
        frac *= 10.0;
        int d = (int)frac;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        frac -= (double)d;
        emit_ch(e, (char)('0' + d));
    }
}

/* Core formatter: %[-0# +]*[0-9]*[.*]*[hlLzjt]*[diuxXocspeEfFgG%] */
static int vformat(emit_t *e, const char *fmt, va_list ap)
{
    const char *p = fmt;

    while (*p) {
        if (*p != '%') { emit_ch(e, *p++); continue; }
        p++;
        if (!*p) break;
        if (*p == '%') { emit_ch(e, '%'); p++; continue; }

        int left = 0, alt = 0;
        char pad = ' ';
        int width = 0, prec = -1;
        int longcnt = 0;   /* 0 int, -1 h, 1 l, 2 ll/z/j/t                */

        for (;;) {
            if (*p == '-') left = 1;
            else if (*p == '0') pad = '0';
            else if (*p == '#') alt = 1;
            else if (*p == '+' || *p == ' ') { /* sign flags: fold */ }
            else break;
            p++;
        }
        while (isdigit((unsigned char)*p)) width = width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { p++; prec = va_arg(ap, int); }
            else while (isdigit((unsigned char)*p)) prec = prec * 10 + (*p++ - '0');
        } else if (*p == '*') {
            p++;
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
        }
        for (;;) {
            if (*p == 'l') { longcnt++; p++; }
            else if (*p == 'z' || *p == 'j' || *p == 't') { longcnt = 2; p++; }
            else if (*p == 'h') { longcnt = -1; p++; }
            else break;
        }
        if (longcnt > 2) longcnt = 2;

        char spec = *p ? *p++ : 0;
        int64_t sv;
        uint64_t uv;
        int neg;

        switch (spec) {
        case 'd': case 'i':
            sv = longcnt ? va_arg(ap, long long) : va_arg(ap, int);
            neg = sv < 0;
            uv = neg ? (uint64_t)(-sv) : (uint64_t)sv;
            emit_num(e, uv, neg, 10, 0, width, prec, pad, 0);
            break;
        case 'u':
            uv = longcnt ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            emit_num(e, uv, 0, 10, 0, width, prec, pad, 0);
            break;
        case 'x': case 'X':
            uv = longcnt ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            emit_num(e, uv, 0, 16, spec == 'X', width, prec, pad, alt);
            break;
        case 'o':
            uv = longcnt ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned int);
            emit_num(e, uv, 0, 8, 0, width, prec, pad, alt);
            break;
        case 'p':
            uv = (uint64_t)(uintptr_t)va_arg(ap, void *);
            emit_num(e, uv, 0, 16, 0, width, prec, pad, 1);
            break;
        case 'c':
            emit_ch(e, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            size_t n;
            int padlen;
            if (!s) s = "(null)";
            n = strlen(s);
            if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            padlen = width - (int)n;
            if (!left) while (padlen-- > 0) emit_ch(e, ' ');
            emit_str(e, s, n);
            if (left) while (padlen-- > 0) emit_ch(e, ' ');
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            emit_double(e, va_arg(ap, double), prec);
            break;
        default:
            emit_ch(e, '%');
            if (spec) emit_ch(e, spec);
            break;
        }
    }
    return (int)e->len;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    emit_t e = { .buf = buf, .cap = n, .len = 0, .file = NULL };
    int r = vformat(&e, fmt, ap);
    if (n) buf[e.len < n ? e.len : n - 1] = 0;
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, ~(size_t)0, fmt, ap);
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(buf, ~(size_t)0, fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    emit_t e = { .buf = NULL, .cap = 0, .len = 0, .file = f };
    return vformat(&e, fmt, ap);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

/* ------------------------------------------------------------------ */
/* scanf family — %d %i %u %x %o %s %c %f %[^set] with widths           */
/* ------------------------------------------------------------------ */

static int scan_digit_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct {
    FILE *file;
    const char *s;    /* string source when file == NULL                */
    int    consumed;
} scan_t;

static int scan_getc(scan_t *sc)
{
    int c;
    if (sc->file) {
        c = buf_getc(sc->file);
        if (c != EOF) sc->consumed++;
        return c;
    }
    if (!*sc->s) return EOF;
    sc->consumed++;
    return (unsigned char)*sc->s++;
}

static void scan_ungetc(scan_t *sc, int c)
{
    if (c == EOF) return;
    if (sc->file) {
        sc->file->ungot = c & 0xff;
        sc->file->eof = 0;
    } else {
        sc->s--;
    }
    sc->consumed--;
}

static void scan_skip_ws(scan_t *sc)
{
    int c;
    for (;;) {
        c = scan_getc(sc);
        if (c == EOF || !isspace(c)) break;
    }
    scan_ungetc(sc, c);
}

static int vscan(scan_t *sc, const char *fmt, va_list ap)
{
    int converted = 0;
    const char *p = fmt;

    while (*p) {
        if (isspace((unsigned char)*p)) {
            scan_skip_ws(sc);
            p++;
            continue;
        }
        if (*p != '%') {
            int c = scan_getc(sc);
            if (c != (unsigned char)*p) { scan_ungetc(sc, c); goto done; }
            p++;
            continue;
        }
        p++;
        {
            int suppress = 0, width = 0;
            int longcnt = 0;
            char spec;

            if (*p == '*') { suppress = 1; p++; }
            while (isdigit((unsigned char)*p))
                width = width * 10 + (*p++ - '0');
            if (!width) width = 1 << 20;
            for (;;) {
                if (*p == 'l') { longcnt++; p++; }
                else if (*p == 'h') { longcnt = -1; p++; }
                else break;
            }
            spec = *p ? *p++ : 0;
            if (!spec) goto done;

            if (spec == 'c') {
                int c = scan_getc(sc);
                if (c == EOF) goto done;
                if (!suppress) {
                    *va_arg(ap, char *) = (char)c;
                    converted++;
                }
                continue;
            }
            if (spec == 's' || spec == '[') {
                char *out = suppress ? NULL : va_arg(ap, char *);
                char set[256];
                int invert = 0, n = 0;
                if (spec == '[') {
                    memset(set, 0, sizeof(set));
                    if (*p == '^') { invert = 1; p++; }
                    if (*p == ']') { set[']'] = 1; p++; }
                    while (*p && *p != ']') set[(unsigned char)*p++] = 1;
                    if (*p == ']') p++;
                }
                scan_skip_ws(sc);
                for (;;) {
                    int c = scan_getc(sc);
                    int ok;
                    if (c == EOF) break;
                    ok = (spec == 's') ? !isspace(c)
                                       : (invert ? !set[c & 0xff]
                                                 : set[c & 0xff]);
                    if (!ok || n >= width) { scan_ungetc(sc, c); break; }
                    if (out) out[n] = (char)c;
                    n++;
                }
                if (n == 0) goto done;
                if (out) { out[n] = 0; converted++; }
                continue;
            }
            if (spec == 'n') {
                if (!suppress) *va_arg(ap, int *) = sc->consumed;
                continue;
            }
            if (spec == 'f' || spec == 'e' || spec == 'g') {
                char num[64];
                int n = 0, any = 0;
                int c;
                scan_skip_ws(sc);
                c = scan_getc(sc);
                if (c == '+' || c == '-') {
                    num[n++] = (char)c;
                    c = scan_getc(sc);
                }
                while (isdigit(c)) {
                    if (n < 63) num[n++] = (char)c;
                    c = scan_getc(sc); any = 1;
                }
                if (c == '.') {
                    if (n < 63) num[n++] = (char)c;
                    c = scan_getc(sc);
                    while (isdigit(c)) {
                        if (n < 63) num[n++] = (char)c;
                        c = scan_getc(sc); any = 1;
                    }
                }
                scan_ungetc(sc, c);
                if (!any) goto done;
                num[n] = 0;
                if (!suppress) {
                    double v = strtod(num, NULL);
                    if (longcnt > 0) *va_arg(ap, double *) = v;
                    else             *va_arg(ap, float *) = (float)v;
                    converted++;
                }
                continue;
            }

            /* integer specs */
            {
                int base = 10, is_signed = 1;
                char num[64];
                int n = 0, any = 0;
                int c;

                switch (spec) {
                case 'd': case 'u': base = 10; break;
                case 'i': base = 0; break;
                case 'x': case 'X': base = 16; is_signed = 0; break;
                case 'o': base = 8; is_signed = 0; break;
                case 'p': base = 16; is_signed = 0; break;
                default: goto done;
                }
                scan_skip_ws(sc);
                c = scan_getc(sc);
                if (is_signed && (c == '+' || c == '-')) {
                    num[n++] = (char)c;
                    c = scan_getc(sc);
                }
                if (base == 16 && c == '0') {
                    int c2 = scan_getc(sc);
                    if (c2 == 'x' || c2 == 'X') {
                        num[n++] = '0'; num[n++] = 'x';
                        c = scan_getc(sc);
                    } else {
                        scan_ungetc(sc, c2);
                    }
                }
                while (n < width) {
                    int dv = scan_digit_val(c);
                    if (dv < 0 || dv >= base) break;
                    if (n < 63) num[n++] = (char)c;
                    c = scan_getc(sc);
                    any = 1;
                }
                scan_ungetc(sc, c);
                if (!any) goto done;
                num[n] = 0;
                if (!suppress) {
                    long v = strtol(num, NULL, base);
                    if (spec == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                    else if (longcnt > 0) *va_arg(ap, long *) = v;
                    else if (longcnt < 0) *va_arg(ap, short *) = (short)v;
                    else *va_arg(ap, int *) = (int)v;
                    converted++;
                }
            }
        }
    }
done:
    return converted;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    scan_t sc = { .file = NULL, .s = str, .consumed = 0 };
    int r;
    va_start(ap, fmt);
    r = vscan(&sc, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    scan_t sc = { .file = f, .s = NULL, .consumed = 0 };
    int r;
    va_start(ap, fmt);
    r = vscan(&sc, fmt, ap);
    va_end(ap);
    return r;
}
