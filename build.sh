#!/bin/bash
set -e

PROJECT_DIR="/Users/matthewbriggs/Downloads/macemu-master 4"
XCODE_DIR="$PROJECT_DIR/SheepShaver/src/MacOSX"
ENTITLEMENTS="$XCODE_DIR/SheepShaver/SheepShaver.entitlements"

echo "==> Cleaning DerivedData..."
rm -rf ~/Library/Developer/Xcode/DerivedData/SheepShaver-*

echo "==> Building Release..."
cd "$XCODE_DIR"
xcodebuild -project SheepShaver.xcodeproj -scheme SheepShaver -configuration Release build

APP=$(ls -d ~/Library/Developer/Xcode/DerivedData/SheepShaver-*/Build/Products/Release/SheepShaver.app)

# Xcode signs with the hardened runtime, which makes dyld reject the vendored
# SDL dylibs (library validation / Team ID mismatch) and the app dies at launch.
# Re-sign ad-hoc WITHOUT --options runtime: dylibs first, then the bundle, then
# the main binary last so the JIT entitlement survives on the binary itself.
echo "==> Re-signing (ad-hoc, no hardened runtime)..."
find "$APP/Contents/Frameworks" -type f -name "*.dylib" -exec codesign --force -s - {} \;
codesign --force -s - "$APP"
codesign --force -s - --entitlements "$ENTITLEMENTS" "$APP/Contents/MacOS/SheepShaver"
codesign --verify --deep --strict "$APP"
echo "==> Signature verified"

echo "==> Copying to Desktop..."
rm -rf ~/Desktop/SheepShaver.app
ditto "$APP" ~/Desktop/SheepShaver.app

echo "==> Done: ~/Desktop/SheepShaver.app ($( /usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' ~/Desktop/SheepShaver.app/Contents/Info.plist ))"
