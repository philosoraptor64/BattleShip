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

#include "coroutine.h"
#include "port_watchdog.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(BATTLESHIP_IOS)
extern "C" void port_ios_alert(const char* text);                         // port_ios_url.mm
extern "C" void port_ios_status_show(const char* title, const char* text);  // port_ios_status.mm
extern "C" void port_ios_status_hide(void);
#endif

extern "C" void port_log(const char *fmt, ...);

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

/* One step of a start-up wait (finding the other player, the netplay
 * handshake): hand the frame back to the main loop instead of sleeping, so
 * the app keeps handling OS events (iOS kills an app that stops responding,
 * e.g. when the player switches apps while waiting), then pad to ~16 ms so
 * retry counts keep meaning the same time. Only for waits before a battle:
 * rollback snapshots happen at frame boundaries during battles. */
extern "C" void port_netplay_boot_wait(void) {
    const auto start = std::chrono::steady_clock::now();
    port_coroutine_yield();  // no-op outside a coroutine
    const auto spent = std::chrono::steady_clock::now() - start;
    const auto step = std::chrono::microseconds(16666);
    if (spent < step) {
        std::this_thread::sleep_for(step - spent);
    }
    port_watchdog_note_yield();
}

/* Status shown while connecting (iOS overlay; NULL title hides it). Called
 * from netpeer when the handshake finishes or fails. */
extern "C" void port_netplay_status(const char *title, const char *text) {
#if defined(BATTLESHIP_IOS)
    if (title == nullptr) {
        port_ios_status_hide();
    } else {
        port_ios_status_show(title, text ? text : "");
    }
#else
    (void)title;
    (void)text;
#endif
}

/* A message for the player about online play (iOS alert; logged anyway). */
extern "C" void port_netplay_alert(const char *text) {
    port_log("SSB64 NetPeer: %s\n", text);
#if defined(BATTLESHIP_IOS)
    port_ios_alert(text);
#endif
}

namespace {
N64NetRendezvousConfig BaseConfig() {
    N64NetRendezvousConfig cfg{};
    cfg.server = std::getenv("SSB64_NETPLAY_SERVER");
    cfg.game = kGameId;
    cfg.room = std::getenv("SSB64_NETPLAY_ROOM");
    cfg.password = std::getenv("SSB64_NETPLAY_PASSWORD");
    cfg.log = port_log;
    cfg.yield = port_netplay_boot_wait;
    return cfg;
}
} // namespace

extern "C" void port_netplay_rendezvous_bye(int sock) {
    N64NetRendezvousConfig cfg = BaseConfig();
    n64net_rendezvous_bye(sock, &cfg);
}

extern "C" int port_netplay_rendezvous(int sock, int is_host, struct sockaddr_in *out_peer) {
    N64NetRendezvousConfig cfg = BaseConfig();
    cfg.is_host = is_host;
    cfg.force_relay = EnvInt("SSB64_NETPLAY_RELAY", 0);
    // Host-set-up matches: the host waits 5 minutes for a joiner before
    // playing offline; the joiner waits up to an hour for the host's next match.
    const bool lobby = EnvInt("SSB64_NETPLAY_LOBBY", 0) != 0;
    cfg.wait_secs = EnvInt("SSB64_NETPLAY_WAIT_SECS", lobby ? (is_host ? 300 : 3600) : 600);

    port_log("SSB64 NetPeer: netplay build %s\n", SSB64_NETPLAY_BUILD_ID);
    {
        std::string room = (cfg.room && cfg.room[0]) ? cfg.room : "";
        std::string text = is_host ? "Room " + room + " is open. Waiting for the other player to join..."
                                   : "Joining room " + room + "...";
        port_netplay_status(is_host ? "Hosting an online match" : "Joining an online match", text.c_str());
    }
    N64NetRendezvousResult result{};
    int rc = n64net_rendezvous_connect(sock, &cfg, &result);
    if (rc != 0) {
        std::string why = ExplainFailure(result.error, cfg.room);
        port_log("SSB64 NetPeer: %s\n", why.c_str());
        port_netplay_status(nullptr, nullptr);
#if defined(BATTLESHIP_IOS)
        port_ios_alert(why.c_str());
#endif
        return rc;
    }
    *out_peer = result.peer;
    port_netplay_status(is_host ? "Hosting an online match" : "Joining an online match",
                        "Player found. Starting the match...");
    port_log("SSB64 NetPeer: room %s connected (%s)\n", result.room, result.relayed ? "relayed" : "direct");
    return 0;
}
