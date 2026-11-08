#include "enhancements.h"
#include "net/netplay_settings.h"
#include <libultraship/bridge/consolevariablebridge.h>

namespace ssb64 {
    namespace enhancements {

        const char* CpuLevel9CVarName() {
            return "gEnhancements.CpuLevel9";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int port_enhancement_cpu_level_9(void) {
    return port_netplay_setting(ssb64::enhancements::CpuLevel9CVarName(), CVarGetInteger(ssb64::enhancements::CpuLevel9CVarName(), 0)) != 0;
}
