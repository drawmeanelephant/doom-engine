/* virelai/libc/string.c — byte/string ops, freestanding. */

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d < (uintptr_t)s) {
        while (n--) *d++ = *s++;
    } else if ((uintptr_t)d > (uintptr_t)s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++; q++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    size_t i;
    for (i = 0; i < n && src[i]; i++) *d++ = src[i];
    *d = 0;
    return dst;
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

static int ci_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && ci_lower((unsigned char)*a) == ci_lower((unsigned char)*b)) {
        a++; b++;
    }
    return ci_lower((unsigned char)*a) - ci_lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a &&
           ci_lower((unsigned char)*a) == ci_lower((unsigned char)*b)) {
        a++; b++; n--;
    }
    return n ? ci_lower((unsigned char)*a) - ci_lower((unsigned char)*b) : 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return (char *)hay;
    for (; *hay; hay++) {
        if (!strncmp(hay, needle, nl)) return (char *)hay;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t l = strnlen(s, n);
    char *p = malloc(l + 1);
    if (p) { memcpy(p, s, l); p[l] = 0; }
    return p;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        if (strchr(accept, *s)) return (char *)s;
    }
    return NULL;
}

static char *strtok_save;

char *strtok(char *s, const char *delim)
{
    char *tok;
    if (!s) s = strtok_save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { strtok_save = s; return NULL; }
    tok = s;
    s = strpbrk(s, delim);
    if (s) *s++ = 0;
    strtok_save = s;
    return tok;
}

static const char *const err_names[] = {
    "no error", "EPERM", "ENOENT", "ESRCH", "EINTR", "EIO", "ENXIO",
    "E2BIG", "ENOEXEC", "EBADF", "ECHILD", "EAGAIN", "ENOMEM",
    "EACCES", "EFAULT", "EBUSY", "EEXIST", "ENODEV", "ENOTDIR",
    "EISDIR", "EINVAL",
};

char *strerror(int errnum)
{
    static char unk[24];
    if (errnum >= 0 && errnum < (int)(sizeof(err_names) / sizeof(err_names[0])))
        return (char *)err_names[errnum];
    snprintf(unk, sizeof(unk), "errno %d", errnum);
    return unk;
}
