/* iOS start menu (port_ios_menu.mm), used by port_ios.cpp. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PORT_IOS_MENU_LOCAL = 0,  // play offline as normal
    PORT_IOS_MENU_JOIN = 1,   // join room `room`
    PORT_IOS_MENU_HOST = 2,   // host room `room` (set up the match in the VS menus)
};

typedef struct PortIosMenuState {
    char server[256];    // host:port of the room-code server ("" = not set)
    char password[96];
    char room[8];        // last room code
    int last_was_host;   // the last online session hosted `room`
    int voice;           // voice chat: 0 off, 1 open mic, 2 push to talk
    int settings_changed;
} PortIosMenuState;

/* Shows the menu and returns PORT_IOS_MENU_* once the player has chosen.
 * Updates `state` (room, and server/password if edited). */
int port_ios_start_menu(PortIosMenuState* state);

#ifdef __cplusplus
}
#endif
