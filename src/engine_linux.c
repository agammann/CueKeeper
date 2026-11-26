/* Linux smoke-test adapter. The QNX build uses engine_qnx.c. */
#include "cuekeeper.h"
#include "linux_socket.h"

#include <poll.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

static int arm_scene_timer(int timer, const CkScene *scene)
{
    int64_t deadline;
    if (!ck_scene_next_deadline(scene, &deadline)) {
        return 0;
    }
    struct itimerspec expiration = {
        .it_value = {.tv_sec = deadline / CK_NSEC_PER_SEC, .tv_nsec = deadline % CK_NSEC_PER_SEC},
    };
    return timerfd_settime(timer, TFD_TIMER_ABSTIME, &expiration, NULL);
}

static int serve_request(int server, int timer, CkScene *scene)
{
    CkRequest request;
    struct sockaddr_un client;
    socklen_t client_size = sizeof(client);
    ssize_t received = recvfrom(server, &request, sizeof(request), MSG_TRUNC,
                                (struct sockaddr *)&client, &client_size);
    if (received == -1) {
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    }
    if (received != sizeof(request) || client_size <= sizeof(client.sun_family) ||
        client_size > sizeof(client)) {
        return 0;
    }

    uint32_t result = CK_RESULT_INVALID;
    if (request.type == CK_REQUEST_TYPE && request.version == CK_PROTOCOL_VERSION) {
        if (request.command == CK_CMD_STATUS) {
            result = CK_RESULT_OK;
        } else if (request.command == CK_CMD_GO) {
            int64_t now;
            if (ck_now_ns(&now) == -1) {
                return -1;
            }
            CkScene next = *scene;
            if (ck_scene_start(&next, now)) {
                if (arm_scene_timer(timer, &next) == -1) {
                    return -1;
                }
                *scene = next;
                result = CK_RESULT_OK;
            } else {
                result = CK_RESULT_ACTIVE;
            }
        }
    }

    CkReply reply;
    ck_scene_snapshot(scene, result, &reply);
    /* A disappeared or stalled controller can't block the timeline. */
    (void)sendto(server, &reply, sizeof(reply), MSG_DONTWAIT, (struct sockaddr *)&client, client_size);
    return 0;
}

static void run_engine(int timer, int server)
{
    CkScene scene = {0};
    struct pollfd sources[] = {{.fd = timer, .events = POLLIN}, {.fd = server, .events = POLLIN}};
    for (;;) {
        if (poll(sources, 2, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            return;
        }
        if (sources[0].revents & POLLIN) {
            uint64_t expirations;
            if (read(timer, &expirations, sizeof(expirations)) == -1) {
                if (errno == EINTR) {
                    continue;
                }
                perror("read timerfd");
                return;
            }
            int64_t now;
            if (ck_now_ns(&now) == -1 || ck_scene_advance(&scene, now) == -1 ||
                arm_scene_timer(timer, &scene) == -1) {
                perror("advance timer");
                return;
            }
        }
        if ((sources[1].revents & POLLIN) && serve_request(server, timer, &scene) == -1) {
            perror("serve request");
            return;
        }
    }
}

int main(void)
{
    struct sockaddr_un address;
    if (ck_linux_socket_address(&address) == -1) {
        perror("CUEKEEPER_SOCKET");
        return EXIT_FAILURE;
    }
    int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    if (timer == -1) {
        perror("timerfd_create");
        return EXIT_FAILURE;
    }
    int server = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (server == -1) {
        perror("socket");
        close(timer);
        return EXIT_FAILURE;
    }
    umask(0077);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("bind CUEKEEPER_SOCKET");
    } else {
        printf("READY\t%s\n", CK_SERVICE);
        fflush(stdout);
        run_engine(timer, server);
        unlink(address.sun_path);
    }
    close(server);
    close(timer);
    return EXIT_FAILURE;
}
