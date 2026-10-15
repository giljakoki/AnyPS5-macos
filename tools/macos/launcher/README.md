# AnyPS5 Launcher

SwiftUI launcher for the game bundles in `~/AnyPS5/apps/`.

- Build/install: `./build.sh` (needs swiftc, sips, iconutil) -> `~/AnyPS5/AnyPS5 Launcher.app`
- Per-game status/fps notes: `~/AnyPS5/launcher/games.json` (seeded from `games.json` if missing; keyed by bundle name; reload button in the app). Unlisted bundles show as "Untested".
- Play = `open -a <bundle>`; "Running" is detected via `pgrep -f <bundle>/Contents/MacOS/eboot` every 2 s; Stop sends SIGTERM, then SIGKILL after 5 s.
