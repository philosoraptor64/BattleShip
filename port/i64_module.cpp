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
#include <atomic>
#include <chrono>
#include "i64/n64_game.h"

#include "port_log.h"
#include "reentry.h"

#include "netplay_build_id.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

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
extern int (*gPortNetHostSendTo)(uint32_t machine, const void* data, uint32_t size);
extern int gPortCatchUp;
extern "C" int syNetPeerAdmit(int machine);   // decomp sys/netpeer.c
extern "C" unsigned syNetPeerOpenPlayers(void);
extern "C" int syNetPeerAdd(int machine, unsigned mask);
extern "C" int syNetPeerCatchingUp(void);
extern int (*gPortNetHostRecv)(void* buf, uint32_t cap);
extern int (*gPortNetHostRecvFrom)(void* buf, uint32_t cap, uint32_t* machine);
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
    4,      /* players: up to 4 over up to 4 devices (several controllers on one device) */
    60,
    N64_GAME_INFO_CAN_UNLOCK_ALL,   /* "everything unlocked" (all characters, Mushroom Kingdom, item switch, sound test) */
    /* v2: a device that leaves: the game's own CPU takes its fighter over, or it stays in with nothing pressed */
    N64_DROPOUT_CAN_CPU | N64_DROPOUT_CAN_IDLE | N64_DROPOUT_CAN_JOIN,
    4,
};

/* Survive the reset of the globals between runs (reentry.cpp). */
#define KEEP __attribute__((section("__DATA,__ssb64keep")))
KEEP const n64_host* sHost = nullptr;

/* i64's controllers (n64_host.pad_read; i64 docs/controllers.md): pad k of this device -> port k, in place of
 * libultraship's ControlDeck (libultraship os.cpp gLusHostPadRead), so a controller behaves the same in every game.
 * Online, the netplay code takes this device's controllers from these ports as before. */
extern "C" int (*gLusHostPadRead)(int port, uint16_t* button, int8_t* stick_x, int8_t* stick_y);
static int HostPadRead(int port, uint16_t* button, int8_t* stick_x, int8_t* stick_y) {
    n64_pad_state st{};
    if (sHost == nullptr || sHost->pad_read == nullptr || port < 0 || !sHost->pad_read((uint32_t)port, &st)) {
        return 0;
    }
    *button = st.buttons;
    *stick_x = st.stick_x;
    *stick_y = st.stick_y;
    return 1;
}
bool sUnlockAll = false;
extern "C" void port_save_reset_path(void);   // port/port_save.cpp
extern "C" bool gLusHostPaced; /* libultraship gfx_sdl2.cpp */
/* libultraship gfx_metal.cpp: a picture of the screen for i64's home screen (30 s in, then each minute) */
extern "C" void (*gLusFrameCaptureCallback)(const uint8_t* rgba, uint32_t w, uint32_t h);
extern "C" std::atomic<int> gLusFrameCaptureRequest;
static unsigned sPreviewFrames = 0;

/* i64 decides when it wants a home-screen picture (todo item 20: one rule in i64 for every game) */
static volatile int sPreviewWanted = 0;
extern "C" N64_GAME_EXPORT void n64_game_capture_preview(void) { sPreviewWanted = 1; }
static bool PreviewDue(unsigned frame) {
    (void) frame;
    if (!sPreviewWanted) return false;
    sPreviewWanted = 0;
    return true;
}
#if defined(__APPLE__)
extern "C" uint64_t (*port_coroutine_arena_reserve)(uint64_t size, uint64_t align);   /* coroutine_android.cpp */
extern "C" void port_coroutine_pool_release_for_reentry(void);
extern "C" uint64_t (*gPortSceneHeapArena)(uint64_t size, uint64_t align);
#endif
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

/* ROM import: Torch extracts BattleShip.o2r from the player's own ROM into
 * host->data_dir, using the recipes (config.yml + yamls/) shipped with the
 * module in host->bundle_dir (TorchRecipe/ if present). Any byte order is
 * accepted: .n64/.v64 dumps are converted to big-endian (.z64) first. */
extern "C" int torch_extract_o2r(const char* rom_path, const char* src_dir, const char* dst_dir);

extern "C" N64_GAME_EXPORT int n64_game_import_rom(const n64_host* host, const char* rom_path,
                                                   void (*progress)(float, void*), void* user, char* err,
                                                   size_t err_size) {
    namespace fs = std::filesystem;
    /* Before Torch runs: everything so far is static initialisation, the
     * pristine state. Taking it here also closes the list of C++ blocks made
     * "before main" (reentry.cpp): otherwise Torch's big temporary buffers
     * land in it, and those libc++ frees behind operator delete's back. */
    port_reentry_save_pristine();
    if (host == nullptr || rom_path == nullptr || host->data_dir == nullptr || host->bundle_dir == nullptr) {
        std::snprintf(err, err_size, "missing folders");
        return 1;
    }
    if (sRunning) {
        std::snprintf(err, err_size, "the game is running");
        return 1;
    }
    if (progress != nullptr) progress(0.0f, user);
    std::error_code ec;
    const fs::path data(host->data_dir);
    fs::create_directories(data, ec);
    fs::path recipes = fs::path(host->bundle_dir) / "TorchRecipe";
    if (!fs::exists(recipes / "config.yml", ec)) {
        recipes = fs::path(host->bundle_dir);
    }
    if (!fs::exists(recipes / "config.yml", ec)) {
        std::snprintf(err, err_size, "the game's extraction recipes are missing from the module");
        return 1;
    }

    /* Byte order: big-endian dumps start 80 37 12 40. */
    std::vector<uint8_t> rom;
    {
        std::ifstream in(rom_path, std::ios::binary);
        rom.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (rom.size() < (8u << 20) || rom.size() > (64u << 20) || (rom.size() % 4) != 0) {
        std::snprintf(err, err_size, "that file isn't an N64 ROM");
        return 1;
    }
    fs::path source(rom_path);
    const fs::path temp = data / ".import.z64";
    if (!(rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40)) {
        if (rom[0] == 0x37 && rom[1] == 0x80) {  // .v64: 16-bit byteswapped
            for (size_t i = 0; i + 1 < rom.size(); i += 2) std::swap(rom[i], rom[i + 1]);
        } else if (rom[0] == 0x40 && rom[1] == 0x12) {  // .n64: 32-bit little-endian
            for (size_t i = 0; i + 3 < rom.size(); i += 4) {
                std::swap(rom[i], rom[i + 3]);
                std::swap(rom[i + 1], rom[i + 2]);
            }
        } else {
            std::snprintf(err, err_size, "that file isn't an N64 ROM");
            return 1;
        }
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(rom.data()), (std::streamsize)rom.size());
        if (!out) {
            std::snprintf(err, err_size, "not enough space to import the ROM");
            return 1;
        }
        source = temp;
    }
    rom.clear();
    rom.shrink_to_fit();

    const fs::path o2r = data / "BattleShip.o2r";
    fs::remove(o2r, ec);
    const int rc = torch_extract_o2r(source.c_str(), recipes.c_str(), data.c_str());
    fs::remove(temp, ec);
    if (rc != 0 || !fs::exists(o2r, ec)) {
        fs::remove(o2r, ec);  // never keep a partial archive
        std::snprintf(err, err_size,
                      rc == 3 ? "this isn't Super Smash Bros. (USA) v1.0" : "extracting the game data failed (%d)", rc);
        return 1;
    }
    /* The recipe stamp the game checks at start (first_run.cpp): the archive
     * matches this build's extraction pipeline. */
    {
        std::ofstream stamp(o2r.string() + ".recipe", std::ios::trunc);
        stamp << SSB64_ASSET_RECIPE_HASH << "\n";
    }
    if (progress != nullptr) progress(1.0f, user);
    return 0;
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
    if (launch->mode == N64_LAUNCH_ONLINE && (host->net_send == nullptr || host->net_recv == nullptr)) {
        std::snprintf(err, err_size, "online needs i64's game channel");
        return 1;
    }
    /* Game data: imported from the player's ROM by n64_game_import_rom, for
     * this build's extraction recipe. */
    if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
        const std::string o2r = std::string(host->data_dir) + "/BattleShip.o2r";
        std::ifstream stamp(o2r + ".recipe");
        std::string have;
        std::getline(stamp, have);
        if (!std::filesystem::exists(o2r)) {
            std::snprintf(err, err_size, "import your Super Smash Bros. ROM first");
            return 1;
        }
        if (have != SSB64_ASSET_RECIPE_HASH) {
            std::snprintf(err, err_size, "the game data is from another version: import the ROM again");
            return 1;
        }
    }
    /* First start: everything the module has done so far is static
     * initialisation, i.e. the pristine state to come back to after a run. */
    port_reentry_save_pristine();

    sHost = host;
    gLusHostPadRead = (host->abi_version >= 2 && host->pad_read != nullptr) ? HostPadRead : nullptr;
    gLusHostPaced = (launch->flags & N64_LAUNCH_HOST_PACED) != 0; /* libultraship: i64 paces the frames */
    sPreviewFrames = 0;
    gLusFrameCaptureCallback = host->offer_title_art;
    port_coroutine_arena_reserve = host->arena_reserve;   /* (NULL in an older i64) */
    gPortSceneHeapArena = host->arena_reserve;            /* the 16 MB scene arena too (scene_heap.cpp) */
    sExitRequested = false;
    if (host->data_dir != nullptr && host->data_dir[0] != '\0') {
        SetEnv("SHIP_HOME", host->data_dir);  // config, saves, BattleShip.o2r
        std::snprintf(sLogPath, sizeof sLogPath, "%s/ssb64.log", host->data_dir);
        SetEnv("SSB64_LOG_PATH", sLogPath);
    }
    if (host->bundle_dir != nullptr && host->bundle_dir[0] != '\0') {
        SetEnv("SSB64_BUNDLE_DIR", host->bundle_dir);  // f3d.o2r, fonts, gamecontrollerdb.txt
        /* the renderer's shader archive comes with the module: without it libultraship's loader stops and the
         * shader warm-up waits for ever (a module installed without its resources): say so instead */
        if (!std::filesystem::exists(std::string(host->bundle_dir) + "/f3d.o2r")) {
            std::snprintf(err, err_size, "Super Smash Bros. is missing part of its install (f3d.o2r): reinstall i64");
            return 1;
        }
    }
    /* "everything unlocked" (i64's choice; online the host's): a save of its own, so its progress is kept and the
     * normal save never gets the unlocks (decomp lb/lbbackup.c applies them at boot) */
    sUnlockAll = (launch->flags & N64_LAUNCH_UNLOCK_ALL) != 0;
    unsetenv("SSB64_SAVE_PATH");
    if (sUnlockAll && host->data_dir != nullptr && host->data_dir[0] != '\0') {
        SetEnv("SSB64_SAVE_PATH", std::string(host->data_dir) + "/ssb64_save-unlocked.bin");
    }
    port_save_reset_path();
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
        "SSB64_NETPLAY_VOICE", "SSB64_NETPLAY_LOBBY", "SSB64_NETPLAY_SESSION", "SSB64_NETPLAY_LOCAL_MASK",
        "SSB64_NETPLAY_REMOTE_MASK", "SSB64_NETPLAY_PADS", "SSB64_NETPLAY_MACHINES", "SSB64_NETPLAY_MACHINE_OF",
        "SSB64_NETPLAY_JOINING",
    };
    for (const char* v : kNetVars) unsetenv(v);
    gPortNetHostSend = nullptr;
    gPortNetHostRecv = nullptr;
    gPortNetHostRecvFrom = nullptr;
    if (launch->mode == N64_LAUNCH_ONLINE) {
        const bool isHost = launch->local_machine == 0;
        /* the player table (ports in order): this device's players and everyone else's */
        unsigned localMask = 0, remoteMask = 0;
        std::string pads;
        for (uint32_t p = 0; p < launch->player_count && p < 4; p++) {
            if (launch->players[p].machine == N64_MACHINE_NONE) {   // a CPU nobody plays (joining into CPU slots)
                pads += std::string(p ? "," : "") + "0";
                continue;
            }
            (launch->players[p].local ? localMask : remoteMask) |= 1u << p;
            pads += std::string(p ? "," : "") + std::to_string(launch->players[p].local ? launch->players[p].pad : 0);
        }
        if (localMask == 0 || remoteMask == 0) {
            std::snprintf(err, err_size, "online needs players on this device and on another");
            return 1;
        }
        SetEnv("SSB64_NETPLAY", "1");
        SetEnv("SSB64_NETPLAY_MODE", "session");      // the whole game as one console
        SetEnv("SSB64_NETPLAY_BOOTSTRAP", "1");
        if (isHost) SetEnv("SSB64_NETPLAY_HOST", "1");
        SetEnv("SSB64_NETPLAY_LOCAL_PLAYER", std::to_string(__builtin_ctz(localMask)));
        SetEnv("SSB64_NETPLAY_REMOTE_PLAYER", std::to_string(__builtin_ctz(remoteMask)));
        SetEnv("SSB64_NETPLAY_LOCAL_MASK", std::to_string(localMask));
        SetEnv("SSB64_NETPLAY_REMOTE_MASK", std::to_string(remoteMask));
        SetEnv("SSB64_NETPLAY_PADS", pads);
        SetEnv("SSB64_NETPLAY_MACHINES", std::to_string(launch->num_machines));
        {
            std::string machineOf;
            for (uint32_t p = 0; p < launch->player_count && p < 4; p++) {
                const unsigned m = launch->players[p].machine == N64_MACHINE_NONE ? 0 : launch->players[p].machine;
                machineOf += std::string(p ? "," : "") + std::to_string(m);
            }
            SetEnv("SSB64_NETPLAY_MACHINE_OF", machineOf);
        }
        if (launch->flags & N64_LAUNCH_JOINING) SetEnv("SSB64_NETPLAY_JOINING", "1");
        SetEnv("SSB64_NETPLAY_BIND", "0.0.0.0:0");     // unused: i64's channel
        SetEnv("SSB64_NETPLAY_PEER", "0.0.0.0:0");
        if (launch->seed != 0) SetEnv("SSB64_NETPLAY_SEED", std::to_string(launch->seed));
        gPortNetHostSend = host->net_send;
        gPortNetHostSendTo = host->abi_version >= 2 ? host->net_send_to : nullptr;
        gPortNetHostRecv = host->net_recv;
        gPortNetHostRecvFrom = host->net_recv_from;
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

extern "C" const char* port_net_end_reason(void);   // port/net/netplay_transport.cpp

extern "C" int gPortHidden;   // port/net/netplay_transport.cpp

/* i64 went out of view (1) or came back (0): keep playing, drawing nothing meanwhile (no GPU work). */
extern "C" N64_GAME_EXPORT void n64_game_set_hidden(int hidden) {
    gPortHidden = (sRunning && hidden) ? 1 : 0;
    if (sRunning && sHost && sHost->log) {
        sHost->log(hidden ? "ssb64: out of view, still running (nothing drawn)" : "ssb64: back in view");
    }
}

extern "C" N64_GAME_EXPORT n64_frame_result n64_game_frame(void) {
    if (!sRunning || sExitRequested) {
        return N64_GAME_FRAME_EXIT;
    }
    ++sPreviewFrames;
    if (!gPortHidden && PreviewDue(sPreviewFrames) && gLusFrameCaptureCallback) {
        gLusFrameCaptureRequest = 1;
    }
    if (port_net_end_reason()[0]) {
        return N64_GAME_FRAME_EXIT;   // a desync or the friend gone: i64 shows why (n64_game_exit_message)
    }
    if (syNetPeerCatchingUp()) {
        /* joined mid-session: replay the host's log, not shown or heard, as many frames as fit in ~14 ms */
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(14);
        int ok = 1;
        gPortCatchUp = 1;
        do {
            ok = PortRunFrame();
        } while (ok && syNetPeerCatchingUp() && !port_net_end_reason()[0] && std::chrono::steady_clock::now() < until);
        gPortCatchUp = 0;
        return ok ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
    }
    return PortRunFrame() ? N64_GAME_FRAME_CONTINUE : N64_GAME_FRAME_EXIT;
}

/* The session's host: a newcomer takes over the players of `machine` (a device that left; CPU or idle since). */
/* joining into CPUs nobody played (i64 asks the machine that started the game) */
extern "C" N64_GAME_EXPORT uint32_t n64_game_open_players(void) { return syNetPeerOpenPlayers(); }
extern "C" N64_GAME_EXPORT int n64_game_add(uint32_t machine, uint32_t player_mask) {
    return syNetPeerAdd((int)machine, player_mask);
}

extern "C" N64_GAME_EXPORT int n64_game_admit(uint32_t machine) {
    return syNetPeerAdmit((int)machine);
}

// why frame() ended the game, for i64's menu ("" = the player quit)
extern "C" N64_GAME_EXPORT void n64_game_exit_message(char* out, size_t cap) {
    std::snprintf(out, cap, "%s", port_net_end_reason());
}

extern "C" void syNetPeerHostPause(int is_paused, int is_initiator);  // decomp sys/netpeer.c
// "everything unlocked" this run (decomp lb/lbbackup.c asks at boot)
extern "C" int port_unlock_all_active(void) { return sUnlockAll ? 1 : 0; }
extern "C" int syNetPeerDropoutIdle(int player);                      // decomp sys/netpeer.c
extern "C" int syNetPeerDropoutCpu(int player, int level);

/* i64's drop-out menu (the host's module): a device left a session of more than two. Its player stays
 * in, with nothing pressed, from a tick every device agrees on. */
extern "C" N64_GAME_EXPORT int n64_game_dropout(uint32_t player, n64_dropout_action action, const uint32_t* sel,
                                                uint32_t count) {
    if (player >= 4) return 1;
    if (action == N64_DROPOUT_CPU) return syNetPeerDropoutCpu((int)player, count > 0 ? (int)sel[0] + 1 : 5);
    if (action == N64_DROPOUT_IDLE) return syNetPeerDropoutIdle((int)player);
    return 1;
}

/* The CPU dialog: Smash's own CPU levels 1-9 (option k = level k + 1). */
extern "C" N64_GAME_EXPORT int n64_game_cpu_options(uint32_t player, n64_cpu_options* out) {
    static const char* const kLevels[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
    if (player >= 4) return 0;
    std::memset(out, 0, sizeof(*out));
    out->choice_count = 1;
    out->choices[0].label = "Level";
    out->choices[0].count = 9;
    for (int i = 0; i < 9; i++) out->choices[0].options[i] = kLevels[i];
    out->choices[0].initial = 4;
    return 1;
}

/* i64 keeps the game while its players are in i64's menu: 1 = this player
 * paused, 2 = the friend did, 0 = back into the game. Online, both sides tell
 * each other they're away (no one gives up on the silence); on resume the
 * player who paused presses Start, which opens the pause menu in a running
 * VS battle; in menus the game just carries on. */
extern "C" N64_GAME_EXPORT void n64_game_set_paused(int paused) {
    static int pausedBy = 0;
    if (!sRunning) {
        return;
    }
    if (paused != 0) {
        pausedBy = paused;
        syNetPeerHostPause(1, paused == 1);
    } else {
        syNetPeerHostPause(0, pausedBy == 1);
        pausedBy = 0;
    }
}

extern "C" N64_GAME_EXPORT void n64_game_stop(void) {
    gLusFrameCaptureCallback = nullptr;
    gLusFrameCaptureRequest = 0;
    gPortHidden = 0;
    if (!sRunning) {
        return;
    }
    PortRunEnd();
    gPortHostAudioWrite = nullptr;
    gPortHostAudioBuffered = nullptr;
    gPortNetHostSend = nullptr;
    gPortNetHostRecv = nullptr;
    gPortNetHostRecvFrom = nullptr;
    LUS_SetExternalWindow(nullptr, nullptr);
    port_reentry_release_caches();
    port_coroutine_pool_release_for_reentry();   /* (its stacks are in i64's arena) */
    port_reentry_restore_pristine();
    sRunning = false;
    gLusHostPadRead = nullptr;
    sHost = nullptr;
}

#endif /* SSB64_I64_MODULE */
