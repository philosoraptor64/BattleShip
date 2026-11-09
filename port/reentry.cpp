/* Re-entry: run the game several times in one process (for i64, the launcher,
 * where a game module is started and stopped many times while the app runs).
 *
 * At the top of main(), before anything else runs, we copy the program's own
 * writable data (C/C++ globals: __DATA,__data/__bss/__common and the
 * __SSB64SNAP sections that hold the decomp's globals). After a full
 * shutdown (PortGameShutdown + PortShutdown) we copy it back, so the next
 * start sees every global exactly as a fresh process would. Sections the
 * runtime owns (ObjC metadata, GOT, thread-locals) are left alone.
 *
 * State that must survive the reset (this file's own buffers, the soak
 * counters) lives in the __DATA,__ssb64keep section, which is not restored.
 *
 * SSB64_SOAK=N runs N start/run/stop cycles of SSB64_SOAK_FRAMES frames
 * (default 600) and logs memory and thread counts after each one. */
#include "reentry.h"

#include "port_log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
// This image's own Mach-O header: the executable's, or the module's when the
// game is built as a library (i64), where _mh_execute_header is the host's.
extern "C" void *__dso_handle;
static inline const struct mach_header_64 *PortThisImage() {
    return reinterpret_cast<const struct mach_header_64 *>(&__dso_handle);
}
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <cstdio>
#endif

#define KEEP __attribute__((section("__DATA,__ssb64keep")))

namespace {

struct SavedSection {
    uint8_t *base;
    unsigned long size;
    uint8_t *copy;
};

constexpr int kMaxSections = 8;
KEEP SavedSection sSections[kMaxSections] = {};
KEEP int sSectionCount = 0;
KEEP unsigned long sSavedBytes = 0;

void Add(const char *segment, const char *section) {
#if defined(__APPLE__)
    unsigned long size = 0;
    uint8_t *base = getsectiondata(PortThisImage(), segment, section, &size);
    if (base == nullptr || size == 0 || sSectionCount >= kMaxSections) {
        return;
    }
    uint8_t *copy = static_cast<uint8_t *>(std::malloc(size));
    if (copy == nullptr) {
        return;
    }
    std::memcpy(copy, base, size);
    sSections[sSectionCount++] = {base, size, copy};
    sSavedBytes += size;
#else
    (void)segment;
    (void)section;
#endif
}

} // namespace

static void SealPreMainBlocks(void);
static void RestorePreMainBlocks(void);

extern "C" void port_reentry_save_pristine(void) {
    if (sSectionCount != 0) {
        return;  // only the first, pristine state
    }
    SealPreMainBlocks();
    Add("__DATA", "__data");
    Add("__DATA", "__bss");
    Add("__DATA", "__common");
    Add("__SSB64SNAP", "__data");
    Add("__SSB64SNAP", "__bss");
}

extern "C" void portGfxReleaseForReentry(void);
extern "C" void portRelocTableReleaseForReentry(void);
extern "C" void portParticleBankReleaseForReentry(void);
extern "C" void portFixupReleaseForReentry(void);
extern "C" void portRollbackReleaseForReentry(void);
extern "C" void port_dl_range_unregister(const void *base);
extern "C" void *gPortSceneHeap;

/* After shutdown, before the globals are reset: free what the engine's own
 * shutdown leaves in global caches (resetting the globals would only orphan
 * it). */
extern "C" void port_reentry_release_caches(void) {
    portGfxReleaseForReentry();
    portRelocTableReleaseForReentry();
    portParticleBankReleaseForReentry();
    portFixupReleaseForReentry();
    portRollbackReleaseForReentry();
    // The scene arena (sys/taskman.c) is allocated once and recycled across
    // scenes; a reset would only forget it.
    if (gPortSceneHeap != nullptr) {
        port_dl_range_unregister(gPortSceneHeap);
        std::free(gPortSceneHeap);
        gPortSceneHeap = nullptr;
    }
}

extern "C" void port_reentry_restore_pristine(void) {
    for (int i = 0; i < sSectionCount; i++) {
        std::memcpy(sSections[i].base, sSections[i].copy, sSections[i].size);
    }
    RestorePreMainBlocks();
}

extern "C" unsigned long port_reentry_saved_bytes(void) {
    return sSavedBytes;
}

extern "C" void port_reentry_stats(unsigned long *footprint_kb, unsigned int *threads) {
    *footprint_kb = 0;
    *threads = 0;
#if defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        *footprint_kb = static_cast<unsigned long>(info.phys_footprint / 1024);
    }
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) == KERN_SUCCESS) {
        *threads = n;
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            mach_port_deallocate(mach_task_self(), list[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), n * sizeof(thread_act_t));
    }
#endif
}

/* ---------------------------------------------------------------------
 * C++ heap blocks made before main().
 *
 * Global C++ objects allocate during static initialisation (1047 blocks,
 * ~55 KB in Smash). The pristine copy of the globals points at them; if a
 * run frees one and the memory gets reused, the next run's restored
 * globals would point at someone else's data. So: record every operator
 * new before main, copy those blocks' contents with the globals, never
 * free them afterwards, and copy their contents back on each reset.
 * (Mario Kart does the same in its own operator new; in i64 this belongs
 * in the app's operator new.)
 * ------------------------------------------------------------------- */
#include <algorithm>
#include <new>

namespace {
struct PreMainBlock {
    void *ptr;
    size_t size;
    uint8_t *copy;
};
constexpr int kMaxPreMain = 8192;
KEEP PreMainBlock sPreMain[kMaxPreMain] = {};
KEEP int sPreMainCount = 0;
KEEP bool sPreMainSealed = false;  // pristine copy taken: the set is final, sorted

bool IsPreMain(void *p) {
    if (!sPreMainSealed || p == nullptr) {
        return false;
    }
    int lo = 0, hi = sPreMainCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (sPreMain[mid].ptr == p) return true;
        if (sPreMain[mid].ptr < p) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

void *Allocate(size_t n) {
    void *p = std::malloc(n ? n : 1);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    if (!sPreMainSealed && sPreMainCount < kMaxPreMain) {
        sPreMain[sPreMainCount++] = {p, n, nullptr};
    }
    return p;
}

void Release(void *p) {
    if (p == nullptr) {
        return;
    }
    if (!sPreMainSealed) {
        for (int i = sPreMainCount - 1; i >= 0; i--) {  // freed before main: forget it
            if (sPreMain[i].ptr == p) {
                sPreMain[i] = sPreMain[--sPreMainCount];
                break;
            }
        }
    } else if (IsPreMain(p)) {
        return;  // kept for the next run's restored globals
    }
    std::free(p);
}
} // namespace

void *operator new(std::size_t n) { return Allocate(n); }
void *operator new[](std::size_t n) { return Allocate(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept {
    try { return Allocate(n); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept {
    try { return Allocate(n); } catch (...) { return nullptr; }
}
void operator delete(void *p) noexcept { Release(p); }
void operator delete[](void *p) noexcept { Release(p); }
void operator delete(void *p, std::size_t) noexcept { Release(p); }
void operator delete[](void *p, std::size_t) noexcept { Release(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { Release(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { Release(p); }

static void SealPreMainBlocks(void) {
    if (sPreMainSealed) return;
    std::sort(sPreMain, sPreMain + sPreMainCount,
              [](const PreMainBlock &a, const PreMainBlock &b) { return a.ptr < b.ptr; });
    size_t bytes = 0;
    for (int i = 0; i < sPreMainCount; i++) {
        sPreMain[i].copy = static_cast<uint8_t *>(std::malloc(sPreMain[i].size ? sPreMain[i].size : 1));
        std::memcpy(sPreMain[i].copy, sPreMain[i].ptr, sPreMain[i].size);
        bytes += sPreMain[i].size;
    }
    sPreMainSealed = true;
    std::fprintf(stderr, "SSB64 reentry: kept %d C++ blocks (%zu KB) made before main\n", sPreMainCount, bytes / 1024);
}

static void RestorePreMainBlocks(void) {
    for (int i = 0; i < sPreMainCount; i++) {
        std::memcpy(sPreMain[i].ptr, sPreMain[i].copy, sPreMain[i].size);
    }
}
