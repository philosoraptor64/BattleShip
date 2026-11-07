#include "enhancements.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/controller/controldevice/controller/Controller.h>
#include <ship/controller/controldevice/controller/ControllerStick.h>
#include <ship/controller/controldevice/controller/mapping/ControllerAxisDirectionMapping.h>

#include <algorithm>
#include <cmath>

namespace {

// Per-player NRage-style analog-stick remap. When the enable cvar for a player
// is on, src/sys/controller.c discards the libultraship Process()'d int8_t
// stick output and recomputes the value from the raw per-direction axis
// magnitudes via the per-axis formula in
// usbh_xpad.c (Ownasaurus/USBtoN64v2). Disabled by default; the LUS pipeline
// (circular deadzone + octagonal gate + notch snap) runs unchanged when off.
// Enable key has to live UNDER PX (as a sibling of Deadzone/Range), not AT PX.
// The config layer stores cvars in flattened JSON-pointer form and unflattens
// on every save; if PX were both a scalar (the enable flag) and a parent of
// PX.Deadzone / PX.Range, nlohmann's unflatten throws type_error.313 on the
// next save and Config::Save truncates the file to empty before serializing,
// wiping the user's settings (issue #96).
constexpr const char* kAnalogRemapEnableCVars[PORT_ENHANCEMENT_MAX_PLAYERS] = {
    "gEnhancements.AnalogRemap.P1.Enabled",
    "gEnhancements.AnalogRemap.P2.Enabled",
    "gEnhancements.AnalogRemap.P3.Enabled",
    "gEnhancements.AnalogRemap.P4.Enabled",
};
constexpr const char* kAnalogRemapDeadzoneCVars[PORT_ENHANCEMENT_MAX_PLAYERS] = {
    "gEnhancements.AnalogRemap.P1.Deadzone",
    "gEnhancements.AnalogRemap.P2.Deadzone",
    "gEnhancements.AnalogRemap.P3.Deadzone",
    "gEnhancements.AnalogRemap.P4.Deadzone",
};
constexpr const char* kAnalogRemapRangeCVars[PORT_ENHANCEMENT_MAX_PLAYERS] = {
    "gEnhancements.AnalogRemap.P1.Range",
    "gEnhancements.AnalogRemap.P2.Range",
    "gEnhancements.AnalogRemap.P3.Range",
    "gEnhancements.AnalogRemap.P4.Range",
};

// Up-only overrides, for sticks whose spring leaves them resting partway up
// after an up flick (which blocks tap jumps: they need |y| < 20 first).
// UpDeadzone < 0 means "same as Deadzone". UpRecenter: see RecenterUp().
constexpr const char* kAnalogRemapUpDeadzoneCVars[PORT_ENHANCEMENT_MAX_PLAYERS] = {
    "gEnhancements.AnalogRemap.P1.UpDeadzone",
    "gEnhancements.AnalogRemap.P2.UpDeadzone",
    "gEnhancements.AnalogRemap.P3.UpDeadzone",
    "gEnhancements.AnalogRemap.P4.UpDeadzone",
};
constexpr const char* kAnalogRemapUpRecenterCVars[PORT_ENHANCEMENT_MAX_PLAYERS] = {
    "gEnhancements.AnalogRemap.P1.UpRecenter",
    "gEnhancements.AnalogRemap.P2.UpRecenter",
    "gEnhancements.AnalogRemap.P3.UpRecenter",
    "gEnhancements.AnalogRemap.P4.UpRecenter",
};

// Recentering. Some controllers (seen on an Xbox Wireless Controller over
// Bluetooth on iOS) report up as slowly creeping back toward center after an
// up flick -- from full tilt to ~45% over tens of seconds -- while the stick
// is physically centered, and up also creeps away from center (to ~45%)
// while untouched. The ghost is slow and steady; a thumb is fast, pinned or
// jittery. After kRecenterFallTics consecutive small falls, or
// kRecenterRiseTics consecutive tiny rises, the current value becomes up's
// zero, so the next flick starts fresh. The zero follows the stick down and
// clears near center or on any down input. Unchanged ticks (no new report)
// keep both streaks.
//
// A fast rise (>= kRecenterFlickRise in one tick) is a new flick: up's zero
// moves to where the rise started. If up was already non-neutral, that tick
// reports 0 so the game sees the stick leave neutral (tap jump requires
// |y| < 20 the tick before) -- one tick of latency, only for such flicks.
//
// Up can also drop part-way on release and then freeze (e.g. at 45%). So a
// fast fall (a release) is assumed to reach neutral: its value becomes the
// zero at once, and only a new rise reads as up again. A value frozen for
// kRecenterStillTics below full tilt becomes the zero too.
constexpr float kRecenterMaxFall  = 3.0f; // per tick, of 85
constexpr float kRecenterMinSpan  = 3.0f;  // travel left above a zero near full tilt
constexpr float kRecenterStill     = 0.3f;  // per tick, of 85: "frozen" report
constexpr int   kRecenterStillTics = 8;
constexpr float kRecenterFullHold  = 80.0f; // frozen at/above this = a real full-tilt hold
constexpr int   kRecenterFallTics = 4;
constexpr float kRecenterMaxRise  = 0.5f; // per tick, of 85
constexpr int   kRecenterRiseTics = 20;
constexpr float kRecenterClear    = 4.0f;
constexpr float kRecenterFlickRise = 8.0f; // per tick, of 85

struct UpRecenterState {
    float rest;
    float last;
    int   falls;
    int   rises;
    int   still;
    float out;
    bool  flicking;
};
UpRecenterState sUpRecenter[PORT_ENHANCEMENT_MAX_PLAYERS];

float RecenterUp(int player_index, float up, float down) {
    UpRecenterState& st = sUpRecenter[player_index];

    if (up < kRecenterClear || down > 0.0f) {
        st = {};
        return up;
    }
    const float fall = st.last - up;
    if (-fall >= kRecenterFlickRise) {
        // Consecutive fast rises are one flick: only its first tick re-zeroes.
        if (!st.flicking) {
            const bool was_up = st.out >= kRecenterClear;
            st.rest     = st.last;
            st.flicking = true;
            st.last     = up;
            st.falls = st.rises = 0;
            if (was_up) {
                st.out = 0.0f;
                return st.out;
            }
        }
        st.last = up;
        st.out  = std::max(0.0f, (up - st.rest) * 85.0f / std::max(85.0f - st.rest, kRecenterMinSpan));
        return st.out;
    }
    st.flicking = false;
    st.still    = (std::fabs(fall) < kRecenterStill) ? st.still + 1 : 0;
    const bool released = fall >= kRecenterMaxFall;
    if (fall > 0.0f) {
        st.falls = released ? 0 : st.falls + 1;
        st.rises = 0;
    } else if (fall < 0.0f) {
        st.rises = (-fall < kRecenterMaxRise) ? st.rises + 1 : 0;
        st.falls = 0;
    }
    st.last = up;
    const bool frozen = st.still >= kRecenterStillTics && up < kRecenterFullHold;
    if (released || st.falls >= kRecenterFallTics || st.rises >= kRecenterRiseTics || frozen || up < st.rest) {
        st.rest = up;
    }
    st.out = std::max(0.0f, (up - st.rest) * 85.0f / std::max(85.0f - st.rest, kRecenterMinSpan));
    return st.out;
}

constexpr float kAnalogRemapDeadzoneDefault = 0.10f;
constexpr float kAnalogRemapRangeDefault    = 1.00f;

} // namespace

// NRage-style per-axis stick remap. See header comment for the design.
//
// Reads the raw per-direction normalized magnitudes from libultraship
// (0..MAX_AXIS_RANGE per direction) and reconstructs a signed per-axis value,
// then applies the verbatim usbh_xpad.c formula independently to X and Y:
//   - linear deadzone subtraction with rescale gain
//   - asymmetric negative branch (`-(in+1)`), faithful to the source
//   - linear range scale capped at 127
// No octagonal gate, no radial deadzone, no notch snap.
//
// When the enable cvar is off, this is a no-op so libultraship's Process()
// output flows through to the game unchanged.
extern "C" void port_enhancement_analog_remap(int player_index, signed char* stick_x, signed char* stick_y) {
    if (player_index < 0 || player_index >= PORT_ENHANCEMENT_MAX_PLAYERS) return;
    if (!CVarGetInteger(kAnalogRemapEnableCVars[player_index], 0)) return;

    auto ctx = Ship::Context::GetInstance();
    if (!ctx) return;
    auto deck = ctx->GetControlDeck();
    if (!deck) return;
    auto controller = deck->GetControllerByPort(static_cast<uint8_t>(player_index));
    if (!controller) return;
    auto stick = controller->GetLeftStick();
    if (!stick) return;

    const float right = stick->GetAxisDirectionValue(Ship::RIGHT);
    const float left  = stick->GetAxisDirectionValue(Ship::LEFT);
    float       up    = stick->GetAxisDirectionValue(Ship::UP);
    const float down  = stick->GetAxisDirectionValue(Ship::DOWN);

    if (CVarGetInteger(kAnalogRemapUpRecenterCVars[player_index], 0)) {
        up = RecenterUp(player_index, up, down);
    }

    // libultraship hands us per-direction magnitudes in [0, RAW_MAX] where
    // RAW_MAX is MAX_AXIS_RANGE (85.0f). The Ownasaurus/USBtoN64v2 firmware,
    // by contrast, runs the formula on the int16 USB stick value directly
    // (XPAD_MAX = 32767). Run the formula in the firmware's domain so it
    // is byte-faithful to usbh_xpad.c — including the `-(in+1)` asymmetry,
    // which is meaningful at int16 scale (one LSB of 32767, present to
    // protect against negating INT16_MIN) but would explode to ~1% of full
    // scale if applied to LUS's [-85, +85] floats directly. Output naturally
    // lands in [-127, +127] like the firmware.
    const float in_x = (right - left) * (32767.0f / 85.0f);
    const float in_y = (up - down)    * (32767.0f / 85.0f);

    constexpr float XPAD_MAX = 32767.0f;
    const float dz_pct =
        std::clamp(CVarGetFloat(kAnalogRemapDeadzoneCVars[player_index], kAnalogRemapDeadzoneDefault), 0.0f, 0.99f);
    const float rng_pct =
        std::clamp(CVarGetFloat(kAnalogRemapRangeCVars[player_index], kAnalogRemapRangeDefault), 0.50f, 1.50f);

    const float up_dz_cvar = CVarGetFloat(kAnalogRemapUpDeadzoneCVars[player_index], -1.0f);
    const float up_dz_pct  = (up_dz_cvar < 0.0f) ? dz_pct : std::clamp(up_dz_cvar, 0.0f, 0.99f);

    const float n64_max = 127.0f * rng_pct;

    // Verbatim usbh_xpad.c L260-L303 formula. The firmware truncates with a
    // (uint8_t) cast then unary-negates; at range > ~100% that truncation
    // wraps a >127 result into a small negative value (firmware bug). We
    // clamp to s8 instead so the user's range slider stays monotonic past
    // 100%.
    auto remap_axis = [&](float stick, float pos_dz_pct) -> signed char {
        const float pos_dz_val      = pos_dz_pct * XPAD_MAX;
        const float pos_dz_relation = (XPAD_MAX > pos_dz_val) ? XPAD_MAX / (XPAD_MAX - pos_dz_val) : 1.0f;
        const float dz_val          = dz_pct * XPAD_MAX;
        const float dz_relation     = (XPAD_MAX > dz_val) ? XPAD_MAX / (XPAD_MAX - dz_val) : 1.0f;
        float out;
        if (stick >= pos_dz_val) {
            float unscaled = (stick - pos_dz_val) * pos_dz_relation;
            out = unscaled * (n64_max / XPAD_MAX);
        } else if (stick <= -dz_val) {
            float temp     = -(stick + 1.0f);
            float unscaled = (temp - dz_val) * dz_relation;
            out            = -(unscaled * (n64_max / XPAD_MAX));
        } else {
            return 0;
        }
        if (out >  127.0f) out =  127.0f;
        if (out < -127.0f) out = -127.0f;
        return static_cast<signed char>(out);
    };

    *stick_x = remap_axis(in_x, dz_pct);
    *stick_y = remap_axis(in_y, up_dz_pct);
}

namespace ssb64 {
namespace enhancements {

const char* AnalogRemapCVarName(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= PORT_ENHANCEMENT_MAX_PLAYERS) {
        return kAnalogRemapEnableCVars[0];
    }
    return kAnalogRemapEnableCVars[playerIndex];
}

const char* AnalogRemapDeadzoneCVarName(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= PORT_ENHANCEMENT_MAX_PLAYERS) {
        return kAnalogRemapDeadzoneCVars[0];
    }
    return kAnalogRemapDeadzoneCVars[playerIndex];
}

const char* AnalogRemapRangeCVarName(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= PORT_ENHANCEMENT_MAX_PLAYERS) {
        return kAnalogRemapRangeCVars[0];
    }
    return kAnalogRemapRangeCVars[playerIndex];
}

} // namespace enhancements
} // namespace ssb64
