/* Re-entry support: start and stop the game several times in one process
 * (see reentry.cpp). */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in main(): copy the program's writable globals as they are. */
void port_reentry_save_pristine(void);
/* After a full shutdown: free global caches the engine's shutdown keeps. */
void port_reentry_release_caches(void);
/* After a full shutdown: put every global back to that copy. */
void port_reentry_restore_pristine(void);
unsigned long port_reentry_saved_bytes(void);
/* Physical footprint (KB) and thread count of the process right now. */
void port_reentry_stats(unsigned long *footprint_kb, unsigned int *threads);

#ifdef __cplusplus
}
#endif
