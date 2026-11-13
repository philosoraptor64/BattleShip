#ifndef _WIN32
#include <unistd.h>
#endif
#include <cstdint>
#include <vector>
#include "port_save.h"
#include "port_log.h"

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

// Where the SRAM file lives, in priority order:
//
//   1. $SSB64_SAVE_PATH                  — explicit override (debug / CI).
//   2. Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin")
//      — same convention as BattleShip.o2r and BattleShip.cfg.json:
//         * NON_PORTABLE=ON                                 → OS app-data dir
//             macOS    ~/Library/Application Support/BattleShip/
//             Linux    $XDG_DATA_HOME/BattleShip/  (or ~/.local/share/...)
//             Windows  %APPDATA%\BattleShip\
//         * NON_PORTABLE=OFF                                → cwd (portable)
//         * SHIP_HOME=<dir>                                 → that dir wins on macOS/Linux
//   3. SDL_GetPrefPath fallback                              — only if Ship::Context
//                                                             hasn't been constructed yet
//                                                             (shouldn't happen — SRAM
//                                                             I/O runs after PortInit —
//                                                             but defensive).
//   4. "ssb64_save.bin" in cwd                               — last-resort.

namespace {

std::once_flag gPathOnce;
std::string gSavePath;

void resolveSavePath()
{
    if (const char *override = std::getenv("SSB64_SAVE_PATH")) {
        gSavePath = override;
        return;
    }
    try {
        // Mirrors how BattleShip.o2r / BattleShip.cfg.json / logs/*.log
        // are located. Honors SHIP_HOME and the NON_PORTABLE build flag.
        gSavePath = Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin");
        if (!gSavePath.empty()) {
            return;
        }
    } catch (...) {
        // Ship::Context not yet alive — fall through.
    }
    if (char *p = SDL_GetPrefPath(NULL, "BattleShip")) {
        gSavePath = std::string(p) + "ssb64_save.bin";
        SDL_free(p);
        return;
    }
    gSavePath = "ssb64_save.bin"; // last-resort cwd
}

const std::string &savePath()
{
    std::call_once(gPathOnce, resolveSavePath);
    return gSavePath;
}

} // namespace

extern "C" const char *port_save_get_path(void)
{
    return savePath().c_str();
}

extern "C" int port_save_read(uintptr_t offset, void *dst, size_t size)
{
    if (size == 0) return 0;
    std::memset(dst, 0, size);

    FILE *f = std::fopen(savePath().c_str(), "rb");
    if (f == NULL) {
        return 0; // missing file -> zero-filled, treated as fresh SRAM
    }
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return 0; // short file -> zero-filled
    }
    (void)std::fread(dst, 1, size, f);
    std::fclose(f);
    return 0;
}

extern "C" int port_save_write(uintptr_t offset, const void *src, size_t size)
{
    if (size == 0) return 0;

    const std::string &path = savePath();

    // The whole save is read, patched in memory and written to a temporary file that then
    // replaces the save in one step (rename): stopped mid-write (a crash, iOS ending the app,
    // i64 stopping the game), the old save stays whole instead of half-updated.
    std::vector<uint8_t> image;
    if (FILE *in = std::fopen(path.c_str(), "rb")) {
        std::fseek(in, 0, SEEK_END);
        long n = std::ftell(in);
        std::fseek(in, 0, SEEK_SET);
        if (n > 0) {
            image.resize((size_t)n);
            image.resize(std::fread(image.data(), 1, (size_t)n, in));
        }
        std::fclose(in);
    }
    if (image.size() < offset + size) image.resize(offset + size, 0);   // zero-padded, as before
    std::memcpy(image.data() + offset, src, size);

    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == NULL) {
        port_log("SSB64 Save: open(%s) for write failed\n", tmp.c_str());
        return -1;
    }
    bool ok = std::fwrite(image.data(), 1, image.size(), f) == image.size() && std::fflush(f) == 0;
#ifndef _WIN32
    if (ok) ok = fsync(fileno(f)) == 0;
#endif
    if (std::fclose(f) != 0) ok = false;
#ifdef _WIN32
    if (ok) std::remove(path.c_str());   // rename doesn't replace an existing file on Windows
#endif
    if (ok) ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    if (!ok) {
        std::remove(tmp.c_str());
        port_log("SSB64 Save: write of 0x%zx bytes at offset 0x%lx failed\n", size, (unsigned long)offset);
        return -1;
    }
    return 0;
}
