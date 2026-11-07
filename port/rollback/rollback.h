#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Non-zero while frames are being resimulated after a rollback: input is
 * read from the ledger instead of sampled, and the network, audio output and
 * drawing are skipped. */
extern int gPortRollbackResim;

/* SSB64_ROLLBACK_SYNCTEST=1: call begin before running a frame and end
 * after; end restores the pre-frame snapshot, resimulates the frame with the
 * callback and compares state hashes. begin returns 0 when not testing. */
int port_rollback_synctest_begin(void);
void port_rollback_synctest_end(void (*resimulate_frame)(void));

/* SSB64_NETPLAY_ROLLBACK=1: call begin before running a battle frame (it
 * performs any pending rollback with the callback, then snapshots) and end
 * after it. begin returns 0 when rollback is not active. */
int port_rollback_frame_begin(void (*resimulate_frame)(void));
void port_rollback_frame_end(void);

#ifdef __cplusplus
}
#endif
