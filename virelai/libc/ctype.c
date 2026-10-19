/* virelai/libc/ctype.c — ASCII ctype, table-free. */

#include <ctype.h>

int isalpha(int c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isprint(int c)  { return c >= 0x20 && c <= 0x7e; }
int iscntrl(int c)  { return (c >= 0 && c < 0x20) || c == 0x7f; }
int isxdigit(int c) { return isdigit(c) ||
                      (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isgraph(int c)  { return c > 0x20 && c <= 0x7e; }
int ispunct(int c)  { return isgraph(c) && !isalnum(c); }
int toupper(int c)  { return islower(c) ? c - ('a' - 'A') : c; }
int tolower(int c)  { return isupper(c) ? c + ('a' - 'A') : c; }
