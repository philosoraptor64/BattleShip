/* Gameplay-affecting settings in an online session.
 *
 * Settings that change the rules or the flow of the game must be identical
 * on both machines, or the simulations drift apart. At session start the host
 * snapshots them (sys/netpeer.c sends the snapshot with the session data) and
 * both machines read the snapshot until the session ends; nothing is written
 * to either player's settings file. Purely visual or input-shaping options
 * (hitboxes, HUD, stick remap, D-pad jump, C-stick smash) stay personal: the
 * input options are applied before inputs are sent.
 *
 * The list's order is part of the netplay protocol: append only. */
#include "netplay_settings.h"

#include <libultraship/bridge/consolevariablebridge.h>

#include <cstring>

namespace {
struct Setting {
    const char *cvar;
    int fallback;
};
const Setting kSettings[] = {
    { "gEnhancements.CpuLevel9", 0 },
    { "gEnhancements.NeutralSpawns", 0 },
    { "gEnhancements.BonusStagesEnabled", 1 },
    { "gEnhancements.SkipResultsScreen", 0 },
    { "gEnhancements.ClassicCoop", 1 },
    { "gEnhancements.ClassicCoopFriendlyFire", 0 },
    { "gEnhancements.CompRuleset", 0 },
    { "gEnhancements.StageHazardsDisabled", 0 },
    { "gEnhancements.TapJumpDisabled.P1", 0 },
    { "gEnhancements.TapJumpDisabled.P2", 0 },
    { "gEnhancements.TapJumpDisabled.P3", 0 },
    { "gEnhancements.TapJumpDisabled.P4", 0 },
};
constexpr int kCount = sizeof(kSettings) / sizeof(kSettings[0]);
static_assert(kCount <= PORT_NETPLAY_SETTINGS_MAX, "netplay settings list too long");

bool sActive = false;
int32_t sValues[PORT_NETPLAY_SETTINGS_MAX] = {};
} // namespace

extern "C" void port_netplay_settings_capture(int32_t *out) {
    for (int i = 0; i < PORT_NETPLAY_SETTINGS_MAX; i++) {
        out[i] = (i < kCount) ? CVarGetInteger(kSettings[i].cvar, kSettings[i].fallback) : 0;
    }
}

extern "C" void port_netplay_settings_begin(const int32_t *values) {
    std::memcpy(sValues, values, sizeof(sValues));
    sActive = true;
}

extern "C" void port_netplay_settings_end(void) {
    sActive = false;
}

extern "C" int port_netplay_setting(const char *cvar, int value) {
    if (!sActive || cvar == nullptr) {
        return value;
    }
    for (int i = 0; i < kCount; i++) {
        if (std::strcmp(kSettings[i].cvar, cvar) == 0) {
            return sValues[i];
        }
    }
    return value;
}
