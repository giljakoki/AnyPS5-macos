import SwiftUI
import AppKit

struct GameInfo: Codable { var title: String?; var status: String?; var fps: String? }

struct Game: Identifiable {
    let id: String          // bundle name
    let url: URL
    var title: String
    var status: String
    var fps: String
    var titleID: String
    var art: NSImage?
    var logURL: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/AnyPS5/\(titleID).log") }
    var ebootPath: String { url.appendingPathComponent("Contents/MacOS/eboot").path }
}

func tierColor(_ s: String) -> Color {
    switch s.lowercased() {
    case "great": return Color(red: 0.25, green: 0.78, blue: 0.45)
    case "good": return Color(red: 0.35, green: 0.65, blue: 0.95)
    case "playable": return Color(red: 0.95, green: 0.75, blue: 0.25)
    case "experimental": return Color(red: 0.95, green: 0.45, blue: 0.3)
    default: return Color.gray
    }
}

let home = FileManager.default.homeDirectoryForCurrentUser
let appsDir = home.appendingPathComponent("AnyPS5/apps")
let jsonURL = home.appendingPathComponent("AnyPS5/launcher/games.json")

func loadGames() -> [Game] {
    let meta = (try? JSONDecoder().decode([String: GameInfo].self, from: Data(contentsOf: jsonURL))) ?? [:]
    let names = ((try? FileManager.default.contentsOfDirectory(atPath: appsDir.path)) ?? []).filter { $0.hasSuffix(".app") }.sorted()
    return names.map { n in
        let url = appsDir.appendingPathComponent(n)
        let script = (try? String(contentsOf: url.appendingPathComponent("Contents/MacOS/launch"), encoding: .utf8)) ?? ""
        var tid = ""
        if let r = script.range(of: "PPSA[0-9]+", options: .regularExpression) { tid = String(script[r]) }
        let sys = url.appendingPathComponent("Contents/Resources/game/app0/sce_sys")
        var art: NSImage? = nil
        for f in ["icon0.png", "pic0.png"] { if let i = NSImage(contentsOf: sys.appendingPathComponent(f)) { art = i; break } }
        if art == nil { art = NSImage(contentsOf: url.appendingPathComponent("Contents/Resources/AppIcon.icns")) }
        let m = meta[n]
        return Game(id: n, url: url, title: m?.title ?? String(n.dropLast(4)), status: m?.status ?? "Untested",
                    fps: m?.fps ?? "", titleID: tid, art: art)
    }
}

func isRunning(_ g: Game) -> Bool {
    let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/pgrep")
    p.arguments = ["-f", g.ebootPath]; p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
    do { try p.run(); p.waitUntilExit() } catch { return false }
    return p.terminationStatus == 0
}

func sh(_ path: String, _ args: [String]) {
    let p = Process(); p.executableURL = URL(fileURLWithPath: path); p.arguments = args
    p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
    try? p.run()
}

final class Model: ObservableObject {
    @Published var games: [Game] = loadGames()
    @Published var running: Set<String> = []
    var timer: Timer?
    init() {
        poll()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.poll() }
    }
    func poll() {
        let gs = games
        DispatchQueue.global().async {
            let r = Set(gs.filter(isRunning).map { $0.id })
            DispatchQueue.main.async { if r != self.running { self.running = r } }
        }
    }
    func play(_ g: Game) {
        guard !running.contains(g.id) else { return }
        sh("/usr/bin/open", ["-a", g.url.path])
        running.insert(g.id)
    }
    func stop(_ g: Game) {
        sh("/usr/bin/pkill", ["-TERM", "-f", g.ebootPath])
        DispatchQueue.global().asyncAfter(deadline: .now() + 5) {
            if isRunning(g) { sh("/usr/bin/pkill", ["-KILL", "-f", g.ebootPath]) }
        }
    }
    func reload() { games = loadGames(); poll() }
}

// ObservableObject instead of @State: the Command Line Tools lack the SwiftUI macro plugin.
final class HoverBox: ObservableObject { @Published var on = false }

struct Card: View {
    let g: Game; let running: Bool
    @ObservedObject var m: Model
    @StateObject private var hv = HoverBox()
    private var hover: Bool { hv.on }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack(alignment: .topTrailing) {
                Group {
                    if let a = g.art { Image(nsImage: a).resizable().aspectRatio(contentMode: .fill) }
                    else { Color.gray.opacity(0.3) }
                }
                .frame(height: 170).frame(maxWidth: .infinity).clipped()
                if running {
                    Text("Running").font(.caption.bold()).padding(.horizontal, 8).padding(.vertical, 3)
                        .background(Color.green, in: Capsule()).foregroundColor(.black).padding(8)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 12))
            HStack {
                Text(g.title).font(.headline).foregroundColor(.white).lineLimit(1)
                Spacer()
                Text(g.status).font(.caption.bold()).padding(.horizontal, 8).padding(.vertical, 2)
                    .background(tierColor(g.status).opacity(0.25), in: Capsule())
                    .foregroundColor(tierColor(g.status))
            }
            Text(g.fps.isEmpty ? " " : g.fps).font(.caption).foregroundColor(.gray)
            HStack {
                if running {
                    Button(action: { m.stop(g) }) { Text("Stop").frame(maxWidth: .infinity) }.tint(.red)
                } else {
                    Button(action: { m.play(g) }) { Text("Play").font(.headline).frame(maxWidth: .infinity) }.tint(.blue)
                }
                Menu {
                    Button("Open log") { NSWorkspace.shared.open(g.logURL) }
                    Button("Reveal in Finder") { NSWorkspace.shared.activateFileViewerSelecting([g.url]) }
                } label: { Image(systemName: "ellipsis.circle") }
                .menuStyle(.borderlessButton).frame(width: 30)
            }
            .buttonStyle(.borderedProminent).controlSize(.large)
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 16).fill(Color.white.opacity(hover ? 0.12 : 0.07)))
        .overlay(RoundedRectangle(cornerRadius: 16).stroke(running ? Color.green.opacity(0.7) : Color.white.opacity(hover ? 0.25 : 0.08), lineWidth: 1))
        .scaleEffect(hover ? 1.015 : 1)
        .animation(.easeOut(duration: 0.12), value: hover)
        .onHover { hv.on = $0 }
        .onTapGesture(count: 2) { m.play(g) }
        .contextMenu {
            Button("Open log") { NSWorkspace.shared.open(g.logURL) }
            Button("Reveal in Finder") { NSWorkspace.shared.activateFileViewerSelecting([g.url]) }
        }
    }
}

struct ContentView: View {
    @StateObject var m = Model()
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text("AnyPS5").font(.system(size: 28, weight: .bold)).foregroundColor(.white)
                    Spacer()
                    Button { m.reload() } label: { Image(systemName: "arrow.clockwise") }
                        .help("Reload games.json")
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 240, maximum: 340), spacing: 16)], spacing: 16) {
                    ForEach(m.games) { g in Card(g: g, running: m.running.contains(g.id), m: m) }
                }
            }.padding(24)
        }
        .background(Color(red: 0.05, green: 0.06, blue: 0.1))
        .frame(minWidth: 560, minHeight: 420)
    }
}

@main struct LauncherApp: App {
    var body: some Scene {
        WindowGroup("AnyPS5 Launcher") { ContentView().preferredColorScheme(.dark) }
            .defaultSize(width: 1000, height: 680)
    }
}
