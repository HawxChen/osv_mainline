#ifndef COMPAT_LINUX_RANDOM_H
#define COMPAT_LINUX_RANDOM_H

#include <sys/types.h>

extern "C" {
int getrandom(void *buf, size_t buflen, unsigned int flags);
}

#define GRND_RANDOM	0x0002
#define GRND_NONBLOCK   0x0001

#endif
