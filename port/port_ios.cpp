/* iOS startup glue.
 *
 * libultraship resolves both the app directory and the data directory to
 * ~/Documents on iOS, and an iOS app cannot be launched with environment
 * variables. Before anything else runs, this:
 *
 *  1. copies the files bundled in the app's GameData/ folder (BattleShip.o2r,
 *     f3d.o2r, gamecontrollerdb.txt, assets/) into Documents when they are
 *     missing or a different size (i.e. after an app update), and
 *  2. reads Documents/netplay.cfg (KEY=VALUE lines, '#' comments) and sets
 *     each entry as an environment variable, so the SSB64_NETPLAY_* debug
 *     netplay settings work on iPhone. The file is editable from the Files
 *     app (UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace).
 */
#include "port_ios.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

const char* kNetplayTemplate =
    "# BattleShip netplay settings (iPhone). Remove the leading '#' to enable a line.\n"
    "# Host the match on this phone (player 1):\n"
    "#SSB64_NETPLAY=1\n"
    "#SSB64_NETPLAY_BOOTSTRAP=1\n"
    "#SSB64_NETPLAY_HOST=1\n"
    "#SSB64_NETPLAY_LOCAL_PLAYER=0\n"
    "#SSB64_NETPLAY_REMOTE_PLAYER=1\n"
    "#SSB64_NETPLAY_BIND=0.0.0.0:7001\n"
    "#SSB64_NETPLAY_PEER=192.168.1.20:7002\n"
    "#SSB64_NETPLAY_DELAY=3\n";

void CopyBundledGameData(const fs::path& docs) {
    char* base = SDL_GetBasePath();
    if (base == nullptr) {
        return;
    }
    fs::path src = fs::path(base) / "GameData";
    SDL_free(base);

    std::error_code ec;
    if (!fs::is_directory(src, ec)) {
        return;
    }
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        fs::path rel = fs::relative(it->path(), src, ec);
        fs::path dst = docs / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(dst, ec);
            continue;
        }
        if (fs::exists(dst, ec) && fs::file_size(dst, ec) == it->file_size(ec)) {
            continue;
        }
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
    }
}

void LoadNetplayConfig(const fs::path& docs) {
    fs::path cfg = docs / "netplay.cfg";
    std::error_code ec;

    if (!fs::exists(cfg, ec)) {
        std::ofstream(cfg) << kNetplayTemplate;
        return;
    }
    std::ifstream in(cfg);
    std::string line;
    while (std::getline(in, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#') {
            continue;
        }
        size_t eq = line.find('=', start);
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(start, eq - start);
        std::string value = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
            value.pop_back();
        }
        size_t vstart = value.find_first_not_of(" \t");
        value = (vstart == std::string::npos) ? "" : value.substr(vstart);
        if (!key.empty()) {
            setenv(key.c_str(), value.c_str(), 1);
        }
    }
}

} // namespace

extern "C" void port_ios_prepare(void) {
    const char* home = getenv("HOME");
    if (home == nullptr) {
        return;
    }
    fs::path docs = fs::path(home) / "Documents";
    std::error_code ec;
    fs::create_directories(docs, ec);

    CopyBundledGameData(docs);
    LoadNetplayConfig(docs);

    // Relative loads (BattleShip.o2r, f3d.o2r, assets/...) resolve here.
    chdir(docs.c_str());
}

#else

extern "C" void port_ios_prepare(void) {
}

#endif
