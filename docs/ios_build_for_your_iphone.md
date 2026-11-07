# Build BattleShip for your own iPhone

This builds the game on your Mac from **your own** copy of Super Smash Bros. (USA) and installs it on your iPhone. The repository contains no game data; everything game-related is extracted from your ROM.

## You need

- A Mac with **Xcode** (free from the App Store). Open it once after installing.
- Your **Apple ID** added to Xcode: Xcode > Settings > Accounts > **+** > Apple ID. A free account works.
- **Homebrew**: https://brew.sh
- Your ROM of **Super Smash Bros. (USA) v1.0** (`.z64`, SHA-1 `e2929e10fccc0aa84e5776227e798abc07cedabf`), dumped from a cartridge you own.
- Your iPhone, a USB cable, and a Bluetooth controller (Xbox, PlayStation or Switch Pro) for playing.

## On the iPhone (once)

1. Plug it into the Mac, unlock it, and tap **Trust**.
2. Turn on **Developer Mode**: Settings > Privacy & Security > Developer Mode (at the bottom). The phone restarts; confirm **Turn On**.
   If Developer Mode is not listed yet, run the script below once; it appears after Xcode has seen the phone.

## Build and install

```bash
git clone --recurse-submodules https://github.com/philosoraptor64/BattleShip.git
cd BattleShip
cp "/path/to/Super Smash Bros. (USA).z64" baserom.us.z64
ios/build-and-install.sh
```

The first run takes about an hour (tools, a Mac build, and the iPhone project setup). Later runs take a few minutes.

## After installing

1. On the iPhone: Settings > General > **VPN & Device Management** > your Apple ID > **Trust** (first time only).
2. Open **BattleShip**. When iOS asks to **find devices on local networks**, tap **Allow** (netplay needs it).
3. To play online, tap the **invite link** a friend sent you (`battleship-net://join?...`). The game saves the room and joins it on the next start; if it was already open, swipe it away and open it again.

## Good to know

- With a free Apple ID the app **stops opening after 7 days**. Run `ios/build-and-install.sh` again to renew it (your settings and saves are kept).
- Free Apple IDs can have at most 3 sideloaded apps on a phone.
- Update to the latest version with `git pull --recurse-submodules` before running the script.
- To play offline again, delete `netplay.cfg` in Files > On My iPhone > BattleShip.
- `CLEAN=1 ios/build-and-install.sh` builds an app without any game data; it then asks for the ROM on the phone (copy it to Files > On My iPhone > BattleShip). That variant is the one that may be given to other people.
