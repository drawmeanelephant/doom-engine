#ifndef VIRELAI_UNISTD_H
#define VIRELAI_UNISTD_H

#include <stddef.h>

int isatty(int fd);
int access(const char *path, int mode);
int unlink(const char *path);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned long usec);

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

#endif
