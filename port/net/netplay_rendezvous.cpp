/* SSB64 glue for the n64net rendezvous client (room-code netplay).
 *
 * Settings (environment, or Documents/netplay.cfg on iOS):
 *   SSB64_NETPLAY_SERVER=host:port   rendezvous server (enables room codes)
 *   SSB64_NETPLAY_ROOM=ABCD          room code; the host may leave it unset
 *                                    to get a random one (shown in the log)
 *   SSB64_NETPLAY_PASSWORD=...       server password, if the server has one
 *   SSB64_NETPLAY_HOST=1             create the room (P1); otherwise join (P2)
 *   SSB64_NETPLAY_RELAY=1            skip hole punching, always relay (testing)
 *   SSB64_NETPLAY_WAIT_SECS=600      how long to wait for the other player
 */
#include "n64net_rendezvous.h"
#include "netplay_build_id.h"  // generated: cmake/NetplayBuildId.cmake

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(BATTLESHIP_IOS)
extern "C" void port_ios_alert(const char* text);  // port_ios_url.mm
#endif

extern "C" void port_log(const char *fmt, ...);
extern "C" void port_watchdog_note_yield(void);

namespace {
// Only builds with the same game, netplay protocol and simulation may pair:
// "<game+protocol>+<build fingerprint>". Bump np<N> when the SSB64 netplay
// packet format or match config changes; the fingerprint covers the rest.
constexpr const char *kGameId = "ssb64-us-np2+" SSB64_NETPLAY_BUILD_ID;

// A sentence for the player explaining why joining failed.
std::string ExplainFailure(const char *error, const char *room) {
    std::string e = error ? error : "";
    std::string r = room ? room : "";
    if (e.rfind("version", 0) == 0) {
        return "Can't join room " + r + ": your game version (" SSB64_NETPLAY_BUILD_ID ") is different from the "
               "host's (" + (e.size() > 8 ? e.substr(8) : "unknown") + "). Both players need the same version: "
               "update (git pull) and rebuild, then try again.";
    }
    if (e == "auth") {
        return "The netplay server refused the password. Ask the host for a new invite link.";
    }
    if (e == "noroom") {
        return "Nobody is hosting room " + r + " right now. Ask the host to start it, then open the game again.";
    }
    if (e == "timeout") {
        return "The other player didn't arrive in time. Open the game again to retry.";
    }
    if (e == "resolve") {
        return "Couldn't find the netplay server. Check your internet connection and the invite link.";
    }
    if (e == "full" || e == "taken") {
        return "Room " + r + " is already in use. Ask the host for a new invite link.";
    }
    return "Couldn't connect for online play (" + e + "). Playing offline.";
}

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
    cfg.password = std::getenv("SSB64_NETPLAY_PASSWORD");
    cfg.is_host = is_host;
    cfg.force_relay = EnvInt("SSB64_NETPLAY_RELAY", 0);
    cfg.wait_secs = EnvInt("SSB64_NETPLAY_WAIT_SECS", 600);
    cfg.log = port_log;
    cfg.yield = port_watchdog_note_yield;

    port_log("SSB64 NetPeer: netplay build %s\n", SSB64_NETPLAY_BUILD_ID);
    N64NetRendezvousResult result{};
    int rc = n64net_rendezvous_connect(sock, &cfg, &result);
    if (rc != 0) {
        std::string why = ExplainFailure(result.error, cfg.room);
        port_log("SSB64 NetPeer: %s\n", why.c_str());
#if defined(BATTLESHIP_IOS)
        port_ios_alert(why.c_str());
#endif
        return rc;
    }
    *out_peer = result.peer;
    port_log("SSB64 NetPeer: room %s connected (%s)\n", result.room, result.relayed ? "relayed" : "direct");
    return 0;
}
