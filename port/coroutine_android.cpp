/**
 * coroutine_android.cpp — ARM coroutine impl for Android (bionic).
 *
 * Bionic libc dropped getcontext/makecontext/swapcontext (they were
 * never supported on ARM), so the POSIX ucontext path in
 * coroutine_posix.cpp doesn't compile here. Instead, we ship a minimal
 * asm context-switch that saves the AAPCS callee-saved set — same approach
 * as boost::context's fcontext_t — and drive it from this file:
 *   arm64-v8a   -> port/coroutine_aarch64.S  (AAPCS64)
 *   armeabi-v7a -> port/coroutine_armv7.S    (AArch32 AAPCS)
 *
 * The CoroCtx struct in this file MUST stay in lockstep with the offsets
 * in the matching .S. The static_asserts below catch any drift at compile
 * time.
 */

#if defined(__ANDROID__) || defined(BATTLESHIP_IOS) || (defined(__APPLE__) && defined(__aarch64__))

/* iOS uses this backend too (getcontext() is a stub on devices), and so do
 * Apple Silicon Macs, so both share the pooled allocator below that rollback
 * snapshots rely on. */
#if !defined(__aarch64__) && !defined(__arm__)
#  error "Android coroutine backend supports arm64-v8a (coroutine_aarch64.S) "  \
         "and armeabi-v7a (coroutine_armv7.S) only. Add an x86/x86_64 swap "    \
         "if you need those ABIs."
#endif

#include "coroutine.h"
#include "port_watchdog.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#define MIN_STACK_SIZE 32768

extern "C" {

#if defined(__aarch64__)
struct alignas(16) CoroCtx {
    uint64_t x19, x20, x21, x22;   /* offsets   0..24 */
    uint64_t x23, x24, x25, x26;   /*          32..56 */
    uint64_t x27, x28, x29, x30;   /*          64..88 */
    uint64_t sp;                    /*             96 */
    uint64_t pad;                   /*            104 (keep d8 at 16-aligned 112) */
    double   d8,  d9,  d10, d11;   /*         112..136 */
    double   d12, d13, d14, d15;   /*         144..168 */
};
static_assert(sizeof(CoroCtx) == 176,           "CoroCtx size must match asm");
static_assert(__builtin_offsetof(CoroCtx, x19) == 0,      "x19 offset");
static_assert(__builtin_offsetof(CoroCtx, x29) == 80,     "x29 offset");
static_assert(__builtin_offsetof(CoroCtx, x30) == 88,     "x30 offset");
static_assert(__builtin_offsetof(CoroCtx, sp)  == 96,     "sp offset");
static_assert(__builtin_offsetof(CoroCtx, d8)  == 112,    "d8 offset");
static_assert(__builtin_offsetof(CoroCtx, d15) == 168,    "d15 offset");

#elif defined(__arm__)
/* AArch32 AAPCS callee-saved set. Mirrors coroutine_armv7.S. */
struct alignas(8) CoroCtx {
    uint32_t r4, r5, r6, r7;       /* offsets   0..12 */
    uint32_t r8, r9, r10, r11;     /*          16..28 */
    uint32_t sp;                    /*             32 */
    uint32_t lr;                    /*             36 */
    double   d8,  d9,  d10, d11;   /*          40..64 */
    double   d12, d13, d14, d15;   /*          72..96 */
};
static_assert(sizeof(CoroCtx) == 104,           "CoroCtx size must match asm");
static_assert(__builtin_offsetof(CoroCtx, r4)  == 0,      "r4 offset");
static_assert(__builtin_offsetof(CoroCtx, r11) == 28,     "r11 offset");
static_assert(__builtin_offsetof(CoroCtx, sp)  == 32,     "sp offset");
static_assert(__builtin_offsetof(CoroCtx, lr)  == 36,     "lr offset");
static_assert(__builtin_offsetof(CoroCtx, d8)  == 40,     "d8 offset");
static_assert(__builtin_offsetof(CoroCtx, d15) == 96,     "d15 offset");
#endif

struct PortCoroutine {
    CoroCtx ctx;            /* this coroutine's saved state */
    CoroCtx caller_ctx;     /* whoever last resumed us */
    void  (*entry)(void *); /* user entry */
    void   *arg;            /* arg passed to entry */
    int     finished;       /* 1 once entry returns */
    void   *stack_mem;      /* mmap base (guard page at the bottom) */
    size_t  stack_total;    /* full mapping length incl. guard page */
    int     in_use;         /* pool slot taken (Apple) */
};

#if defined(__APPLE__)
/* Rollback snapshots restore the game to an earlier frame, including
 * coroutines that have since been destroyed. So on Apple the coroutine
 * records live in the snapshot section and every stack comes from one region
 * that is never unmapped: a restored record always points at valid memory.
 * port_coroutine_snapshot_save/restore copy each live stack's used part. */
#define PORT_COROUTINE_POOL_SLOTS 128
#define PORT_COROUTINE_POOL_STACK (1024 * 1024)

__attribute__((section("__SSB64SNAP,__bss"))) static PortCoroutine sCoroutinePool[PORT_COROUTINE_POOL_SLOTS];
static char *sCoroutineStackRegion = nullptr;
static size_t sCoroutineStackStride = 0;
#endif

/* Implemented in port/coroutine_aarch64.S (arm64) / coroutine_armv7.S (arm32) */
void port_coroutine_swap(CoroCtx *from, CoroCtx *to);
#if defined(__aarch64__)
void port_coroutine_trampoline_aarch64(void);
#elif defined(__arm__)
void port_coroutine_trampoline_armv7(void);
#endif

static thread_local PortCoroutine *sCurrentCoroutine = nullptr;

/* Called from the asm trampoline with PortCoroutine* in x0.
 * Marked extern "C" so the symbol matches the asm reference exactly. */
__attribute__((noreturn))
void port_coroutine_trampoline_c(PortCoroutine *co) {
    co->entry(co->arg);
    co->finished = 1;
    sCurrentCoroutine = nullptr;
    /* Permanent yield. There is no caller frame on this coroutine's stack
     * to return to, so we swap back to whoever last resumed us. They will
     * see the finished flag and never resume us again. */
    port_coroutine_swap(&co->ctx, &co->caller_ctx);
    /* If we got back here, the caller resumed a finished coroutine. That's
     * caller error — port_coroutine_resume guards against it, but a hand-
     * rolled swap could trip this. */
    fprintf(stderr, "SSB64: coroutine resumed after finish\n");
    abort();
}

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */

void port_coroutine_init_main(void) {
    /* No-op. caller_ctx is populated by the first port_coroutine_swap;
     * before that, the main thread isn't tracked anywhere. */
}

PortCoroutine *port_coroutine_create(void (*entry)(void *), void *arg, size_t stack_size) {
    if (stack_size < MIN_STACK_SIZE) {
        stack_size = MIN_STACK_SIZE;
    }
    long ps_v = sysconf(_SC_PAGESIZE);
    size_t ps = (ps_v > 0) ? (size_t)ps_v : 4096;
    stack_size = (stack_size + ps - 1) & ~(ps - 1);

#if defined(__APPLE__)
    if (stack_size > PORT_COROUTINE_POOL_STACK) {
        fprintf(stderr, "SSB64: coroutine stack %zu exceeds pool stack size\n", stack_size);
        return nullptr;
    }
    if (sCoroutineStackRegion == nullptr) {
        sCoroutineStackStride = PORT_COROUTINE_POOL_STACK + ps;
        void *region = mmap(nullptr, sCoroutineStackStride * PORT_COROUTINE_POOL_SLOTS, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (region == MAP_FAILED) {
            return nullptr;
        }
        sCoroutineStackRegion = (char *)region;
        for (int i = 0; i < PORT_COROUTINE_POOL_SLOTS; i++) {
            mprotect(sCoroutineStackRegion + i * sCoroutineStackStride, ps, PROT_NONE);
        }
    }
    PortCoroutine *co = nullptr;
    for (int i = 0; i < PORT_COROUTINE_POOL_SLOTS; i++) {
        if (!sCoroutinePool[i].in_use) {
            co = &sCoroutinePool[i];
            memset(co, 0, sizeof(*co));
            co->in_use = 1;
            co->stack_mem = sCoroutineStackRegion + i * sCoroutineStackStride;
            co->stack_total = sCoroutineStackStride;
            break;
        }
    }
    if (co == nullptr) {
        fprintf(stderr, "SSB64: coroutine pool exhausted (%d slots)\n", PORT_COROUTINE_POOL_SLOTS);
        return nullptr;
    }
#else
    PortCoroutine *co = (PortCoroutine *)calloc(1, sizeof(PortCoroutine));
    if (!co) {
        return nullptr;
    }

    /* mmap the stack with a PROT_NONE guard page at the low end so a
     * coroutine stack overflow faults immediately instead of silently
     * corrupting adjacent heap. mmap returns page-aligned memory, which
     * satisfies AAPCS64's 16-byte sp alignment requirement. */
    co->stack_total = stack_size + ps;
    void *stk = mmap(nullptr, co->stack_total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stk == MAP_FAILED) {
        free(co);
        return nullptr;
    }
    mprotect(stk, ps, PROT_NONE);
    co->stack_mem  = stk;
#endif
    co->entry      = entry;
    co->arg        = arg;
    co->finished   = 0;

    /* Initial saved-context state. On first swap into this coroutine:
     *   sp       := top of stack (high address, 8/16-aligned)
     *   arg reg  := PortCoroutine* (read by the asm trampoline as its 1st arg)
     *   link reg := trampoline entry (restored as lr, entered by the swap's
     *               final `ret`/`bx lr`)
     * All other regs are zero-init via calloc. */
    char *stack_top = (char *)co->stack_mem + co->stack_total;
#if defined(__aarch64__)
    co->ctx.sp  = (uint64_t)stack_top;
    co->ctx.x19 = (uint64_t)co;
    co->ctx.x30 = (uint64_t)&port_coroutine_trampoline_aarch64;
#elif defined(__arm__)
    co->ctx.sp = (uint32_t)(uintptr_t)stack_top;
    co->ctx.r4 = (uint32_t)(uintptr_t)co;
    co->ctx.lr = (uint32_t)(uintptr_t)&port_coroutine_trampoline_armv7;
#endif

    return co;
}

void port_coroutine_destroy(PortCoroutine *co) {
    if (!co) return;
    if (co == sCurrentCoroutine) {
        fprintf(stderr, "SSB64: port_coroutine_destroy on current coroutine\n");
        abort();
    }
#if defined(__APPLE__)
    co->in_use = 0;
#else
    if (co->stack_mem) {
        munmap(co->stack_mem, co->stack_total);
    }
    free(co);
#endif
}

void port_coroutine_resume(PortCoroutine *co) {
    if (!co || co->finished) return;

    /* Save the previous current so nested resumes restore correctly.
     * Same semantics as the POSIX backend. Example: main resumes Thread5,
     * Thread5 resumes a GObj coroutine. When the GObj yields,
     * sCurrentCoroutine must be restored to Thread5 (not nullptr) so
     * Thread5 can yield later. */
    PortCoroutine *prev = sCurrentCoroutine;
    sCurrentCoroutine = co;

    port_coroutine_swap(&co->caller_ctx, &co->ctx);

    sCurrentCoroutine = prev;
}

void port_coroutine_yield(void) {
    PortCoroutine *co = sCurrentCoroutine;
    if (!co) {
        fprintf(stderr, "SSB64: port_coroutine_yield called outside coroutine\n");
        return;
    }
    port_watchdog_note_yield();
    sCurrentCoroutine = nullptr;
    port_coroutine_swap(&co->ctx, &co->caller_ctx);
    /* Returns here when resumed. */
}

int port_coroutine_is_finished(PortCoroutine *co) {
    return co ? co->finished : 1;
}

int port_coroutine_in_coroutine(void) {
    return sCurrentCoroutine != nullptr;
}

#if defined(__APPLE__)
/* Snapshot of every live coroutine's used stack, [saved sp, stack top). The
 * records themselves are restored with the snapshot section, so restore
 * runs after that. Only valid between frames, with no coroutine running. */
struct CoroStackRecord {
    uint32_t slot;
    uint32_t pad;
    uint64_t sp;
    uint64_t len;
};

size_t port_coroutine_snapshot_size(void) {
    size_t size = 0;
    for (int i = 0; i < PORT_COROUTINE_POOL_SLOTS; i++) {
        const PortCoroutine &co = sCoroutinePool[i];
        if (co.in_use) {
            size += sizeof(CoroStackRecord) + ((char *)co.stack_mem + co.stack_total - (char *)co.ctx.sp);
        }
    }
    return size;
}

size_t port_coroutine_snapshot_save(uint8_t *dst, size_t capacity) {
    size_t used = 0;
    if (sCurrentCoroutine != nullptr) {
        fprintf(stderr, "SSB64: coroutine snapshot taken inside a coroutine\n");
        abort();
    }
    for (int i = 0; i < PORT_COROUTINE_POOL_SLOTS; i++) {
        const PortCoroutine &co = sCoroutinePool[i];
        if (!co.in_use) {
            continue;
        }
        const char *top = (const char *)co.stack_mem + co.stack_total;
        CoroStackRecord rec = { (uint32_t)i, 0, co.ctx.sp, (uint64_t)(top - (const char *)co.ctx.sp) };
        if (used + sizeof(rec) + rec.len > capacity) {
            return 0;
        }
        memcpy(dst + used, &rec, sizeof(rec));
        memcpy(dst + used + sizeof(rec), (const void *)rec.sp, rec.len);
        used += sizeof(rec) + rec.len;
    }
    return used;
}

void port_coroutine_snapshot_restore(const uint8_t *src, size_t size) {
    size_t at = 0;
    while (at + sizeof(CoroStackRecord) <= size) {
        CoroStackRecord rec;
        memcpy(&rec, src + at, sizeof(rec));
        memcpy((void *)rec.sp, src + at + sizeof(rec), rec.len);
        at += sizeof(rec) + rec.len;
    }
}
#endif

} /* extern "C" */

#endif /* __ANDROID__ || BATTLESHIP_IOS */
