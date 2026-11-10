/* Smash as an i64 module on iPhone: the stand-alone app's UIKit glue
 * (port/port_ios*.mm, port/port_ios.cpp: start menu, status overlay, voice
 * button, invite links, scene life cycle, staging game data into Documents)
 * is left out: inside i64 those are i64's. The game still calls a few of
 * those functions; here they do nothing (or log). */
#if defined(SSB64_I64_MODULE)
#include <TargetConditionals.h>
#if TARGET_OS_IOS

extern "C" {
void port_log(const char* fmt, ...);

void port_ios_prepare(void) {
    /* game data comes from i64 (data_dir, bundle_dir), netplay from launch */
}

void port_ios_wait_while_inactive(void) {
    /* i64 stops calling n64_game_frame while the app is inactive */
}

void port_ios_status_show(const char* title, const char* text) {
    port_log("SSB64 i64: status: %s: %s\n", title ? title : "", text ? text : "");
}

void port_ios_status_hide(void) {
}

void port_ios_alert(const char* text) {
    port_log("SSB64 i64: alert: %s\n", text ? text : "");
}

void port_ios_voice_button_show(void) {
    /* voice is i64's */
}

void port_ios_voice_button_hide(void) {
}
} // extern "C"

#endif /* TARGET_OS_IOS */
#endif /* SSB64_I64_MODULE */
