#ifndef VIRELAI_STDLIB_H
#define VIRELAI_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void   *malloc(size_t n);
void   *calloc(size_t nmemb, size_t size);
void   *realloc(void *p, size_t n);
void    free(void *p);

int     atoi(const char *s);
long    atol(const char *s);
double  atof(const char *s);
long    strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double  strtod(const char *s, char **end);

int     abs(int x);
long    labs(long x);

int     rand(void);
void    srand(unsigned seed);

void    exit(int status);
void    abort(void);
int     atexit(void (*fn)(void));

char   *getenv(const char *name);
int     system(const char *cmd);

#endif
