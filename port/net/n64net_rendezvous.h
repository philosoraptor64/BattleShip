/* n64net rendezvous client: find the other player through a room code.
 *
 * Game-neutral (no SSB64 types); intended to move into the shared n64net
 * library. Runs on the game's own UDP socket, so the NAT mapping it opens
 * is the one gameplay packets use afterwards. See
 * N64/n64net/server/rendezvous.py for the protocol.
 */
#pragma once

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct N64NetRendezvousConfig {
    const char *server;      /* "host:port"; host may be a DNS name */
    const char *game;        /* game + protocol id; only equal ids pair up */
    const char *room;        /* 4-char code; NULL/"" lets the host get a random one */
    int is_host;             /* 1 = create the room, 0 = join it */
    int force_relay;         /* skip hole punching (testing) */
    int wait_secs;           /* how long to wait for the other player (default 600) */
    void (*log)(const char *fmt, ...);
    void (*yield)(void);     /* called every ~10ms while waiting (watchdog etc.) */
} N64NetRendezvousConfig;

typedef struct N64NetRendezvousResult {
    struct sockaddr_in peer; /* where to send gameplay packets */
    int relayed;             /* 1 = peer is the server, which forwards */
    int is_host;             /* role (a joiner is never host) */
    char room[8];
} N64NetRendezvousResult;

/* Blocks until connected, failed or timed out. Returns 0 on success.
 * `sock` must be a bound, non-blocking IPv4 UDP socket. */
int n64net_rendezvous_connect(int sock, const N64NetRendezvousConfig *config, N64NetRendezvousResult *out);

#ifdef __cplusplus
}
#endif
