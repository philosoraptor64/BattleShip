/* BattleShip glue for n64net voice chat (n64net_voice.h).
 *
 * sys/netpeer.c calls port_voice_pump() from its receive loop (every tick and
 * while waiting for the peer) and port_voice_handle_packet() for each
 * datagram, and port_voice_stop() when the connection closes. Voice starts
 * on the first pump of a connection, if enabled:
 *   SSB64_NETPLAY_VOICE=1 (open mic) | ptt (push to talk) | 0/unset (off)
 *   SSB64_NETPLAY_VOICE_TEST=1   a generated tone instead of the microphone
 * Voice state lives here (C++), outside the rollback snapshot. */
#include "n64net_voice.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" {
void port_log(const char *fmt, ...);
void syNetPeerSendBytes(const uint8_t *buffer, uint32_t size);
void portAudioVoiceKeepAlive(void);
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
void port_ios_voice_button_show(void);
void port_ios_voice_button_hide(void);
#endif
#endif
}

namespace {
bool sTriedThisConnection = false;

bool EnvTrue(const char *name) {
    const char *v = getenv(name);
    return v != nullptr && v[0] != '\0' && strcmp(v, "0") != 0;
}
} // namespace

extern "C" void port_voice_pump(void) {
    if (!sTriedThisConnection) {
        sTriedThisConnection = true;
        const char *mode = getenv("SSB64_NETPLAY_VOICE");
        n64net_voice_config config = {};
        config.enabled = EnvTrue("SSB64_NETPLAY_VOICE");
        config.push_to_talk = (mode != nullptr && strcmp(mode, "ptt") == 0);
        config.test_tone = EnvTrue("SSB64_NETPLAY_VOICE_TEST");
        config.log = port_log;
        if (!config.enabled) {
            port_log("n64net voice: off (SSB64_NETPLAY_VOICE not set)\n");
        }
        n64net_voice_start(&config);
#if defined(__APPLE__) && TARGET_OS_IOS
        if (n64net_voice_is_running()) {
            port_ios_voice_button_show();
        }
#endif
    }
    n64net_voice_pump(syNetPeerSendBytes);
    portAudioVoiceKeepAlive();
}

extern "C" int port_voice_handle_packet(const uint8_t *data, int32_t size) {
    return n64net_voice_handle_packet(data, size);
}

extern "C" void port_voice_stop(void) {
#if defined(__APPLE__) && TARGET_OS_IOS
    if (n64net_voice_is_running()) {
        port_ios_voice_button_hide();
    }
#endif
    n64net_voice_stop();
    sTriedThisConnection = false;
}
