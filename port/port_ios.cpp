/* iOS startup glue.
 *
 * libultraship resolves both the app directory and the data directory to
 * ~/Documents on iOS, and an iOS app cannot be launched with environment
 * variables. Before anything else runs, this:
 *
 *  1. copies the files bundled in the app's GameData/ folder (BattleShip.o2r,
 *     f3d.o2r, gamecontrollerdb.txt, assets/) into Documents when they are
 *     missing or a different size (i.e. after an app update), and
 *  2. if BattleShip.o2r is still missing (an app built without ROM-derived
 *     data, SSB64_IOS_BUNDLE_ROM_ASSETS=OFF), extracts it on the phone from
 *     the player's own ROM, which they copy into the app's Files folder,
 *  3. reads Documents/netplay.cfg (KEY=VALUE lines, '#' comments) and sets
 *     each entry as an environment variable, so the SSB64_NETPLAY_* debug
 *     netplay settings work on iPhone. The file is editable from the Files
 *     app (UIFileSharingEnabled + LSSupportsOpeningDocumentsInPlace).
 */
#include "port_ios.h"
#include "port_ios_menu.h"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IOS

#include <SDL2/SDL.h>
#include <CoreFoundation/CoreFoundation.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

/* android_torch_bridge.cpp (shared with Android). Returns 0 on success, 3 if
 * the ROM is not a supported dump. */
extern "C" int torch_extract_o2r(const char* rom_path, const char* src_dir, const char* dst_dir);
extern "C" void port_log(const char* fmt, ...);
/* port_ios_status.mm: input-free status screen for first-launch setup. */
extern "C" void port_ios_status_show(const char* title, const char* text);
extern "C" void port_ios_status_hide(void);

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

constexpr const char* kO2rName = "BattleShip.o2r";

void ShowMessage(const char* title, const char* text) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text, nullptr);
}

// ROM files the player put in Documents (Files -> On My iPhone -> BattleShip).
std::vector<fs::path> FindPlayerRoms(const fs::path& docs) {
    std::vector<fs::path> roms;
    std::error_code ec;
    for (auto it = fs::directory_iterator(docs, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        uintmax_t size = it->file_size(ec);
        if ((ext == ".z64" || ext == ".n64" || ext == ".v64") && size >= (8u << 20) && size <= (64u << 20)) {
            roms.push_back(it->path());
        }
    }
    std::sort(roms.begin(), roms.end());
    return roms;
}

// Runs Torch on a worker thread while the main thread keeps the run loop
// turning, so iOS does not kill the app for a hung launch.
int ExtractOnWorker(const fs::path& rom, const fs::path& recipes, const fs::path& out) {
    std::atomic<bool> done{ false };
    int rc = -1;
    std::thread worker([&] {
        rc = torch_extract_o2r(rom.c_str(), recipes.c_str(), out.c_str());
        done = true;
    });
    while (!done) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }
    worker.join();
    return rc;
}

// Returns true when BattleShip.o2r is ready in Documents.
bool EnsureGameData(const fs::path& docs) {
    std::error_code ec;
    if (fs::exists(docs / kO2rName, ec)) {
        return true;
    }
    std::vector<fs::path> roms = FindPlayerRoms(docs);
    if (roms.empty()) {
        port_log("ios: no %s and no ROM in Documents\n", kO2rName);
        ShowMessage("Add your ROM",
                    "BattleShip plays from your own copy of Super Smash Bros. (USA).\n\n"
                    "1. Copy your ROM file (.z64, .n64 or .v64) to Files > On My iPhone > BattleShip.\n"
                    "2. Open BattleShip again.");
        return false;
    }
    char* base = SDL_GetBasePath();
    fs::path recipes = fs::path(base ? base : "") / "TorchRecipe";
    SDL_free(base);

    port_ios_status_show("Setting up BattleShip",
                         "Building the game data from your ROM.\nThis takes about a minute and only happens once.");
    for (const fs::path& rom : roms) {
        port_log("ios: extracting game data from %s\n", rom.c_str());
        int rc = ExtractOnWorker(rom, recipes, docs);
        port_log("ios: extraction returned %d\n", rc);
        if (rc == 0 && fs::exists(docs / kO2rName, ec)) {
            port_ios_status_hide();
            return true;
        }
        fs::remove(docs / kO2rName, ec);  // never keep a partial archive
    }
    port_ios_status_hide();
    ShowMessage("ROM not supported",
                "None of the ROM files in the BattleShip folder is Super Smash Bros. (USA) v1.0, which "
                "this app needs. Check the file and open BattleShip again.");
    return false;
}

// KEY=VALUE lines ('#' comments) of a file in Documents.
std::vector<std::pair<std::string, std::string>> ReadKeyValues(const fs::path& path) {
    std::vector<std::pair<std::string, std::string>> out;
    std::ifstream in(path);
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
            out.emplace_back(key, value);
        }
    }
    return out;
}

/* Online play settings, kept across launches: Documents/online.cfg holds
 * SERVER, PASSWORD, ROOM and LAST_HOST (written by invite links and the start
 * menu); Documents/online-pending.cfg (MODE=join|host) is a one-shot left by
 * an invite link, so a launch from a link skips the menu. */
void LoadOnlineState(const fs::path& docs, PortIosMenuState* st) {
    *st = PortIosMenuState{};
    for (auto& [k, v] : ReadKeyValues(docs / "online.cfg")) {
        if (k == "SERVER") std::snprintf(st->server, sizeof(st->server), "%s", v.c_str());
        else if (k == "PASSWORD") std::snprintf(st->password, sizeof(st->password), "%s", v.c_str());
        else if (k == "ROOM") std::snprintf(st->room, sizeof(st->room), "%s", v.c_str());
        else if (k == "LAST_HOST") st->last_was_host = (v == "1");
    }
}

void SaveOnlineState(const fs::path& docs, const PortIosMenuState& st) {
    std::ofstream out(docs / "online.cfg", std::ios::trunc);
    out << "# Online play settings (written by invite links and the start menu).\n"
        << "SERVER=" << st.server << "\n"
        << "PASSWORD=" << st.password << "\n"
        << "ROOM=" << st.room << "\n"
        << "LAST_HOST=" << (st.last_was_host ? 1 : 0) << "\n";
}

// Turns an online choice into the SSB64_NETPLAY_* settings for this launch.
void ApplyOnlineChoice(const PortIosMenuState& st, bool host) {
    setenv("SSB64_NETPLAY", "1", 1);
    setenv("SSB64_NETPLAY_BOOTSTRAP", "1", 1);
    // A full online session: once both are connected, the whole game runs on
    // both phones from the same inputs (menus, character select with CPUs
    // and teams, stage select, battles, results), as if on one console.
    setenv("SSB64_NETPLAY_MODE", "session", 1);
    setenv("SSB64_NETPLAY_SERVER", st.server, 1);
    setenv("SSB64_NETPLAY_ROOM", st.room, 1);
    if (st.password[0] != '\0') {
        setenv("SSB64_NETPLAY_PASSWORD", st.password, 1);
    }
    setenv("SSB64_NETPLAY_ROLLBACK", "1", 1);
    setenv("SSB64_NETPLAY_DELAY", "2", 1);
    if (host) {
        setenv("SSB64_NETPLAY_HOST", "1", 1);
    }
    port_log("ios: online %s, room %s\n", host ? "host" : "join", st.room);
}

// Start menu, unless an invite link already decided for this launch.
void RunStartMenu(const fs::path& docs) {
    PortIosMenuState st;
    LoadOnlineState(docs, &st);
    std::error_code ec;
    const fs::path pending = docs / "online-pending.cfg";
    std::string mode;
    for (auto& [k, v] : ReadKeyValues(pending)) {
        if (k == "MODE") mode = v;
    }
    fs::remove(pending, ec);

    int choice;
    if (mode == "join" || mode == "host") {
        choice = (mode == "host") ? PORT_IOS_MENU_HOST : PORT_IOS_MENU_JOIN;
    } else {
        choice = port_ios_start_menu(&st);
    }
    if (choice == PORT_IOS_MENU_LOCAL) {
        if (st.settings_changed) {
            SaveOnlineState(docs, st);
        }
        return;
    }
    st.last_was_host = (choice == PORT_IOS_MENU_HOST);
    SaveOnlineState(docs, st);
    ApplyOnlineChoice(st, choice == PORT_IOS_MENU_HOST);
}

// Scripted settings (tests, advanced use). Returns true if it set anything,
// in which case the start menu is skipped.
bool LoadNetplayConfig(const fs::path& docs) {
    fs::path cfg = docs / "netplay.cfg";
    std::error_code ec;

    if (!fs::exists(cfg, ec)) {
        std::ofstream(cfg) << kNetplayTemplate;
        return false;
    }
    // Older invite links wrote their settings here, which would skip the
    // start menu on every launch: move them to online.cfg instead.
    {
        std::ifstream in(cfg);
        std::string first;
        std::getline(in, first);
        if (first.rfind("# Written by an invite link", 0) == 0) {
            PortIosMenuState st;
            LoadOnlineState(docs, &st);
            bool host = false;
            for (auto& [k, v] : ReadKeyValues(cfg)) {
                if (k == "SSB64_NETPLAY_SERVER") std::snprintf(st.server, sizeof(st.server), "%s", v.c_str());
                else if (k == "SSB64_NETPLAY_PASSWORD") std::snprintf(st.password, sizeof(st.password), "%s", v.c_str());
                else if (k == "SSB64_NETPLAY_ROOM") std::snprintf(st.room, sizeof(st.room), "%s", v.c_str());
                else if (k == "SSB64_NETPLAY_HOST") host = (v == "1");
            }
            st.last_was_host = host ? 1 : 0;
            SaveOnlineState(docs, st);
            in.close();
            std::ofstream(cfg, std::ios::trunc) << kNetplayTemplate;
            port_log("ios: moved old invite settings to online.cfg\n");
            return false;
        }
    }
    bool any = false;
    for (auto& [key, value] : ReadKeyValues(cfg)) {
        setenv(key.c_str(), value.c_str(), 1);
        any = true;
    }
    return any;
}

std::string PercentDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
            isxdigit((unsigned char)in[i + 2])) {
            out += (char)std::stoi(in.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (in[i] == '+') {
            out += ' ';
        } else {
            out += in[i];
        }
    }
    return out;
}

bool AllCharsIn(const std::string& v, const char* allowed) {
    for (char c : v) {
        if (!isalnum((unsigned char)c) && std::strchr(allowed, c) == nullptr) {
            return false;
        }
    }
    return true;
}

} // namespace

/* Invite link: battleship-net://join?s=<host:port>&r=<room>&p=<password>[&h=1]
 * (h=1: this phone hosts the room; the host sets up each match in the VS
 * menus and the room opens when the battle would start).
 * Writes Documents/netplay.cfg so the next start joins that room. Called
 * from port/port_ios_url.mm, before SDL_main when the link launched the app.
 * Returns a message for the player when the app was already running (the
 * settings apply on the next start), else NULL. */
extern "C" const char* port_ios_handle_open_url(const char* url, int app_running) {
    static std::string message;
    const std::string prefix = "battleship-net://join?";
    std::string u = url ? url : "";
    if (u.compare(0, prefix.size(), prefix) != 0) {
        message = "This link is not a BattleShip invite.";
        return app_running ? message.c_str() : nullptr;
    }
    std::string server, room, password;
    bool host = false;
    std::string query = u.substr(prefix.size());
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string kv = query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string k = kv.substr(0, eq), v = PercentDecode(kv.substr(eq + 1));
            if (k == "s") server = v;
            else if (k == "r") room = v;
            else if (k == "p") password = v;
            else if (k == "h") host = (v == "1");
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    for (char& c : room) c = (char)toupper((unsigned char)c);
    bool ok = server.size() >= 3 && server.size() <= 253 && server.find(':') != std::string::npos &&
              AllCharsIn(server, ".-:") && room.size() == 4 && AllCharsIn(room, "") && password.size() <= 64 &&
              AllCharsIn(password, "-_.~!@#$%^*()+=,") ;
    if (!ok) {
        port_log("ios: rejected invite link\n");
        message = "This invite link is damaged. Ask for a new one.";
        return app_running ? message.c_str() : nullptr;
    }
    const char* home = getenv("HOME");
    if (home == nullptr) {
        return nullptr;
    }
    fs::path docs = fs::path(home) / "Documents";
    std::error_code ec;
    fs::create_directories(docs, ec);
    PortIosMenuState st;
    LoadOnlineState(docs, &st);
    std::snprintf(st.server, sizeof(st.server), "%s", server.c_str());
    std::snprintf(st.password, sizeof(st.password), "%s", password.c_str());
    std::snprintf(st.room, sizeof(st.room), "%s", room.c_str());
    st.last_was_host = host ? 1 : 0;
    SaveOnlineState(docs, st);
    std::ofstream(docs / "online-pending.cfg", std::ios::trunc) << "MODE=" << (host ? "host" : "join") << "\n";
    port_log("ios: invite saved for room %s via %s (%s)\n", room.c_str(), server.c_str(), host ? "host" : "join");
    message = host ? "This iPhone will host room " + room +
                         ". Close BattleShip (swipe it away) and open it again; when your friend joins, "
                         "both games open the VS menu together."
                   : "Invite saved for room " + room +
                         ". Close BattleShip (swipe it away in the app switcher) and open it again to join.";
    return app_running ? message.c_str() : nullptr;
}

extern "C" void port_ios_prepare(void) {
    const char* home = getenv("HOME");
    if (home == nullptr) {
        return;
    }
    fs::path docs = fs::path(home) / "Documents";
    std::error_code ec;
    fs::create_directories(docs, ec);

    // SDL exposes the iPhone's accelerometer as a joystick by default, and
    // libultraship merges it into player 1's stick: tilting the phone reads
    // as holding the stick, which swallowed tap jumps.
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");

    CopyBundledGameData(docs);
    if (!EnsureGameData(docs)) {
        exit(0);
    }
    if (!LoadNetplayConfig(docs)) {
        RunStartMenu(docs);
    }

    // Relative loads (BattleShip.o2r, f3d.o2r, assets/...) resolve here.
    chdir(docs.c_str());
}

#else

extern "C" void port_ios_prepare(void) {
}

#endif
