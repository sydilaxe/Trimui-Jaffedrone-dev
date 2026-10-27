/* Wi-Fi sync between HI5 / Jaffedrone units (same protocol as HI5): each one broadcasts tempo, bar phase and
 * transport over UDP ten times a second. The lowest id (or the unit set to
 * LEAD) leads; followers lock their phase by trimming playback speed. */
#pragma once
#include <stdint.h>

#define SYNC_PORT 5150
#define SYNC_PKT_LEN 32

typedef struct {
    uint32_t id;
    int peers;       /* other units heard in the last 3 s */
    int leading;     /* this unit is the leader */
    int have_leader; /* following someone */
    uint32_t leader_id;
    float offset_ms; /* follower: how far we trail the leader (+ = behind) */
} SyncStatus;

void sync_init(void);             /* picks an id and opens the socket */
void sync_poll(uint64_t now_us);  /* main thread, every frame */
void sync_close(void);
const SyncStatus *sync_status(void);

/* protocol core, no sockets (the test harness drives these directly) */
void sync_core_init(uint32_t id);
int sync_make_packet(uint8_t *buf, uint64_t now_us);
void sync_handle_packet(const uint8_t *buf, int n, uint64_t now_us);
void sync_elect(uint64_t now_us);
