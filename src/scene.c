#include "cuekeeper.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    char id[CK_CUE_ID_SIZE];
    int milliseconds;
    const char *output;
    const char *value;
} Cue;

static const Cue cues[CK_CUE_COUNT] = {
    {"lx-1", 5000, "lighting", "Stage to blue"},
    {"snd-1", 15000, "sound", "Play opening chime"},
    {"cap-1", 25000, "caption", "Welcome to the show."},
};

int ck_now_ns(int64_t *now_ns)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) == -1) {
        return -1;
    }
    *now_ns = (int64_t)time.tv_sec * CK_NSEC_PER_SEC + time.tv_nsec;
    return 0;
}

bool ck_scene_start(CkScene *scene, int64_t now_ns)
{
    if (scene->state == CK_STATE_ACTIVE) {
        return false;
    }
    *scene = (CkScene){.state = CK_STATE_ACTIVE, .start_ns = now_ns};
    return true;
}

bool ck_scene_next_deadline(const CkScene *scene, int64_t *deadline_ns)
{
    if (scene->state != CK_STATE_ACTIVE) {
        return false;
    }
    int milliseconds = CK_SCENE_DURATION_MS;
    if (scene->history_count < CK_CUE_COUNT) {
        milliseconds = cues[scene->history_count].milliseconds;
    }
    *deadline_ns = scene->start_ns + milliseconds * CK_NSEC_PER_MSEC;
    return true;
}

int ck_scene_advance(CkScene *scene, int64_t now_ns)
{
    if (scene->state != CK_STATE_ACTIVE) {
        return 0;
    }

    while (scene->history_count < CK_CUE_COUNT) {
        const Cue *cue = &cues[scene->history_count];
        int64_t scheduled_ns = cue->milliseconds * CK_NSEC_PER_MSEC;
        if (now_ns - scene->start_ns < scheduled_ns) {
            break;
        }

        int64_t actual_ns;
        if (ck_now_ns(&actual_ns) == -1) {
            return -1;
        }
        printf("dispatch cue=%s output=%s value=%s\n", cue->id, cue->output, cue->value);
        fflush(stdout);

        CkCueRecord *record = &scene->history[scene->history_count++];
        memcpy(record->id, cue->id, sizeof(record->id));
        record->scheduled_ns = scheduled_ns;
        record->actual_ns = actual_ns - scene->start_ns;
        record->lateness_ns = record->actual_ns - scheduled_ns;
    }

    if (now_ns - scene->start_ns >= CK_SCENE_DURATION_MS * CK_NSEC_PER_MSEC) {
        scene->state = CK_STATE_COMPLETE;
    }
    return 0;
}

void ck_scene_snapshot(const CkScene *scene, uint32_t result, CkReply *reply)
{
    *reply = (CkReply){.version = CK_PROTOCOL_VERSION,
                       .result = result,
                       .state = scene->state,
                       .history_count = scene->history_count};
    memcpy(reply->history, scene->history, scene->history_count * sizeof(CkCueRecord));
}
