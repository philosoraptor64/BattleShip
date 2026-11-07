/* SSB64 glue for the n64net rendezvous client (room-code netplay).
 *
 * Settings (environment, or Documents/netplay.cfg on iOS):
 *   SSB64_NETPLAY_SERVER=host:port   rendezvous server (enables room codes)
 *   SSB64_NETPLAY_ROOM=ABCD          room code; the host may leave it unset
 *                                    to get a random one (shown in the log)
 *   SSB64_NETPLAY_HOST=1             create the room (P1); otherwise join (P2)
 *   SSB64_NETPLAY_RELAY=1            skip hole punching, always relay (testing)
 *   SSB64_NETPLAY_WAIT_SECS=600      how long to wait for the other player
 */
#include "n64net_rendezvous.h"

#include <cstdlib>

extern "C" void port_log(const char *fmt, ...);
extern "C" void port_watchdog_note_yield(void);

namespace {
// Only builds with the same game and netplay protocol may pair. Bump when
// the SSB64 netplay packet format or match config changes.
constexpr const char *kGameId = "ssb64-us-np2";

int EnvInt(const char *name, int fallback) {
    const char *v = std::getenv(name);
    return (v != nullptr && v[0] != '\0') ? std::atoi(v) : fallback;
}
} // namespace

extern "C" int port_netplay_rendezvous(int sock, int is_host, struct sockaddr_in *out_peer) {
    N64NetRendezvousConfig cfg{};
    cfg.server = std::getenv("SSB64_NETPLAY_SERVER");
    cfg.game = kGameId;
    cfg.room = std::getenv("SSB64_NETPLAY_ROOM");
    cfg.is_host = is_host;
    cfg.force_relay = EnvInt("SSB64_NETPLAY_RELAY", 0);
    cfg.wait_secs = EnvInt("SSB64_NETPLAY_WAIT_SECS", 600);
    cfg.log = port_log;
    cfg.yield = port_watchdog_note_yield;

    N64NetRendezvousResult result{};
    int rc = n64net_rendezvous_connect(sock, &cfg, &result);
    if (rc != 0) {
        return rc;
    }
    *out_peer = result.peer;
    port_log("SSB64 NetPeer: room %s connected (%s)\n", result.room, result.relayed ? "relayed" : "direct");
    return 0;
}
