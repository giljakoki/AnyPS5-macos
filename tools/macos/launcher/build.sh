#!/bin/sh
# Builds "AnyPS5 Launcher.app" into ~/AnyPS5/ and seeds ~/AnyPS5/launcher/games.json if missing.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
app="$HOME/AnyPS5/AnyPS5 Launcher.app"
tmp="$(mktemp -d)"
rm -rf "$app"; mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources" "$HOME/AnyPS5/launcher"
swiftc -O -parse-as-library -o "$app/Contents/MacOS/AnyPS5Launcher" "$here/Launcher.swift"
# Icon: gamepad symbol on dark rounded tile
cat > "$tmp/icon.swift" <<'S'
import AppKit
let s: CGFloat = 1024
let img = NSImage(size: NSSize(width: s, height: s))
img.lockFocus()
let r = NSRect(x: 60, y: 60, width: s-120, height: s-120)
let p = NSBezierPath(roundedRect: r, xRadius: 200, yRadius: 200)
NSGradient(colors: [NSColor(red: 0.1, green: 0.2, blue: 0.5, alpha: 1), NSColor(red: 0.03, green: 0.04, blue: 0.1, alpha: 1)])!.draw(in: p, angle: -90)
if let sym = NSImage(systemSymbolName: "gamecontroller.fill", accessibilityDescription: nil)?
    .withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: 520, weight: .regular)) {
    let t = sym.copy() as! NSImage; t.lockFocus(); NSColor.white.set()
    NSRect(origin: .zero, size: t.size).fill(using: .sourceAtop); t.unlockFocus()
    let w = t.size.width, h = t.size.height
    t.draw(in: NSRect(x: (s-w)/2, y: (s-h)/2, width: w, height: h))
}
img.unlockFocus()
let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!
try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
S
swiftc -o "$tmp/icon" "$tmp/icon.swift" && "$tmp/icon" "$tmp/base.png"
set="$tmp/AppIcon.iconset"; mkdir "$set"
for z in 16 32 128 256 512; do
  sips -z $z $z "$tmp/base.png" --out "$set/icon_${z}x${z}.png" >/dev/null
  sips -z $((z*2)) $((z*2)) "$tmp/base.png" --out "$set/icon_${z}x${z}@2x.png" >/dev/null
done
iconutil -c icns "$set" -o "$app/Contents/Resources/AppIcon.icns"
cat > "$app/Contents/Info.plist" <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>AnyPS5 Launcher</string>
<key>CFBundleDisplayName</key><string>AnyPS5 Launcher</string>
<key>CFBundleIdentifier</key><string>org.anyps5.launcher</string>
<key>CFBundleExecutable</key><string>AnyPS5Launcher</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>LSMinimumSystemVersion</key><string>13.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
P
codesign --force -s - "$app" >/dev/null 2>&1 || true
[ -f "$HOME/AnyPS5/launcher/games.json" ] || cp "$here/games.json" "$HOME/AnyPS5/launcher/games.json"
rm -rf "$tmp"
echo "Installed $app"
