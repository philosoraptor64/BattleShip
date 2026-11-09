/* i64host: a minimal stand-in for the i64 launcher, to test a game module
 * (n64_game.h) on the Mac without i64: owns SDL, the window and the audio
 * device, loads the module with dlopen, and starts/runs/stops it several
 * times in one process.
 *
 *   i64host <module.dylib> <data_dir> [runs=3] [frames_per_run=600]
 *           [host|guest <local_udp_port> <peer_udp_port>]
 *
 * With host|guest the runs are ONLINE: the game channel is a UDP socket on
 * 127.0.0.1 to the other i64host (standing in for i64's session), host =
 * player 1, guest = player 2.
 *
 * Logs each run's result plus the process footprint and thread count, so a
 * module that doesn't return to a clean slate shows up as growth. Offline
 * runs only. Build: see tools/i64host/README.md. */
#include "../../port/i64/n64_game.h"

#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <mach/mach.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

SDL_AudioDeviceID sAudio = 0;
uint32_t sAudioRate = 0;

void HostLog(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
}

void AudioWrite(const int16_t *stereo, uint32_t frames, uint32_t rate) {
    if (sAudio == 0 || rate != sAudioRate) {
        if (sAudio != 0) SDL_CloseAudioDevice(sAudio);
        SDL_AudioSpec want{}, have{};
        want.freq = static_cast<int>(rate);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        sAudio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        sAudioRate = rate;
        if (sAudio != 0) SDL_PauseAudioDevice(sAudio, 0);
    }
    if (sAudio != 0) SDL_QueueAudio(sAudio, stereo, frames * 4);
}

uint32_t AudioBuffered(uint32_t rate) {
    if (sAudio == 0 || sAudioRate == 0) return 0;
    const uint64_t frames = SDL_GetQueuedAudioSize(sAudio) / 4;
    return static_cast<uint32_t>(frames * rate / sAudioRate);
}

int sSock = -1;
sockaddr_in sPeer{};
int NetSend(const void *data, uint32_t size) {
    return sendto(sSock, data, size, 0, reinterpret_cast<sockaddr *>(&sPeer), sizeof sPeer) == (ssize_t)size ? 0 : -1;
}
int NetRecv(void *buf, uint32_t cap) {
    const ssize_t n = recvfrom(sSock, buf, cap, 0, nullptr, nullptr);
    return n > 0 ? (int)n : 0;
}
int NetPollEvent(void) { return 0; }
void VoiceSetMuted(int) {}
void VoiceSetTalking(int) {}
int VoiceLocalSpeaking(void) { return 0; }
int VoicePeerSpeaking(void) { return 0; }
void OfferTitleArt(const uint8_t *, uint32_t w, uint32_t h) { HostLog("i64host: title art offered %ux%u\n", w, h); }
bool sExitRequested = false;
void RequestExit(void) { sExitRequested = true; }
void SetAllocHooks(const n64_alloc_hooks *) {}

void Stats(unsigned long *kb, unsigned *threads) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    *kb = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        *kb = static_cast<unsigned long>(info.phys_footprint / 1024);
    }
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    *threads = 0;
    if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
        *threads = n;
        for (mach_msg_type_number_t i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), list[i]);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <module.dylib> <data_dir> [runs] [frames_per_run]\n", argv[0]);
        return 2;
    }
    const int runs = argc > 3 ? std::atoi(argv[3]) : 3;
    const int framesPerRun = argc > 4 ? std::atoi(argv[4]) : 600;

    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("i64host", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                                       SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (win == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    void *mod = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (mod == nullptr) {
        std::fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }
    auto getInfo = reinterpret_cast<const n64_game_info *(*)(void)>(dlsym(mod, "n64_game_get_info"));
    auto start = reinterpret_cast<int (*)(const n64_host *, const n64_launch *, char *, size_t)>(dlsym(mod, "n64_game_start"));
    auto frame = reinterpret_cast<n64_frame_result (*)(void)>(dlsym(mod, "n64_game_frame"));
    auto stop = reinterpret_cast<void (*)(void)>(dlsym(mod, "n64_game_stop"));
    if (!getInfo || !start || !frame || !stop) {
        std::fprintf(stderr, "module is missing n64_game_* exports\n");
        return 1;
    }
    const n64_game_info *info = getInfo();
    std::fprintf(stderr, "i64host: %s (%s) abi %u build %s, %u fps\n", info->title, info->id, info->abi_version,
                 info->netplay_build_id, info->target_fps);

    n64_host host{};
    host.abi_version = N64_GAME_ABI_VERSION;
    host.data_dir = argv[2];
    host.bundle_dir = argv[2];
    host.window = win;
    host.audio_write = AudioWrite;
    host.audio_buffered = AudioBuffered;
    host.net_max_datagram = 1200;
    host.net_send = NetSend;
    host.net_recv = NetRecv;
    host.net_poll_event = NetPollEvent;
    host.voice_set_muted = VoiceSetMuted;
    host.voice_set_talking = VoiceSetTalking;
    host.voice_local_speaking = VoiceLocalSpeaking;
    host.voice_peer_speaking = VoicePeerSpeaking;
    host.offer_title_art = OfferTitleArt;
    host.request_exit = RequestExit;
    host.set_alloc_hooks = SetAllocHooks;
    host.log = HostLog;

    n64_launch launch{};
    launch.abi_version = N64_GAME_ABI_VERSION;
    launch.mode = N64_LAUNCH_OFFLINE;
    if (argc > 7) {
        const bool isHost = std::strcmp(argv[5], "host") == 0;
        sSock = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in me{};
        me.sin_family = AF_INET;
        me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        me.sin_port = htons((uint16_t)std::atoi(argv[6]));
        if (bind(sSock, reinterpret_cast<sockaddr *>(&me), sizeof me) != 0) {
            std::perror("bind");
            return 1;
        }
        fcntl(sSock, F_SETFL, fcntl(sSock, F_GETFL, 0) | O_NONBLOCK);
        sPeer.sin_family = AF_INET;
        sPeer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sPeer.sin_port = htons((uint16_t)std::atoi(argv[7]));
        launch.mode = N64_LAUNCH_ONLINE;
        launch.local_player = isHost ? 0 : 1;
        launch.num_players = 2;
        launch.seed = 12345;
        launch.peer_name = isHost ? "guest" : "host";
        std::fprintf(stderr, "i64host: online as %s, udp %s -> %s\n", isHost ? "host" : "guest", argv[6], argv[7]);
    }

    for (int run = 1; run <= runs; run++) {
        char err[256] = "";
        if (start(&host, &launch, err, sizeof err) != 0) {
            std::fprintf(stderr, "i64host: run %d: start failed: %s\n", run, err);
            return 1;
        }
        int frames = 0;
        while (frames < framesPerRun && frame() == N64_GAME_FRAME_CONTINUE) {
            frames++;
        }
        stop();
        launch.run++;
        // Between runs the app's run loop goes on (i64's would), letting
        // AppKit and the audio device settle.
        for (int i = 0; i < 10; i++) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        unsigned long kb;
        unsigned threads;
        Stats(&kb, &threads);
        std::fprintf(stderr, "i64host: run %d/%d: %d frames, footprint %lu KB, threads %u\n", run, runs, frames, kb,
                     threads);
    }
    if (std::getenv("I64HOST_HOLD") != nullptr) {  // keep the process for vmmap/leaks
        std::fprintf(stderr, "i64host: holding (pid %d)\n", (int)getpid());
        for (;;) SDL_Delay(1000);
    }
    SDL_DestroyWindow(win);
    std::fflush(nullptr);
    _Exit(0);  // modules are never unloaded (their static destructors aren't safe)
}
