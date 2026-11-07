/* Rollback snapshot section placement.
 *
 * Force-included into every ssb64_game C file (see CMakeLists.txt). It puts
 * the game's mutable globals -- including function-static locals -- into
 * dedicated sections, so port/rollback/snapshot.cpp can save and restore the
 * whole simulation with two memcpys instead of a hand-maintained list.
 *
 * Files whose state must survive a rollback (the netplay transport, the
 * replay recorder) or that only hold render output are compiled with
 * SSB64_SNAPSHOT_EXCLUDE and keep the default sections. Inside a snapshotted
 * file, SSB64_SNAPSHOT_OUTSIDE moves one global back out (the input ledger).
 *
 * Supported: Apple (Mach-O). Elsewhere this is a no-op and rollback is off. */
#ifndef SSB64_SNAPSHOT_SECTION_H
#define SSB64_SNAPSHOT_SECTION_H

#if defined(__APPLE__) && defined(__clang__)
#define SSB64_SNAPSHOT_SUPPORTED 1
#define SSB64_SNAPSHOT_SEGMENT "__SSB64SNAP"
#define SSB64_SNAPSHOT_OUTSIDE __attribute__((section("__DATA,__bss")))
/* For port C/C++ state that belongs to the simulation (zero-initialized). */
#define SSB64_SNAPSHOT_INSIDE __attribute__((section("__SSB64SNAP,__bss")))
/* Game code is C. Port C++ files include this header only for the macros
 * and must keep their own state (the snapshots themselves) outside. */
#if !defined(SSB64_SNAPSHOT_EXCLUDE) && !defined(__cplusplus)
#pragma clang section bss = "__SSB64SNAP,__bss" data = "__SSB64SNAP,__data"
#endif
#else
#define SSB64_SNAPSHOT_SUPPORTED 0
#define SSB64_SNAPSHOT_OUTSIDE
#define SSB64_SNAPSHOT_INSIDE
#endif

#endif /* SSB64_SNAPSHOT_SECTION_H */
