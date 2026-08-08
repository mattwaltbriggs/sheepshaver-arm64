#!/bin/bash
set -e

PROJECT_DIR="/Users/matthewbriggs/Downloads/macemu-master 4"
XCODE_DIR="$PROJECT_DIR/SheepShaver/src/MacOSX"

echo "==> Cleaning DerivedData..."
rm -rf ~/Library/Developer/Xcode/DerivedData/SheepShaver-*

echo "==> Building Release..."
cd "$XCODE_DIR"
xcodebuild -project SheepShaver.xcodeproj -scheme SheepShaver -configuration Release build

echo "==> Copying to Desktop..."
rm -rf ~/Desktop/SheepShaver.app
cp -R ~/Library/Developer/Xcode/DerivedData/SheepShaver-*/Build/Products/Release/SheepShaver.app ~/Desktop/SheepShaver.app

echo "==> Done: ~/Desktop/SheepShaver.app"
