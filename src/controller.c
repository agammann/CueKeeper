#include "cuekeeper.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __QNXNTO__
#include <sys/dispatch.h>
#include <sys/neutrino.h>
#else
#include "linux_socket.h"
#include <sys/time.h>
#include <unistd.h>
#endif

static int open_engine(void)
{
#ifdef __QNXNTO__
    return name_open(CK_SERVICE, _NAME_FLAG_COF_CLOEXEC | _NAME_FLAG_COF_NOEVENT);
#else
    struct sockaddr_un address;
    if (ck_linux_socket_address(&address) == -1) {
        return -1;
    }
    int client = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (client == -1) {
        return -1;
    }

    /* Linux autobinds a unique reply address when only the family is supplied. */
    struct sockaddr_un local = {.sun_family = AF_UNIX};
    struct timeval timeout = {.tv_sec = 2};
    if (bind(client, (struct sockaddr *)&local, sizeof(local.sun_family)) == -1 ||
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1 ||
        connect(client, (struct sockaddr *)&address, sizeof(address)) == -1) {
        int saved_errno = errno;
        close(client);
        errno = saved_errno;
        return -1;
    }
    return client;
#endif
}

static void close_engine(int engine)
{
#ifdef __QNXNTO__
    name_close(engine);
#else
    close(engine);
#endif
}

static int exchange(int engine, const CkRequest *request, CkReply *reply)
{
#ifdef __QNXNTO__
    return MsgSend(engine, request, sizeof(*request), reply, sizeof(*reply));
#else
    if (send(engine, request, sizeof(*request), 0) == -1) {
        return -1;
    }
    ssize_t received = recv(engine, reply, sizeof(*reply), MSG_TRUNC);
    if (received == -1) {
        return -1;
    }
    if (received != sizeof(*reply)) {
        errno = EPROTO;
        return -1;
    }
    return 0;
#endif
}

static int command(int engine, const char *line)
{
    CkRequest request = {.type = CK_REQUEST_TYPE, .version = CK_PROTOCOL_VERSION};
    if (strcmp(line, "go") == 0) {
        request.command = CK_CMD_GO;
    } else if (strcmp(line, "status") == 0) {
        request.command = CK_CMD_STATUS;
    } else {
        fprintf(stderr, "Expected go, status, or quit.\n");
        return 1;
    }

    CkReply reply = {0};
    if (exchange(engine, &request, &reply) == -1) {
        perror("cuekeeperctl request");
        return 1;
    }
    const char *const states[] = {"idle", "active", "complete"};
    if (reply.version != CK_PROTOCOL_VERSION || reply.state >= sizeof(states) / sizeof(states[0]) ||
        reply.history_count > CK_CUE_COUNT) {
        fprintf(stderr, "Invalid reply from cuekeeper-engine.\n");
        return 1;
    }
    if (reply.result == CK_RESULT_ACTIVE) {
        fprintf(stderr, "scene is already active\n");
        return 2;
    }
    if (reply.result != CK_RESULT_OK) {
        fprintf(stderr, "cuekeeper-engine rejected the request.\n");
        return 1;
    }

    printf("scene=%s state=%s duration_ms=%d history_count=%u\n", CK_SCENE_NAME,
           states[reply.state], CK_SCENE_DURATION_MS, (unsigned)reply.history_count);
    for (uint32_t i = 0; i < reply.history_count; ++i) {
        const CkCueRecord *record = &reply.history[i];
        printf("cue=%.*s scheduled_ms=%.0f actual_ms=%.3f lateness_ms=%.3f\n",
               CK_CUE_ID_SIZE, record->id, (double)record->scheduled_ns / CK_NSEC_PER_MSEC,
               (double)record->actual_ns / CK_NSEC_PER_MSEC,
               (double)record->lateness_ns / CK_NSEC_PER_MSEC);
    }
    fflush(stdout);
    return 0;
}

static int interactive(int engine)
{
    char line[32];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *newline = strchr(line, '\n');
        if (newline == NULL && !feof(stdin)) {
            int ch;
            do {
                ch = getchar();
            } while (ch != '\n' && ch != EOF);
            fprintf(stderr, "Command is too long.\n");
            continue;
        }
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "quit") == 0) {
            break;
        }
        if (line[0] != '\0') {
            command(engine, line);
        }
    }
    return ferror(stdin) ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "go") != 0 && strcmp(argv[1], "status") != 0)) {
        fprintf(stderr, "Usage: %s [go|status]\n", argv[0]);
        return 1;
    }

    int engine = open_engine();
    if (engine == -1) {
        perror("cuekeeperctl connect");
        return 1;
    }
    int result = argc == 2 ? command(engine, argv[1]) : interactive(engine);
    close_engine(engine);
    return result;
}
