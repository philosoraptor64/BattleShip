#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* iOS: stage bundled game data into Documents, load Documents/netplay.cfg
 * into the environment and chdir there. Call first thing in main(). No-op
 * on every other platform. */
void port_ios_prepare(void);

#ifdef __cplusplus
}
#endif
