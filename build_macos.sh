#!/bin/sh
# Native macOS build: produces a self-contained SavvyCAN.app (Qt + libusb bundled).
# Requires: brew install qt@5 libusb
set -e
cd "$(dirname "$0")"
QT=$(brew --prefix qt@5)
export PATH="$QT/bin:$PATH"

rm -f .qmake.stash
qmake CONFIG+=release CONFIG+=sdk_no_version_check
make -j"$(sysctl -n hw.ncpu)"
macdeployqt SavvyCAN.app
codesign --force --deep -s - SavvyCAN.app

echo "Done. Run: open SavvyCAN.app"
