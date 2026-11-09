/* Smash as an i64 game module (port/i64/n64_game.h).
 *
 * Built only with -DSSB64_I64_MODULE=ON, which makes the game a shared
 * library that exports just the n64_game_* functions (see CMakeLists.txt).
 * i64 owns the app, SDL, the window, audio output and the network session;
 * this file maps its calls onto the stand-alone game's start-up, frame and
 * shutdown (port.cpp) and resets the module to a clean slate after each run
 * (port/reentry.cpp), so it can be started again without restarting the app.
 *
 * Online runs use i64's game channel (host->net_send/net_recv, through
 * port/net/netplay_transport.cpp): no socket or room server of our own, no
 * voice (i64 does it). The usual session handshake then runs over that
 * channel (the host sends its rules, save data and settings; both start on
 * the same tick).
 * Not yet: session blob, ROM import on the device, title art, pause. */
#if defined(SSB64_I64_MODULE)

#define N64_GAME_BUILDING_MODULE 1
#include "i64/n64_game.h"

#include "port_log.h"
#include "reentry.h"

#include "netplay_build_id.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
int PortStartupForModule(int argc, char* argv[]);
void PortRunBegin(void);
int PortRunFrame(void);
void PortRunEnd(void);
void LUS_SetExternalWindow(void* sdlWindow, void** rendererCache);
/* Game audio goes to the host (port/audio/audio_playback.cpp). */
extern void (*gPortHostAudioWrite)(const int16_t* stereo, uint32_t frames, uint32_t rate);
extern uint32_t (*gPortHostAudioBuffered)(uint32_t rate);
/* Online: the host's game channel (port/net/netplay_transport.cpp). */
extern int (*gPortNetHostSend)(const void* data, uint32_t size);
extern int (*gPortNetHostRecv)(void* buf, uint32_t cap);
extern uint32_t gPortNetHostMaxDatagram;
}

namespace {

const n64_game_rom kRoms[] = {
    {"e2929e10fccc0aa84e5776227e798abc07cedabf", "USA 1.0"},
};

const n64_game_info kInfo = {
    N64_GAME_ABI_VERSION,
    "ssb64-us",
    "Super Smash Bros.",
    kRoms,
    1,
    SSB64_NETPLAY_BUILD_ID,
    2,
    60,
    0,
};

/* Survive the reset of the globals between runs (reentry.cpp). */
#define KEEP __attribute__((section("__DATA,__ssb64keep")))
KEEP const n64_host* sHost = nullptr;
KEEP bool sRunning = false;
KEEP bool sExitRequested = false;
KEEP char sLogPath[1024] = {};
KEEP void* sRenderer = nullptr;  // SDL renderer on the host window, reused across runs
KEEP void* sRendererWindow = nullptr;

void SetEnv(const char* key, const std::string& value) {
    setenv(key, value.c_str(), 1);
}

} // namespace

extern "C" N64_GAME_EXPORT const n64_game_info* n64_game_get_info(void) {
    return &kInfo;
}

extern "C" N64_GAME_EXPORT int n64_game_import_rom(const n64_host* host, const char* rom_path,
                                                   void (*progress)(float, void*), void* user, char* err,
                                                   size_t err_size) {
    (void)host;
    (void)rom_path;
    (void)progress;
    (void)user;
    std::snprintf(err, err_size, "ROM import inside i64 isn't built yet");
    return 1;
}

extern "C" N64_GAME_EXPORT uint32_t n64_game_session_blob(const n64_host* host, void* out, uint32_t cap) {
    (void)host;
    (void)out;
    (void)cap;
    return 0;  // online runs not yet
}

extern "C" N64_GAME_EXPORT int n64_game_start(const n64_host* host, const n64_launch* launch, char* err,
                                              size_t err_size) {
    if (sRunning) {
        std::snprintf(err, err_size, "already running");
        return 1;
    }
    if (host == nullptr || launch == nullptr || host->abi_version != N64_GAME_ABI_VERSION ||
        launch->abi_version != N64_GAME_ABI_VERSION) {
        std::snprintf(err, err_size, "i64 and this game module disagree on the interface version");
        return 1;
    }
    if (launch->mode == N64_LAUNCH_ONLINE &&
        (host->net_send == nullptr || host->net_recv == nullptr || launch->local_player > 1)) {
        std::snprintf(err, err_size, "online needs i64's game channel and 2 players");
        return 1;
    }
    /* First start: everything the module has done so far is static
     * initialisation, i.e. the pristine state to come back to after a run. */
    port_reentry_save_pristine();

    sHost = host;
    sExitRequested = false;
    if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
        SetEnv("SHIP_HOME", host->data_dir);  // config, saves, BattleShip.o2r
        std::snprintf(sLogPath, sizeof sLogPath, "%s/ssb64.log", host->data_dir);
        SetEnv("SSB64_LOG_PATH", sLogPath);
    }
    if (sRendererWindow != host->window) {
        sRenderer = nullptr;  // a different window: LUS creates a new renderer
        sRendererWindow = host->window;
    }
    LUS_SetExternalWindow(host->window, &sRenderer);

    /* Netplay settings are read from the environment at start-up: clear any
     * left from an earlier run, then describe this one. */
    static const char* const kNetVars[] = {
        "SSB64_NETPLAY", "SSB64_NETPLAY_MODE", "SSB64_NETPLAY_BOOTSTRAP", "SSB64_NETPLAY_HOST",
        "SSB64_NETPLAY_LOCAL_PLAYER", "SSB64_NETPLAY_REMOTE_PLAYER", "SSB64_NETPLAY_BIND", "SSB64_NETPLAY_PEER",
        "SSB64_NETPLAY_SERVER", "SSB64_NETPLAY_ROOM", "SSB64_NETPLAY_PASSWORD", "SSB64_NETPLAY_SEED",
        "SSB64_NETPLAY_VOICE", "SSB64_NETPLAY_LOBBY", "SSB64_NETPLAY_SESSION",
    };
    for (const char* v : kNetVars) unsetenv(v);
    gPortNetHostSend = nullptr;
    gPortNetHostRecv = nullptr;
    if (launch->mode == N64_LAUNCH_ONLINE) {
        const bool isHost = launch->local_player == 0;
        SetEnv("SSB64_NETPLAY", "1");
        SetEnv("SSB64_NETPLAY_MODE", "session");      // the whole game as one console
        SetEnv("SSB64_NETPLAY_BOOTSTRAP", "1");
        if (isHost) SetEnv("SSB64_NETPLAY_HOST", "1");
        SetEnv("SSB64_NETPLAY_LOCAL_PLAYER", isHost ? "0" : "1");
        SetEnv("SSB64_NETPLAY_REMOTE_PLAYER", isHost ? "1" : "0");
        SetEnv("SSB64_NETPLAY_BIND", "0.0.0.0:0");     // unused: i64's channel
        SetEnv("SSB64_NETPLAY_PEER", "0.0.0.0:0");
        if (launch->seed != 0) SetEnv("SSB64_NETPLAY_SEED", std::to_string(launch->seed));
        gPortNetHostSend = host->net_send;
        gPortNetHostRecv = host->net_recv;
        gPortNetHostMaxDatagram = host->net_max_datagram ? host->net_max_datagram : 1200;
    }
    gPortHostAudioWrite = host->audio_write;
    gPortHostAudioBuffered = host->audio_buffered;

    char arg0[] = "ssb64";
    char* argv[] = {arg0, nullptr};
    if (PortStartupForModule(1, argv) != 0) {
        std::snprintf(err, err_size, "the game couldn't start (see ssb64.log)");
        return 1;
    }
    PortRunBegin();
    sRunning = true;
    return 0;
}

extern "C" N64_GAME_EXPORT n64_frame_result n64_game_frame(void) {
    if (!sRunning || sExitRequested) {
        return N64_GAME_FRAME_EXIT;
    }
    return PortRunFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

extern "C" N64_GAME_EXPORT void n64_game_set_paused(int paused) {
    (void)paused;  // not yet: i64 simply stops calling frame() meanwhile
}

extern "C" N64_GAME_EXPORT void n64_game_stop(void) {
    if (!sRunning) {
        return;
    }
    PortRunEnd();
    gPortHostAudioWrite = nullptr;
    gPortHostAudioBuffered = nullptr;
    gPortNetHostSend = nullptr;
    gPortNetHostRecv = nullptr;
    LUS_SetExternalWindow(nullptr, nullptr);
    port_reentry_release_caches();
    port_reentry_restore_pristine();
    sRunning = false;
    sHost = nullptr;
}

#endif /* SSB64_I64_MODULE */
