# Build BattleShip for your own iPhone

This builds the game on your Mac from **your own** copy of Super Smash Bros. (USA) and installs it on your iPhone. The repository contains no game data; everything game-related is extracted from your ROM.

## You need

- A Mac with **Xcode** (free from the App Store). Open it once after installing.
- Your **Apple ID** added to Xcode: Xcode > Settings > Accounts > **+** > Apple ID. A free account works.
- **Homebrew**: https://brew.sh
- Your ROM of **Super Smash Bros. (USA) v1.0** (SHA-1 of the `.z64`: `e2929e10fccc0aa84e5776227e798abc07cedabf`), dumped from a cartridge you own. `.n64`/`.v64` dumps are converted automatically.
- About **10 GB** of free disk space.
- Your iPhone, a USB cable, and a Bluetooth controller (Xbox, PlayStation or Switch Pro) for playing.

## On the iPhone (once)

1. Plug it into the Mac, unlock it, and tap **Trust**.
2. Turn on **Developer Mode**: Settings > Privacy & Security > Developer Mode (at the bottom). The phone restarts; confirm **Turn On**.
   If Developer Mode is not listed yet, run the script below once; it appears after Xcode has seen the phone.

## Build and install

```bash
git clone --recurse-submodules https://github.com/philosoraptor64/BattleShip.git
cd BattleShip
cp "/path/to/Super Smash Bros. (USA).z64" baserom.us.z64   # a .n64/.v64 file works too
ios/build-and-install.sh
```

The first run takes about an hour (tools, a Mac build, and the iPhone project setup). Later runs take a few minutes.

## After installing

1. On the iPhone: Settings > General > **VPN & Device Management** > your Apple ID > **Trust** (first time only).
2. Open **BattleShip**. When iOS asks to **find devices on local networks**, tap **Allow** (netplay needs it).
3. The game starts with a menu: **Play locally** (the normal game) or **Play online**.
4. To join a friend: tap their **invite link** once (it saves the server and password, and joins right away; if the game was already open, swipe it away and open it again). Later you can also choose **Play online > Join a match** and type the 4-character room code. The game shows "Joining an online match" and waits while the host sets up each match (fighters, CPUs, teams, stage). After the results it waits for the host's next match.

## Hosting a match

1. Choose **Play online > Host a match**. Your room code is shown: tap **Share invite link** to send it to your friend (Messages works), then **Set up the match**. The game starts in the VS menu. (The first time, the server and password come from a host invite link, or enter them under **Online settings**.)
2. Set up the match as usual. **Player 2 is your online opponent:** set it to **CPU** (any level) and pick their fighter; it becomes human when they join. Add CPUs as players 3 and 4, turn on teams, choose rules and items as you like.
3. Pick the stage. Instead of starting, the room opens ("Room ABCD is open"). The match starts as soon as your friend joins. If nobody joins within 5 minutes, you play it offline against the CPU.
4. After each match you're back at character select for the next one.

To play offline, choose **Play locally** at the start.

## Good to know

- With a free Apple ID the app **stops opening after 7 days**. Run `ios/build-and-install.sh` again to renew it (your settings and saves are kept).
- Free Apple IDs can have at most 3 sideloaded apps on a phone.
- Update to the latest version with `git pull --recurse-submodules` before running the script.
- To play offline, choose **Play locally** at the start.
- `CLEAN=1 ios/build-and-install.sh` builds an app without any game data; it then asks for the ROM on the phone (copy it to Files > On My iPhone > BattleShip). That variant is the one that may be given to other people.

## If something goes wrong

The script stops with a message saying what to do. Common cases:

| Message or symptom | Fix |
|---|---|
| "Xcode is not ready" | `sudo xcodebuild -license accept && sudo xcodebuild -runFirstLaunch` |
| "Xcode has no iOS platform yet" | `xcodebuild -downloadPlatform iOS` (or Xcode > Settings > Components) |
| "No team found" | Xcode > Settings > Accounts > + > Apple ID, then run the script again |
| "No iPhone found" | Plug it in, unlock it, tap Trust; turn on Developer Mode; try another cable |
| Build fails mentioning the device or iOS version | Your iPhone's iOS is newer than your Xcode: update Xcode |
| "Maximum number of apps" when installing | Free Apple IDs allow 3 sideloaded apps per phone: delete one |
| The app won't open ("Untrusted Developer") | Settings > General > VPN & Device Management > Trust |
| Can't connect to a friend's room | Tap Allow for local networks (or Settings > Privacy & Security > Local Network > BattleShip); check the invite link; make sure your friend's server and game are running |
| "Desync" or the match freezes online | Both players need the same version: both `git pull --recurse-submodules` and rebuild |

If you're stuck, send the last 30 lines the script printed (the full iPhone build log is `build-ios/xcodebuild.log`).

**If something goes wrong while playing**, send the game's log: Files > On My iPhone > BattleShip > `ssb64.log` (share it). If you've already reopened the game since, send `ssb64-previous.log` instead. After a fix, `git pull --recurse-submodules` and run the script again; it continues where it stopped.
