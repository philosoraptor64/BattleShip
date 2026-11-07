#!/bin/bash
# Build BattleShip for your own iPhone and install it, using your own ROM.
#
# Needs: a Mac with Xcode (signed in to your Apple ID under Xcode > Settings >
# Accounts), Homebrew, your Super Smash Bros. (USA) v1.0 ROM saved as
# baserom.us.z64 in the repo root, and your iPhone plugged in, unlocked,
# trusted and in Developer Mode. See docs/ios_build_for_your_iphone.md.
#
# Usage: ios/build-and-install.sh
# Options (environment): BUNDLE_ID=com.example.battleship  TEAM=<10-char team ID>
#                        DEVICE=<UDID>  INSTALL=0 (build only)  CLEAN=1 (app without game data; it then
#                        asks for the ROM on the phone instead)
# First run takes about an hour (tools, two builds); later runs are minutes.
# Free Apple IDs: the install expires after 7 days; run this again to renew.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mStopped: %s\033[0m\n' "$*" >&2; exit 1; }

step "Checking the Mac"
xcode-select -p 2>/dev/null | grep -q "Xcode.app" ||
	die "Install Xcode from the App Store, open it once, then run: sudo xcode-select -s /Applications/Xcode.app"
xcodebuild -version >/dev/null 2>&1 ||
	die "Xcode is not ready. Run: sudo xcodebuild -license accept && sudo xcodebuild -runFirstLaunch"
xcodebuild -version | head -1
xcodebuild -showsdks 2>/dev/null | grep -q -- "-sdk iphoneos" ||
	die "Xcode has no iOS platform yet. Run: xcodebuild -downloadPlatform iOS  (or Xcode > Settings > Components)"
command -v brew >/dev/null || die "Install Homebrew first: https://brew.sh"
free_gb=$(df -g . | awk 'NR==2 {print $4}')
[ "${free_gb:-0}" -ge 10 ] || die "Only ${free_gb} GB free on this disk; the builds need about 10 GB."

# The ROM: baserom.us.z64, or a .n64/.v64 dump that gets converted to .z64.
want=e2929e10fccc0aa84e5776227e798abc07cedabf
if [ ! -f baserom.us.z64 ]; then
	for f in baserom.us.n64 baserom.us.v64; do [ -f "$f" ] && cp "$f" baserom.us.z64 && break; done
fi
[ -f baserom.us.z64 ] || die "Put your ROM in $ROOT as baserom.us.z64"
if [ "$(shasum baserom.us.z64 | cut -c1-40)" != "$want" ]; then
	# Big-endian .z64 starts 80 37 12 40; .v64 swaps byte pairs, .n64 reverses each 4 bytes.
	python3 - baserom.us.z64 <<'PY'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
head = bytes(data[:4])
if head == b"\x37\x80\x40\x12":      # .v64
    data[0::2], data[1::2] = data[1::2], data[0::2]
elif head == b"\x40\x12\x37\x80":    # .n64
    for i in range(0, len(data) - 3, 4):
        data[i:i + 4] = data[i:i + 4][::-1]
else:
    sys.exit(0)
open(path, "wb").write(data)
print("Converted the ROM to .z64 byte order.")
PY
fi
sha=$(shasum baserom.us.z64 | cut -c1-40)
[ "$sha" = "$want" ] ||
	die "baserom.us.z64 is not Super Smash Bros. (USA) v1.0 (SHA-1 $sha). Other versions (Japan, Europe, Rev A) are not supported."
git submodule update --init --recursive

step "Installing build tools (Homebrew)"
brew install cmake ninja sdl2 glew libzip nlohmann-json tinyxml2 spdlog
# Pillow (image tools used by the build). Homebrew's Python refuses plain
# --user installs (PEP 668), so fall back to allowing it for this one package.
python3 -c "import PIL" 2>/dev/null ||
	python3 -m pip install --user --quiet Pillow 2>/dev/null ||
	python3 -m pip install --user --quiet --break-system-packages Pillow

step "Finding your Apple developer team"
# The key holding the teams differs between Xcode versions, so search them all.
TEAM="${TEAM:-$(defaults read com.apple.dt.Xcode 2>/dev/null |
	grep -oE 'teamID = "?[A-Z0-9]{10}' | head -1 | grep -oE '[A-Z0-9]{10}$' || true)}"
[ -n "$TEAM" ] || die "No team found. In Xcode: Settings > Accounts > + > Apple ID, then run this again."
echo "Team: $TEAM"

step "Finding your iPhone"
if [ -z "${DEVICE:-}" ]; then
	json=$(mktemp)
	xcrun devicectl list devices --json-output "$json" >/dev/null 2>&1 || true
	DEVICE=$(python3 - "$json" <<'EOF'
import json, sys
try:
    devices = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    devices = []
for d in devices:
    hw = d.get("hardwareProperties", {})
    if hw.get("reality") == "physical" and hw.get("platform") == "iOS" and hw.get("udid"):
        print(hw["udid"])
        break
EOF
)
	rm -f "$json"
fi
[ -n "$DEVICE" ] || die "No iPhone found. Plug it in, unlock it, tap Trust, and turn on Developer Mode (Settings > Privacy & Security)."
echo "iPhone: $DEVICE"

# App IDs must be unique at Apple; pick a random one once and keep it, so
# rebuilds update the same app (and keep its saves).
if [ -z "${BUNDLE_ID:-}" ]; then
	[ -s .ios-bundle-id ] || echo "io.github.battleship.p$(LC_ALL=C tr -dc 'a-z0-9' </dev/urandom | head -c 8)" >.ios-bundle-id
	BUNDLE_ID="$(head -1 .ios-bundle-id)"
fi
echo "Bundle ID: $BUNDLE_ID"

step "Building the Mac version first (provides the asset extractor and game data)"
cmake -S . -B build-us-release -GNinja -DCMAKE_BUILD_TYPE=Release -DSSB64_VERSION=us
cmake --build build-us-release

step "Configuring the iPhone project (the first time takes ~20 minutes)"
bundle_assets=ON
[ "${CLEAN:-0}" = 1 ] && bundle_assets=OFF
cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DDEPLOYMENT_TARGET=16.0 -DDISABLE_SCRIPTING=ON \
	-DBZIP2_LIBRARY_RELEASE:STRING=-lbz2 -DZLIB_LIBRARY_RELEASE:STRING=-lz \
	-DSSB64_IOS_BUNDLE_ID="$BUNDLE_ID" -DSSB64_IOS_BUNDLE_ROM_ASSETS="$bundle_assets"

step "Building the iPhone app"
build() {
	xcodebuild -project build-ios/ssb64.xcodeproj -scheme ssb64 -configuration Release \
		-destination "id=$DEVICE" DEVELOPMENT_TEAM="$TEAM" CODE_SIGN_STYLE=Automatic \
		-allowProvisioningUpdates -allowProvisioningDeviceRegistration build >build-ios/xcodebuild.log 2>&1
}
# After source files are added, Xcode regenerates the project on the first
# build and only links the new files on the second.
build || build || { tail -30 build-ios/xcodebuild.log; die "Build failed; full log in build-ios/xcodebuild.log"; }

if [ "${INSTALL:-1}" = 0 ]; then
	echo "Built build-ios/Release-iphoneos/BattleShip.app (INSTALL=0: not installed)."
	exit 0
fi
step "Installing on your iPhone"
xcrun devicectl device install app --device "$DEVICE" build-ios/Release-iphoneos/BattleShip.app

cat <<'EOF'

Done! On your iPhone:
  1. Settings > General > VPN & Device Management > your Apple ID > Trust (first time only).
  2. Open BattleShip. When asked to find devices on local networks, tap Allow.
  3. Tap the invite link your friend sent you to join their room.
Free Apple IDs: the app stops opening after 7 days; run this script again.
EOF
