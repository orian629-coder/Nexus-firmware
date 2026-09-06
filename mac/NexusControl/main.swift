// Nexus Control — a native macOS window that wraps the streamer's control UI.
//
// It launches (and owns the lifetime of) `nexus-streamer --serve`, waits for the local HTTP
// control server to come up, then shows the existing self-contained UI inside a WKWebView in a
// real app window — so you add/scan/pair/control speakers from a Mac app instead of a browser tab.
//
// The streamer binary is located, in order:
//   1. $NEXUS_STREAMER_BIN
//   2. next to this .app bundle (Contents/MacOS/nexus-streamer)
//   3. common build/install locations under the repo and /usr/local/bin
// If no binary is found we still open the window and try to attach to an already-running --serve.

import AppKit
import SwiftUI
import WebKit

let kPort = 8090
let kUrl = URL(string: "http://127.0.0.1:\(kPort)/")!

// MARK: - Streamer process manager

/// Owns the `nexus-streamer --serve` child process: starts it, restarts on crash, and kills it on
/// app quit so we never leak a background server.
final class StreamerProcess {
    private var process: Process?
    private(set) var launchedByUs = false

    /// Search for the streamer binary the app should run.
    static func findBinary() -> String? {
        let fm = FileManager.default
        if let env = ProcessInfo.processInfo.environment["NEXUS_STREAMER_BIN"],
           fm.isExecutableFile(atPath: env) {
            return env
        }
        var candidates: [String] = []
        // Bundled inside the .app (Contents/MacOS/nexus-streamer), same dir as our own executable.
        let exeDir = Bundle.main.bundlePath + "/Contents/MacOS"
        candidates.append(exeDir + "/nexus-streamer")
        // Repo build/install locations (dev convenience).
        let home = fm.homeDirectoryForCurrentUser.path
        for base in ["\(home)/nexus-speaker", fm.currentDirectoryPath] {
            candidates.append("\(base)/build/streamer/nexus-streamer")
            candidates.append("\(base)/build-rpi/streamer/nexus-streamer")
        }
        candidates.append("/usr/local/bin/nexus-streamer")
        for c in candidates where fm.isExecutableFile(atPath: c) { return c }
        return nil
    }

    func start() {
        guard let bin = Self.findBinary() else {
            NSLog("Nexus Control: no nexus-streamer binary found; will try to attach to a running server.")
            return
        }
        let p = Process()
        p.executableURL = URL(fileURLWithPath: bin)
        p.arguments = ["--serve", String(kPort)]
        p.terminationHandler = { [weak self] proc in
            NSLog("Nexus Control: streamer exited (status \(proc.terminationStatus)).")
            self?.process = nil
        }
        do {
            try p.run()
            process = p
            launchedByUs = true
            NSLog("Nexus Control: launched \(bin) --serve \(kPort)")
        } catch {
            NSLog("Nexus Control: failed to launch streamer: \(error)")
        }
    }

    func stop() {
        guard let p = process, p.isRunning else { return }
        p.terminate()
        // Give it a beat to release the port, then hard-kill if still alive.
        let deadline = Date().addingTimeInterval(2)
        while p.isRunning && Date() < deadline { usleep(50_000) }
        if p.isRunning { kill(p.processIdentifier, SIGKILL) }
    }
}

/// Polls the control URL until it answers (or times out), so we don't show a blank webview before
/// the server is listening.
func waitForServer(timeout: TimeInterval, _ done: @escaping (Bool) -> Void) {
    let start = Date()
    func probe() {
        var req = URLRequest(url: kUrl)
        req.timeoutInterval = 1
        // GET, not HEAD: the streamer answers HEAD with 405. The probe only checks that SOME
        // response arrived, so HEAD happened to work — but it made "the server is up" indis-
        // tinguishable from "the server is up and rejecting us", which is not a distinction worth
        // leaving to luck at startup.
        req.httpMethod = "GET"
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            if (resp as? HTTPURLResponse) != nil {
                DispatchQueue.main.async { done(true) }
            } else if Date().timeIntervalSince(start) > timeout {
                DispatchQueue.main.async { done(false) }
            } else {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { probe() }
            }
        }.resume()
    }
    probe()
}

// MARK: - WebView

struct WebView: NSViewRepresentable {
    let url: URL
    @Binding var reloadToken: Int

    func makeNSView(context: Context) -> WKWebView {
        let cfg = WKWebViewConfiguration()
        let wv = WKWebView(frame: .zero, configuration: cfg)
        wv.load(URLRequest(url: url))
        context.coordinator.webView = wv
        return wv
    }

    func updateNSView(_ wv: WKWebView, context: Context) {
        if context.coordinator.lastReload != reloadToken {
            context.coordinator.lastReload = reloadToken
            wv.load(URLRequest(url: url))
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }
    final class Coordinator { var webView: WKWebView?; var lastReload = 0 }
}

// MARK: - Root view

/// One window, two screens. Edit REPLACES the list rather than opening a panel over it: a popover
/// or a second window would leave two places showing speaker state at once, and the operator would
/// have to keep track of which one is authoritative. Going in and coming back is unambiguous.
enum Screen: Equatable {
    case discovery
    case edit(String)  // device id
}

struct RootView: View {
    @StateObject private var state = AppState()
    @State private var ready = false
    @State private var failed = false

    var body: some View {
        ZStack {
            Nexus.bg.ignoresSafeArea()
            if ready {
                content
            } else {
                VStack(spacing: 16) {
                    ProgressView().controlSize(.large)
                    Text(failed ? "לא הצלחתי להתחבר לסטרימר" : "מפעיל את הסטרימר…")
                        .foregroundColor(.white).font(.title3)
                    if failed {
                        Text("ודא ש-nexus-streamer בנוי (build/streamer/nexus-streamer)")
                            .foregroundColor(.gray).font(.callout)
                        Button("נסה שוב") { retry() }
                    }
                }
            }
        }
        .frame(minWidth: 720, minHeight: 760)
        .environment(\.layoutDirection, .rightToLeft)
        .environmentObject(state)
        .onAppear(perform: boot)
        .onDisappear { state.stop() }
    }

    private var content: some View {
        VStack(spacing: 0) {
            header
            // DiscoveryView owns its own scrolling and fixed action bar, so it must not be wrapped
            // in another ScrollView — that would collapse the device list to its natural height.
            switch state.screen {
            case .discovery:
                DiscoveryView()
            case .edit(let deviceId):
                if let s = state.speakers.first(where: { $0.deviceId == deviceId }) {
                    SpeakerEditScreen(speaker: s)
                } else {
                    // The speaker was removed (or dropped off the network) while its screen was
                    // open. Returning to the list is the only honest thing to show.
                    VStack(spacing: 12) {
                        Text("הרמקול אינו זמין יותר").foregroundColor(Nexus.muted)
                        Button("חזרה לרשימה") { state.screen = .discovery }
                            .buttonStyle(NexusButtonStyle(ghost: true))
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
            statusBar
        }
    }

    /// The app bar. Its title says which screen you are on, so the window never has to be guessed
    /// at from its contents.
    private var header: some View {
        AppBar(title: barTitle) {
            switch state.screen {
            case .discovery:
                BarAction(title: "רענן") {
                    Task { await state.refreshAll() }
                }
            case .edit:
                BarAction(title: "חזרה") {
                    state.screen = .discovery
                }
            }
        }
    }

    private var barTitle: String {
        switch state.screen {
        case .discovery: return "רמקולים"
        case .edit(let id):
            return state.speakers.first { $0.deviceId == id }?.name ?? "רמקול"
        }
    }


    /// One line that always says what just happened — errors here rather than in a modal, so a
    /// rejected command is visible without interrupting the operator mid-adjustment.
    private var statusBar: some View {
        HStack {
            if let err = state.errorMessage {
                Text(err).foregroundColor(Nexus.alert)
            } else if let poll = state.pollFailure {
                Text(poll).foregroundColor(Nexus.alert)
            } else if !state.status.isEmpty {
                Text(state.status).foregroundColor(Nexus.muted)
            }
            Spacer()
            // A live heartbeat: without it, a poll that has silently died looks exactly like a
            // poll that is working on unchanged data.
            if let t = state.lastPollAt {
                Text("עודכן \(t.formatted(date: .omitted, time: .standard))")
                    .foregroundColor(Nexus.muted)
            }
            Text("\(state.speakers.count) רמקולים").foregroundColor(Nexus.muted)
        }
        .font(.system(size: 11))
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(Nexus.surface)
    }

    private func boot() {
        appProcess.start()
        waitForServer(timeout: 15) { ok in
            if ok {
                ready = true
                state.start()
            } else {
                failed = true
            }
        }
    }

    private func retry() {
        failed = false
        appProcess.start()
        waitForServer(timeout: 15) { ok in
            if ok { ready = true; state.start() } else { failed = true }
        }
    }
}

// Global so AppDelegate can stop it on quit.
let appProcess = StreamerProcess()

// MARK: - App lifecycle

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Force dark appearance regardless of the system setting.
        //
        // Every SwiftUI control that draws its own text — TextField, Picker, Stepper, Toggle —
        // takes it from the system label colour, which is BLACK under a light appearance. On this
        // window's dark greys that is the one combination that stops being readable, and setting
        // .foregroundColor on each control individually does not reach the text those controls
        // render internally. Pinning the appearance fixes all of them at once, and keeps the app
        // looking the same on a machine set to Light.
        NSApp.appearance = NSAppearance(named: .darkAqua)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ notification: Notification) { appProcess.stop() }
}

@main
struct NexusControlApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    var body: some Scene {
        WindowGroup("Nexus Control") {
            RootView()
        }
        .windowResizability(.contentSize)
    }
}
