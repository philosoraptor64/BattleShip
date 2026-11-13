/* The scene arena (decomp sys/taskman.c, 16 MB, reused across scenes for the whole run). Inside i64 it comes from
 * i64's game memory arena (i64 docs/game-memory-arena.md), which i64 wipes after every run: a malloc'd 16 MB block
 * freed at stop stayed in the app's footprint (the system allocator keeps freed large blocks until exit). */
#include <cstdint>
#include <cstdlib>
#include <sys/mman.h>

extern "C" {
uint64_t (*gPortSceneHeapArena)(uint64_t size, uint64_t align) = nullptr; /* n64_host.arena_reserve (i64_module.cpp) */
static void *sFromArena = nullptr;

void *port_scene_heap_alloc(size_t size) {
    if (gPortSceneHeapArena != nullptr) {
        const uint64_t a = gPortSceneHeapArena(size, 0);
        if (a != 0 && mprotect((void *)(uintptr_t)a, size, PROT_READ | PROT_WRITE) == 0) {
            sFromArena = (void *)(uintptr_t)a;
            return sFromArena;
        }
    }
    return malloc(size);
}

void port_scene_heap_free(void *p, size_t size) {
    if (p == nullptr) {
        return;
    }
    if (p == sFromArena) {
        /* back to reserved, not unmapped: i64 wipes the arena after the run, and a hole could be filled meanwhile */
        mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        sFromArena = nullptr;
        return;
    }
    free(p);
}
}
