#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/random.h>

extern "C" int getrandom(void *buf, size_t buflen, unsigned int flags)
{
    FILE* f = nullptr;

    // Do we want true random
    if (flags & GRND_RANDOM) {
        f  = fopen("/dev/random", "r");
    } else {
        f  = fopen("/dev/urandom", "r");
    }

    if (!f) {
	return -1;
    }

    // non blocking mode
    if (flags & GRND_NONBLOCK) {
        int d = fileno(f);
        // non blocking
        fcntl(d, F_SETFL, O_NONBLOCK);
        // no buffer
        setbuf(f, 0);
    }

    int ret = fread(buf, 1, buflen, f);

    if (fclose(f)) {
        return -1;
    }

    // Short non blocking read
    if (!ret && (flags & GRND_NONBLOCK)) {
        errno = EAGAIN;
        return -1;
    }

    return ret;
}
