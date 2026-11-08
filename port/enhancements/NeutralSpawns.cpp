#include "enhancements.h"
#include "net/netplay_settings.h"
#include <libultraship/bridge/consolevariablebridge.h>

namespace ssb64 {
    namespace enhancements {

        const char* NeutralSpawnsCVarName() {
            return "gEnhancements.NeutralSpawns";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int port_enhancement_neutral_spawns(void) {
    return port_netplay_setting(ssb64::enhancements::NeutralSpawnsCVarName(), CVarGetInteger(ssb64::enhancements::NeutralSpawnsCVarName(), 0)) != 0;
}
