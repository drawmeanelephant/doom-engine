#ifndef VIRELAI_SYS_STAT_H
#define VIRELAI_SYS_STAT_H

#include <sys/types.h>

struct stat {
    off_t   st_size;
    mode_t  st_mode;
};

#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_ISDIR(m) (((m) & S_IFDIR) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFREG) == S_IFREG)

int mkdir(const char *path, mode_t mode);
int stat(const char *path, struct stat *st);

#endif
