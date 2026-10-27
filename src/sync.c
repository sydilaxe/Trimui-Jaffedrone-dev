#include "sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "ui.h"

#define NPEER 8
#define PEER_TTL_US 3000000ULL
#define SEND_US 100000ULL
#define NERR 8

typedef struct {
    uint32_t id;
    uint64_t last_us;
    int playing, role;
    float bpm;
    double pos;
} Peer;

static SyncStatus st;
static Peer peers[NPEER];
static int sock = -1;
static uint64_t last_send_us, last_if_us;
static struct sockaddr_in bcast[8];
static int nbcast;
static double errs[NERR];
static int nerrs, erri;

/* ------------------------------------------------------------ protocol */

static void put32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }
static uint32_t get32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }

void sync_core_init(uint32_t id)
{
    memset(&st, 0, sizeof st);
    memset(peers, 0, sizeof peers);
    st.id = id;
    nerrs = erri = 0;
}

int sync_make_packet(uint8_t *b, uint64_t now_us)
{
    memcpy(b, "HI5S", 4);
    b[4] = 1;
    b[5] = (uint8_t)!!live.playing;
    b[6] = (uint8_t)song.gp[GP_SYNC];
    b[7] = 0;
    put32(b + 8, st.id);
    float bpm = engine_bpm();
    memcpy(b + 12, &bpm, 4);
    ui_lock();
    double pos = engine_position();
    ui_unlock();
    memcpy(b + 16, &pos, 8);
    memcpy(b + 24, &now_us, 8);
    return SYNC_PKT_LEN;
}

static Peer *peer_slot(uint32_t id, uint64_t now_us)
{
    Peer *free = NULL;
    for (int i = 0; i < NPEER; i++) {
        if (peers[i].id == id && peers[i].last_us)
            return &peers[i];
        if (!free && (!peers[i].last_us || now_us - peers[i].last_us > PEER_TTL_US))
            free = &peers[i];
    }
    if (free)
        memset(free, 0, sizeof *free);
    return free;
}

static double wrap16(double x)
{
    x = fmod(x, 16.0);
    if (x > 8.0)
        x -= 16.0;
    if (x <= -8.0)
        x += 16.0;
    return x;
}

static double median_err(void)
{
    double a[NERR];
    int n = nerrs < NERR ? nerrs : NERR;
    memcpy(a, errs, n * sizeof a[0]);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && a[j - 1] > a[j]; j--) {
            double t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
    return n ? a[n / 2] : 0.0;
}

/* a packet from the unit we follow: match tempo and transport, then phase */
static void follow(const Peer *p)
{
    ui_lock();
    if (fabsf(p->bpm - engine_bpm()) > 0.01f && p->bpm >= 40.f && p->bpm <= 250.f)
        engine_set_bpm(p->bpm);
    if (p->playing != live.playing) {
        engine_set_playing(p->playing);
        if (p->playing)
            engine_set_position(p->pos);
        nerrs = erri = 0;
    }
    double rate = 1.0;
    if (live.playing) {
        double err = wrap16(p->pos - engine_position());
        errs[erri] = err;
        erri = (erri + 1) % NERR;
        if (nerrs < NERR)
            nerrs++;
        double med = median_err();
        if (nerrs >= 3 && fabs(med) > 0.5) {
            engine_set_position(engine_position() + med);
            nerrs = erri = 0;
            med = 0.0;
        }
        else if (nerrs >= 3) {
            double k = med * 0.15;
            rate = 1.0 + (k > 0.03 ? 0.03 : k < -0.03 ? -0.03 : k);
        }
        st.offset_ms = (float)(med * 15000.0 / engine_bpm());
    }
    engine_set_rate(rate);
    ui_unlock();
}

void sync_handle_packet(const uint8_t *b, int n, uint64_t now_us)
{
    if (n < SYNC_PKT_LEN || memcmp(b, "HI5S", 4) || b[4] != 1)
        return;
    uint32_t id = get32(b + 8);
    if (id == st.id || song.gp[GP_SYNC] == SYNC_OFF)
        return;
    Peer *p = peer_slot(id, now_us);
    if (!p)
        return;
    p->id = id;
    p->last_us = now_us;
    p->playing = b[5];
    p->role = b[6];
    memcpy(&p->bpm, b + 12, 4);
    memcpy(&p->pos, b + 16, 8);
    sync_elect(now_us);
    if (st.have_leader && st.leader_id == id)
        follow(p);
}

/* leader: a unit set to LEAD beats one on AUTO, then the lowest id wins;
 * units set to FOLLOW never lead */
void sync_elect(uint64_t now_us)
{
    int alive = 0, best_rank = 3;
    uint32_t best_id = 0;
    int mode = song.gp[GP_SYNC];
    int self_rank = mode == SYNC_LEAD ? 0 : mode == SYNC_AUTO ? 1 : 3;
    if (self_rank < 3) {
        best_rank = self_rank;
        best_id = st.id;
    }
    for (int i = 0; i < NPEER; i++) {
        Peer *p = &peers[i];
        if (!p->last_us || now_us - p->last_us > PEER_TTL_US)
            continue;
        alive++;
        int rank = p->role == SYNC_LEAD ? 0 : p->role == SYNC_AUTO ? 1 : 3;
        if (rank < best_rank || (rank == best_rank && rank < 3 && p->id < best_id)) {
            best_rank = rank;
            best_id = p->id;
        }
    }
    st.peers = alive;
    int leading = mode != SYNC_OFF && best_rank < 3 && best_id == st.id;
    int have_leader = mode != SYNC_OFF && best_rank < 3 && best_id != st.id;
    if (!have_leader && st.have_leader) {
        ui_lock();
        engine_set_rate(1.0);
        ui_unlock();
        nerrs = erri = 0;
        st.offset_ms = 0.f;
    }
    if (have_leader && (!st.have_leader || st.leader_id != best_id))
        nerrs = erri = 0;
    st.leading = leading;
    st.have_leader = have_leader;
    st.leader_id = have_leader ? best_id : 0;
}

const SyncStatus *sync_status(void) { return &st; }

/* ------------------------------------------------------------- sockets */

static void refresh_bcast(void)
{
    struct ifaddrs *ifa, *p;
    nbcast = 0;
    if (getifaddrs(&ifa) != 0)
        return;
    for (p = ifa; p && nbcast < 8; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_BROADCAST) ||
            (p->ifa_flags & IFF_LOOPBACK) || !p->ifa_broadaddr)
            continue;
        bcast[nbcast] = *(struct sockaddr_in *)p->ifa_broadaddr;
        bcast[nbcast].sin_port = htons(SYNC_PORT);
        nbcast++;
    }
    freeifaddrs(ifa);
}

void sync_init(void)
{
    uint32_t id = (uint32_t)time(NULL) ^ (uint32_t)getpid() * 2654435761u;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        uint32_t r = 0;
        if (fread(&r, 1, 4, f) == 4)
            id ^= r;
        fclose(f);
    }
    if (!id)
        id = 1;
    sync_core_init(id);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(SYNC_PORT);
    if (bind(sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "sync: bind failed: %s\n", strerror(errno));
        close(sock);
        sock = -1;
        return;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    refresh_bcast();
}

void sync_poll(uint64_t now_us)
{
    if (sock < 0)
        return;
    uint8_t b[64];
    for (;;) {
        ssize_t n = recv(sock, b, sizeof b, 0);
        if (n <= 0)
            break;
        sync_handle_packet(b, (int)n, now_us);
    }
    sync_elect(now_us);
    if (song.gp[GP_SYNC] == SYNC_OFF)
        return;
    if (now_us - last_if_us > 5000000ULL) {
        last_if_us = now_us;
        refresh_bcast();
    }
    if (now_us - last_send_us < SEND_US)
        return;
    last_send_us = now_us;
    int n = sync_make_packet(b, now_us);
    for (int i = 0; i < nbcast; i++)
        sendto(sock, b, n, 0, (struct sockaddr *)&bcast[i], sizeof bcast[i]);
    if (!nbcast) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        a.sin_port = htons(SYNC_PORT);
        sendto(sock, b, n, 0, (struct sockaddr *)&a, sizeof a);
    }
}

void sync_close(void)
{
    if (sock >= 0)
        close(sock);
    sock = -1;
}
