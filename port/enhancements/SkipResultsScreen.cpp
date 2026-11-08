#include "enhancements.h"
#include "net/netplay_settings.h"
#include <libultraship/bridge/consolevariablebridge.h>

namespace ssb64 {
    namespace enhancements {

        const char* SkipResultsScreenCVarName() {
            return "gEnhancements.SkipResultsScreen";
        }

    } // namespace enhancements
} // namespace ssb64

extern "C" int port_enhancement_skip_results_screen(void) {
    return port_netplay_setting(ssb64::enhancements::SkipResultsScreenCVarName(), CVarGetInteger(ssb64::enhancements::SkipResultsScreenCVarName(), 0)) != 0;
}
