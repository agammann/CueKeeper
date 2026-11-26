#ifndef LINUX_SOCKET_H
#define LINUX_SOCKET_H

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

static inline int ck_linux_socket_address(struct sockaddr_un *address)
{
    const char *path = getenv("CUEKEEPER_SOCKET");
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strlen(path) >= sizeof(address->sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    *address = (struct sockaddr_un){.sun_family = AF_UNIX};
    strcpy(address->sun_path, path);
    return 0;
}

#endif
