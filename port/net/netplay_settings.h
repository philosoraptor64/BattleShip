/* Gameplay settings shared by an online session (port/net/netplay_settings.cpp).
 * C interface, used by sys/netpeer.c and the enhancement getters. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_NETPLAY_SETTINGS_MAX 16

/* Fills `out` (PORT_NETPLAY_SETTINGS_MAX values) with this machine's current
 * values of the gameplay-affecting settings, in the fixed list's order. */
void port_netplay_settings_capture(int32_t *out);
/* For the rest of the session every listed setting reads these values (the
 * host's), whatever this machine's own settings say. */
void port_netplay_settings_begin(const int32_t *values);
void port_netplay_settings_end(void);
/* `value` (the local setting) unless a session overrides `cvar`. */
int port_netplay_setting(const char *cvar, int value);

#ifdef __cplusplus
}
#endif
