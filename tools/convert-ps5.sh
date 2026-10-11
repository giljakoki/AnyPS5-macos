#!/bin/bash
# Converts a PS5 title folder into a macOS application with the AnyPS5 macOS package.
#
#   bash convert-ps5.sh <title folder> <AppName> [relinker options...]
#
# The title folder holds eboot.elf or eboot.bin, sce_sys/param.json and the title's data. Decrypted
# module copies named <module>.elf replace the encrypted <module>.prx beside them (the originals move
# to <title folder>-encrypted-backup). A fake-signed eboot.bin or module (homebrew) is unpacked with
# unfself.py. Unity titles get --module-dir for Media/Modules and Media/Plugins. Extra arguments go to
# the relinker, for example --exclude-sce-module libc.prx. INPUT=<file> puts a key mapping in the app.
# The app is written next to the relinker as <AppName>.app and opened.
here=$(cd "$(dirname "$0")" && pwd)
if [ -x "$here/relinker" ]; then tools=$here; else tools=${ANYPS5_TOOLS:-$HOME/Downloads/anyps5-macos-x86_64}; fi
unfself=$tools/unfself.py
[ -f "$unfself" ] || unfself=$here/unfself.py
[ -n "$1" ] && [ -n "$2" ] || { echo "usage: bash convert-ps5.sh <title folder> <AppName> [relinker options...]"; exit 1; }
game=$(cd "$1" && pwd) || exit 1
name=$2
shift 2
[ -x "$tools/relinker" ] || { echo "STOP: no relinker in $tools; set ANYPS5_TOOLS to the unpacked anyps5-macos-x86_64 folder"; exit 1; }
magic() { head -c 4 "$1" | od -An -tx1 | tr -d ' \n'; }
backup="$game-encrypted-backup"
cd "$game" || exit 1
for d in sce_module sce_modules prx Media/Plugins Media/Modules; do
  [ -d "$d" ] || continue
  for e in "$d"/*.elf; do
    [ -f "$e" ] || continue
    p="${e%.elf}.prx"
    mkdir -p "$backup/$d"
    [ -f "$p" ] && mv "$p" "$backup/$d/"
    mv "$e" "$p"
  done
  for p in "$d"/*.prx "$d"/*.sprx; do
    [ -f "$p" ] || continue
    case "$(magic "$p")" in
      4f153d1d|5414f5ee)
        if [ -f "$unfself" ] && python3 "$unfself" "$p" "$p.unpacked" > /dev/null 2>&1; then
          mkdir -p "$backup/$d"
          mv "$p" "$backup/$d/"
          mv "$p.unpacked" "$p"
          echo "unpacked fake-signed module: $p"
        else
          rm -f "$p.unpacked"
          echo "still encrypted: $p"
        fi;;
    esac
  done
done
eboot=
for f in eboot.elf eboot.bin; do
  if [ -f "$f" ] && [ "$(magic "$f")" = 7f454c46 ]; then eboot="$game/$f"; break; fi
done
if [ -z "$eboot" ] && [ -f eboot.bin ] && [ -f "$unfself" ]; then
  if python3 "$unfself" eboot.bin eboot.elf; then
    mkdir -p "$backup"
    mv eboot.bin "$backup/"
    eboot="$game/eboot.elf"
  fi
fi
[ -n "$eboot" ] || { echo "STOP: no decrypted eboot.elf or eboot.bin in $game"; exit 1; }
[ -f sce_sys/param.json ] || { echo "STOP: no sce_sys/param.json; copy the complete sce_sys folder of the dump"; exit 1; }
find . -name '* [0-9].prx' -not -path './sce_sys/*' | while read -r f; do echo "possible duplicate copy (move it out of the title folder): $f"; done
options=()
for d in Media/Modules Media/Plugins; do [ -d "$d" ] && options+=(--module-dir "$d"); done
packaging=()
[ -n "$INPUT" ] && packaging+=(--input-config "$INPUT")
cd "$tools" || exit 1
out="out-$name"
rm -rf "$out" "$name.app"
mkdir -p "$out"
echo "converting $eboot ${options[*]} $*"
./relinker --macos --to-rosetta "${options[@]}" "$@" "$eboot" "$out/eboot" > "$out.log" 2>&1
status=$?
grep -E "Processing|Warning|FAIL" "$out.log"
[ "$status" -eq 0 ] || { echo "STOP: the relinker failed; its output is in $tools/$out.log"; exit 1; }
python3 package_macos_app.py --relinked "$out" --game "$game" --libs libs --vulkan vulkan "${packaging[@]}" "$name.app" || exit 1
echo "opening $name.app; if it closes, run: tail -n 100 \"\$(ls -t ~/Library/Logs/AnyPS5/*.log | head -n 1)\""
open "$name.app"
