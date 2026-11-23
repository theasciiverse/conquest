#!/bin/sh
# Lays out the av archive in stage/: the menu (conquest), the client,
# the server and its helpers, the universe definition (conqinitrc)
# beside them, the player's guide, the licence, README.md and
# PATCHES.md.
set -eu
cd "$(dirname "$0")/.."
rm -rf stage && mkdir -p stage
cp av-build/bin/conquest av-build/bin/conquest-tty av-build/bin/conquestd \
   av-build/bin/conqoper av-build/bin/conqdriv stage/
cp initfiles/conqinitrc stage/
cp docs/conquest-guide.md stage/
cp LICENSE.txt PATCHES.md stage/
cp asciiverse/README.md stage/README.md
