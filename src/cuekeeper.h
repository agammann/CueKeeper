#ifndef CUEKEEPER_H
#define CUEKEEPER_H

#include <stdbool.h>
#include <stdint.h>

#define CK_SERVICE "cuekeeper"
#define CK_REQUEST_TYPE UINT16_C(0x434b)
#define CK_PROTOCOL_VERSION UINT16_C(1)
#define CK_NSEC_PER_SEC INT64_C(1000000000)
#define CK_NSEC_PER_MSEC INT64_C(1000000)
#define CK_SCENE_NAME "opening"
#define CK_SCENE_DURATION_MS 30000
#define CK_CUE_COUNT 3
#define CK_CUE_ID_SIZE 8

enum { CK_CMD_GO = 1, CK_CMD_STATUS };
enum { CK_RESULT_OK, CK_RESULT_ACTIVE, CK_RESULT_INVALID };
enum { CK_STATE_IDLE, CK_STATE_ACTIVE, CK_STATE_COMPLETE };

/* Local IPC. Both processes use the same target ABI and protocol version. */
typedef struct {
    uint16_t type;
    uint16_t version;
    uint32_t command;
} CkRequest;

typedef struct {
    char id[CK_CUE_ID_SIZE];
    int64_t scheduled_ns;
    int64_t actual_ns;
    int64_t lateness_ns;
} CkCueRecord;

typedef struct {
    uint32_t version;
    uint32_t result;
    uint32_t state;
    uint32_t history_count;
    CkCueRecord history[CK_CUE_COUNT];
} CkReply;

typedef struct {
    int64_t start_ns;
    uint32_t state;
    uint32_t history_count;
    CkCueRecord history[CK_CUE_COUNT];
} CkScene;

int ck_now_ns(int64_t *now_ns);
bool ck_scene_start(CkScene *scene, int64_t now_ns);
bool ck_scene_next_deadline(const CkScene *scene, int64_t *deadline_ns);
int ck_scene_advance(CkScene *scene, int64_t now_ns);
void ck_scene_snapshot(const CkScene *scene, uint32_t result, CkReply *reply);

#endif
