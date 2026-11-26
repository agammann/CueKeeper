#include "cuekeeper.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#define CK_TIMER_PULSE _PULSE_CODE_MINAVAIL

_Static_assert(CK_REQUEST_TYPE > _IO_MAX, "Use an application message type");

typedef union {
    struct _pulse pulse;
    struct {
        uint16_t type;
        uint16_t subtype;
    } connect;
    CkRequest request;
} ReceiveBuffer;

static int arm_scene_timer(timer_t timer, const CkScene *scene)
{
    int64_t deadline_ns;
    if (!ck_scene_next_deadline(scene, &deadline_ns)) {
        return 0;
    }

    const struct itimerspec expiration = {
        .it_value = {
            .tv_sec = (time_t)(deadline_ns / CK_NSEC_PER_SEC),
            .tv_nsec = (long)(deadline_ns % CK_NSEC_PER_SEC),
        },
    };
    return timer_settime(timer, TIMER_ABSTIME, &expiration, NULL);
}

static void serve_request(rcvid_t rcvid, const ReceiveBuffer *message,
                          const struct _msg_info *info, timer_t timer, CkScene *scene)
{
    if (info->msglen < (int64_t)sizeof(message->connect.type)) {
        (void)MsgError(rcvid, EMSGSIZE);
        return;
    }
    if (message->connect.type == _IO_CONNECT &&
        info->msglen >= (int64_t)sizeof(message->connect) &&
        message->connect.subtype == _IO_CONNECT_OPEN) {
        (void)MsgReply(rcvid, EOK, NULL, 0);
        return;
    }
    if (message->request.type != CK_REQUEST_TYPE) {
        (void)MsgError(rcvid, ENOSYS);
        return;
    }
    if (info->msglen != (int64_t)sizeof(CkRequest) ||
        info->srcmsglen != (int64_t)sizeof(CkRequest) ||
        info->dstmsglen < (int64_t)sizeof(CkReply)) {
        (void)MsgError(rcvid, EMSGSIZE);
        return;
    }

    uint32_t result = CK_RESULT_INVALID;
    const CkRequest *request = &message->request;
    if (request->version == CK_PROTOCOL_VERSION && request->command == CK_CMD_STATUS) {
        result = CK_RESULT_OK;
    } else if (request->version == CK_PROTOCOL_VERSION && request->command == CK_CMD_GO) {
        int64_t now_ns;
        if (ck_now_ns(&now_ns) == -1) {
            const int error = errno;
            perror("clock_gettime");
            (void)MsgError(rcvid, error);
            return;
        }

        // Publish the new run only after the first timer has been armed.
        CkScene next_scene = *scene;
        if (!ck_scene_start(&next_scene, now_ns)) {
            result = CK_RESULT_ACTIVE;
        } else {
            if (arm_scene_timer(timer, &next_scene) == -1) {
                const int error = errno;
                perror("timer_settime");
                (void)MsgError(rcvid, error);
                return;
            }
            *scene = next_scene;
            result = CK_RESULT_OK;
        }
    }

    CkReply reply;
    ck_scene_snapshot(scene, result, &reply);
    if (MsgReply(rcvid, EOK, &reply, sizeof(reply)) == -1 && errno != ESRCH) {
        const int error = errno;
        perror("MsgReply");
        (void)MsgError(rcvid, error);
    }
}

static void run_engine(int channel, timer_t timer)
{
    CkScene scene = {0};
    for (;;) {
        ReceiveBuffer message;
        struct _msg_info info;
        const rcvid_t rcvid = MsgReceive(channel, &message, sizeof(message), &info);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("MsgReceive");
            return;
        }
        if (rcvid > 0) {
            serve_request(rcvid, &message, &info, timer, &scene);
            continue;
        }

        switch (message.pulse.code) {
        case CK_TIMER_PULSE: {
            int64_t now_ns;
            if (ck_now_ns(&now_ns) == -1 || ck_scene_advance(&scene, now_ns) == -1) {
                perror("clock_gettime");
                return;
            }
            if (arm_scene_timer(timer, &scene) == -1) {
                perror("timer_settime");
                return;
            }
            break;
        }
        case _PULSE_CODE_DISCONNECT:
            if (ConnectDetach(message.pulse.scoid) == -1) {
                perror("ConnectDetach (client)");
            }
            break;
        default:
            // Requests receive their replies inline, so no unblock work is pending.
            break;
        }
    }
}

int main(void)
{
    name_attach_t *service = name_attach(NULL, CK_SERVICE, 0);
    if (service == NULL) {
        perror("name_attach " CK_SERVICE);
        return EXIT_FAILURE;
    }

    const int timer_connection = ConnectAttach(0, 0, service->chid, _NTO_SIDE_CHANNEL,
                                                _NTO_COF_CLOEXEC);
    if (timer_connection == -1) {
        perror("ConnectAttach (timer)");
        (void)name_detach(service, 0);
        return EXIT_FAILURE;
    }

    struct sigevent event = {0};
    SIGEV_PULSE_INIT(&event, timer_connection, SIGEV_PULSE_PRIO_INHERIT, CK_TIMER_PULSE, 0);

    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) == -1) {
        perror("timer_create");
    } else {
        printf("READY\t%s\n", CK_SERVICE);
        (void)fflush(stdout);
        run_engine(service->chid, timer);
        (void)timer_delete(timer);
    }
    (void)ConnectDetach(timer_connection);
    (void)name_detach(service, 0);
    return EXIT_FAILURE;
}
