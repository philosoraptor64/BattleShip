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
#include <mach/mach.h>
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
    uint8_t *base = getsectiondata(&_mh_execute_header, segment, section, &size);
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

extern "C" void port_reentry_save_pristine(void) {
    if (sSectionCount != 0) {
        return;  // only the first, pristine state
    }
    Add("__DATA", "__data");
    Add("__DATA", "__bss");
    Add("__DATA", "__common");
    Add("__SSB64SNAP", "__data");
    Add("__SSB64SNAP", "__bss");
}

extern "C" void portGfxReleaseForReentry(void);
extern "C" void portRelocTableReleaseForReentry(void);
extern "C" void portParticleBankReleaseForReentry(void);

/* After shutdown, before the globals are reset: free what the engine's own
 * shutdown leaves in global caches (resetting the globals would only orphan
 * it). */
extern "C" void port_reentry_release_caches(void) {
    portGfxReleaseForReentry();
    portRelocTableReleaseForReentry();
    portParticleBankReleaseForReentry();
}

extern "C" void port_reentry_restore_pristine(void) {
    for (int i = 0; i < sSectionCount; i++) {
        std::memcpy(sSections[i].base, sSections[i].copy, sSections[i].size);
    }
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
