/* Rollback snapshots: save and restore the whole VS simulation between frames.
 *
 * A snapshot is the game's mutable globals (the __SSB64SNAP data and bss
 * sections, see snapshot_section.h), the used part of the scene arena's
 * general heap, and the used part of every live coroutine stack. Restoring
 * copies them back; suspended game threads then resume exactly where they
 * were. Only valid between frames, when no coroutine is running.
 *
 * SSB64_ROLLBACK_SYNCTEST=1 checks it every netplay battle frame: snapshot,
 * run the frame, hash the state, restore, re-run the frame as a
 * resimulation (no new input sampling, network, audio output or drawing),
 * hash again and compare. */
#include "rollback.h"

#include "port_log.h"
#include "bridge/lbreloc_byteswap.h"
#include "resource/RelocPointerTable.h"
#include "rollback/snapshot_section.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#if SSB64_SNAPSHOT_SUPPORTED
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

extern "C" {
int gPortRollbackResim = 0;

struct PortMallocRegion {
    uint32_t id;
    void *start;
    void *end;
    void *ptr;
};
extern PortMallocRegion gSYTaskmanGeneralHeap;

size_t port_coroutine_snapshot_size(void);
size_t port_coroutine_snapshot_save(uint8_t *dst, size_t capacity);
void port_coroutine_snapshot_restore(const uint8_t *src, size_t size);

void syNetSyncHashTick(uint32_t *out_columns);
unsigned int syNetInputGetTick(void);
int syNetPeerCheckLocalPlayer(int player);
int port_rollback_game_in_battle(void);
}

namespace {

constexpr int kHashColumns = 12; // SYNETSYNC_COLUMN_NUM
const char *const kColumnNames[kHashColumns] = { "full",  "rng",    "battle", "fighters", "items",  "weapons",
                                                 "stage", "objman", "input",  "joints",   "camera", "vars" };

struct Section {
    uint8_t *base = nullptr;
    size_t size = 0;
};

struct GameSnapshot {
    std::vector<uint8_t> data;
    std::vector<uint8_t> bss;
    std::vector<uint8_t> arena;
    std::vector<uint8_t> stacks;
    size_t relocMark = 0; // reloc token table journal position
    std::shared_ptr<const PortFixupState> fixups; // in-place fixup trackers
};

Section sData;
Section sBss;
bool sSectionsFound;

bool FindSections() {
#if SSB64_SNAPSHOT_SUPPORTED
    if (!sSectionsFound) {
        unsigned long size = 0;
        sData.base = getsectiondata(&_mh_execute_header, SSB64_SNAPSHOT_SEGMENT, "__data", &size);
        sData.size = size;
        sBss.base = getsectiondata(&_mh_execute_header, SSB64_SNAPSHOT_SEGMENT, "__bss", &size);
        sBss.size = size;
        sSectionsFound = (sData.base != nullptr && sBss.base != nullptr);
        if (sSectionsFound) {
            port_log("SSB64 Rollback: snapshot sections data=%zuKB bss=%zuKB\n", sData.size / 1024, sBss.size / 1024);
        }
    }
#endif
    return sSectionsFound;
}

void Save(GameSnapshot &snap) {
    snap.data.assign(sData.base, sData.base + sData.size);
    snap.bss.assign(sBss.base, sBss.base + sBss.size);

    const auto *start = static_cast<const uint8_t *>(gSYTaskmanGeneralHeap.start);
    const auto *ptr = static_cast<const uint8_t *>(gSYTaskmanGeneralHeap.ptr);
    snap.arena.assign(start, ptr);

    snap.stacks.resize(port_coroutine_snapshot_size());
    snap.stacks.resize(port_coroutine_snapshot_save(snap.stacks.data(), snap.stacks.size()));
    snap.relocMark = portRelocJournalMark();
    snap.fixups = portFixupStateCapture();
}

void Restore(const GameSnapshot &snap) {
    auto *arenaEnd = static_cast<uint8_t *>(gSYTaskmanGeneralHeap.ptr);

    std::memcpy(sData.base, snap.data.data(), sData.size);
    std::memcpy(sBss.base, snap.bss.data(), sBss.size);

    // The heap is a bump allocator over a zeroed arena: put back what was
    // allocated then, and re-zero what has been allocated since so later
    // allocations see the same memory they did the first time.
    auto *start = static_cast<uint8_t *>(gSYTaskmanGeneralHeap.start);
    std::memcpy(start, snap.arena.data(), snap.arena.size());
    if (arenaEnd > start + snap.arena.size()) {
        std::memset(start + snap.arena.size(), 0, arenaEnd - (start + snap.arena.size()));
    }

    port_coroutine_snapshot_restore(snap.stacks.data(), snap.stacks.size());
    portRelocJournalUndoTo(snap.relocMark);
    portFixupStateRestore(snap.fixups);
}

// SyncTest state: a ring of pre-frame snapshots and post-frame hashes,
// indexed by battle frame number.
constexpr int kRing = 16;
GameSnapshot sRing[kRing];
uint32_t sRingHash[kRing][kHashColumns];
uint32_t sFrame;          // battle frames run so far
uint32_t sValidFrames;    // consecutive frames with a ring snapshot
int sSyncTestEnabled = -1;
int sSyncTestDepth = 1;   // frames rewound per check
bool sSyncTestArmed;
uint32_t sSyncTestFrames;
uint32_t sSyncTestMismatches;
double sSaveUsecs, sRestoreUsecs, sResimUsecs;

double UsecsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t).count();
}

void SaveTimed(GameSnapshot &snap) {
    auto t = std::chrono::steady_clock::now();
    Save(snap);
    sSaveUsecs += UsecsSince(t);
}

void ReportMismatch(uint32_t frame, const uint32_t *expected, const uint32_t *actual) {
    uint32_t mask = 0;
    for (int i = 0; i < kHashColumns; i++) {
        if (expected[i] != actual[i]) {
            mask |= 1u << i;
        }
    }
    if (mask == 0) {
        return;
    }
    sSyncTestMismatches++;
    if (sSyncTestMismatches <= 20) {
        char cols[160] = "";
        for (int i = 0; i < kHashColumns; i++) {
            if (mask & (1u << i)) {
                std::strncat(cols, kColumnNames[i], sizeof(cols) - std::strlen(cols) - 2);
                std::strncat(cols, " ", sizeof(cols) - std::strlen(cols) - 1);
            }
        }
        port_log("SSB64 Rollback: SYNCTEST MISMATCH frame=%u (rewound %d) tick=%u columns=%s\n", frame,
                 sSyncTestDepth, syNetInputGetTick(), cols);
    }
}

} // namespace

extern "C" int port_rollback_synctest_begin(void) {
    if (sSyncTestEnabled < 0) {
        const char *env = std::getenv("SSB64_ROLLBACK_SYNCTEST");
        sSyncTestEnabled = (env != nullptr) ? std::atoi(env) : 0;
        const char *depth = std::getenv("SSB64_ROLLBACK_SYNCTEST_FRAMES");
        if (depth != nullptr) {
            sSyncTestDepth = std::max(1, std::min(kRing - 1, std::atoi(depth)));
        }
    }
    sSyncTestArmed = false;
    if (!sSyncTestEnabled || !FindSections()) {
        return 0;
    }
    static bool journaling;
    if (!journaling) {
        journaling = true;
        portRelocJournalSetEnabled(1);
        port_log("SSB64 Rollback: synctest rewinding %d frame(s) every frame\n", sSyncTestDepth);
    }
    // Netplay VS battle, past the start barrier.
    if ((!syNetPeerCheckLocalPlayer(0) && !syNetPeerCheckLocalPlayer(1)) || syNetInputGetTick() < 1 ||
        !port_rollback_game_in_battle()) {
        sValidFrames = 0;
        return 0;
    }
    SaveTimed(sRing[sFrame % kRing]);
    sSyncTestArmed = true;
    return 1;
}

extern "C" void port_rollback_synctest_end(void (*resimulate_frame)(void)) {
    if (!sSyncTestArmed) {
        return;
    }
    sSyncTestArmed = false;
    if (!port_rollback_game_in_battle()) {
        sValidFrames = 0; // the frame ended the battle; nothing to compare
        return;
    }
    syNetSyncHashTick(sRingHash[sFrame % kRing]);
    const unsigned int tick = syNetInputGetTick();
    sValidFrames++;
    sFrame++;

    if (sValidFrames < (uint32_t)sSyncTestDepth) {
        return;
    }
    // Rewind to before frame (sFrame - depth) and replay up to the present,
    // re-taking each snapshot on the way as rollback itself will.
    const uint32_t first = sFrame - sSyncTestDepth;
    auto t = std::chrono::steady_clock::now();
    Restore(sRing[first % kRing]);
    sRestoreUsecs += UsecsSince(t);

    t = std::chrono::steady_clock::now();
    gPortRollbackResim = 1;
    for (uint32_t f = first; f < sFrame; f++) {
        if (f != first) {
            Save(sRing[f % kRing]);
        }
        resimulate_frame();
        uint32_t actual[kHashColumns];
        syNetSyncHashTick(actual);
        ReportMismatch(f, sRingHash[f % kRing], actual);
    }
    gPortRollbackResim = 0;
    sResimUsecs += UsecsSince(t);
    sSyncTestFrames++;

    if (syNetInputGetTick() != tick) {
        sSyncTestMismatches++;
        port_log("SSB64 Rollback: SYNCTEST tick mismatch %u vs %u\n", syNetInputGetTick(), tick);
    }
    // Keep reloc journal history only as far back as the oldest snapshot.
    portRelocJournalForgetBefore(sRing[(sFrame - std::min<uint32_t>(sValidFrames, kRing - 1)) % kRing].relocMark);

    if (sSyncTestFrames % 600 == 0) {
        const GameSnapshot &snap = sRing[(sFrame - 1) % kRing];
        port_log("SSB64 Rollback: synctest checks=%u depth=%d mismatches=%u avg save=%.0fus restore=%.0fus "
                 "resim(all %d)=%.0fus snapshot=%zuKB (arena %zuKB, stacks %zuKB)\n",
                 sSyncTestFrames, sSyncTestDepth, sSyncTestMismatches, sSaveUsecs / sSyncTestFrames,
                 sRestoreUsecs / sSyncTestFrames, sSyncTestDepth, sResimUsecs / sSyncTestFrames,
                 (snap.data.size() + snap.bss.size() + snap.arena.size() + snap.stacks.size()) / 1024,
                 snap.arena.size() / 1024, snap.stacks.size() / 1024);
    }
}
