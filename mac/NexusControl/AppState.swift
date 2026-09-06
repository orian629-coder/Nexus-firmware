// Observable state behind the UI: polls the streamer and holds what it reported.
//
// The governing rule, inherited from the web UI and enforced here too: a control shows what the
// SPEAKER CONFIRMED, never what the user just asked for. A slider the user is dragging is marked
// pending so a poll landing mid-drag cannot yank it, and once the reply arrives the control settles
// on the speaker's answer — snapping back if the speaker disagreed.

import Combine
import Foundation

@MainActor
final class AppState: ObservableObject {
    @Published var speakers: [Speaker] = []
    @Published var levels: Levels = .idle
    @Published var devices: [AudioDevice] = []
    @Published var zones: [Zone] = []
    @Published var dsp: DspConfig = .flat

    @Published var selectedSpeakerId: String?
    @Published var selectedDeviceId: String?
    @Published var selectedZoneId: String?

    /// Which screen the single window is showing. Held here rather than in the view so that an
    /// action can navigate — removing a speaker, for instance, has to leave its edit screen.
    @Published var screen: Screen = .discovery

    @Published var status: String = ""
    @Published var errorMessage: String?

    /// Why the background poll is failing, if it is. Shown in the status bar: a UI that simply
    /// stops updating gives the operator nothing to act on, and "nothing happens" is the hardest
    /// symptom to diagnose precisely because it looks the same as "everything is fine and idle".
    @Published var pollFailure: String?
    /// When speakers were last read successfully — proves the poll is alive even when values are
    /// unchanged, which is otherwise indistinguishable from a frozen UI.
    @Published var lastPollAt: Date?

    // ── discovery (the first screen) ──
    /// Subnet to sweep. Prefilled from the Mac's own address so the common case needs no typing.
    @Published var networkRange: String = ""
    @Published var discovered: [DiscoveredDevice] = []
    @Published var scanning = false
    @Published var scanMessage: String = ""
    /// Which discovered devices are ticked for broadcast.
    @Published var selectedForBroadcast = Set<String>()

    /// Measurement results, keyed by device id.
    @Published var measurements: [String: MeasurementReply] = [:]
    @Published var measuring = false
    @Published var hardwareRtlMs: Double = 0
    @Published var networkRtlMs: Double = 0

    /// Controls the user is actively changing. A background poll must not overwrite these.
    private var pending = Set<String>()

    private let api = StreamerAPI()
    private var levelTimer: Timer?
    private var stateTimer: Timer?

    var selectedSpeaker: Speaker? {
        speakers.first { $0.deviceId == selectedSpeakerId }
    }

    func start() {
        if networkRange.isEmpty { networkRange = Self.localSubnet() }
        Task {
            await refreshAll()
            // Scan once on open: the first screen is "find my speakers", and making the user press
            // a button to see what is obviously the point of the screen is friction for nothing.
            scanNetwork()
        }
        // Levels at 10 Hz: fast enough to read a transient, slow enough not to flood loopback.
        levelTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.refreshLevels() }
        }
        // Everything else at 2 s — speaker state changes on the streamer's own 5 s poll anyway.
        stateTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.refreshState() }
        }
    }

    func stop() {
        levelTimer?.invalidate()
        stateTimer?.invalidate()
    }

    // MARK: - Polling

    func refreshAll() async {
        await refreshState()
        await refreshLevels()
        devices = (try? await api.sources()) ?? []
        if selectedDeviceId == nil {
            // Default to a loopback device: "play what this Mac is playing" is the common case.
            selectedDeviceId = devices.first(where: { $0.loopback })?.id ?? devices.first?.id
        }
        if let d = try? await api.dsp() { applyDsp(d) }
    }

    private func refreshLevels() async {
        do {
            levels = try await api.levels()
            pollFailure = nil
        } catch {
            // Surfaced rather than swallowed: `try?` here is what turns a broken connection into a
            // silent, motionless UI — the failure mode that looks like "nothing happens".
            pollFailure = "שגיאת תקשורת: \(error.localizedDescription)"
        }
    }

    private func refreshState() async {
        do {
            let s = try await api.speakers()
            speakers = s
            if selectedSpeakerId == nil { selectedSpeakerId = s.first?.deviceId }
            pollFailure = nil
            lastPollAt = Date()
        } catch {
            pollFailure = "לא ניתן לקרוא רמקולים: \(error.localizedDescription)"
        }
        if let z = try? await api.zones() {
            zones = z
            if selectedZoneId == nil || !z.contains(where: { $0.zoneId == selectedZoneId }) {
                selectedZoneId = z.first?.zoneId
            }
        }
    }

    /// Only writes the fields the user is not currently holding.
    private func applyDsp(_ c: DspConfig) {
        var next = dsp
        if !pending.contains("master") { next = DspConfig(bypass: c.bypass,
                                                          inputGainDb: next.inputGainDb,
                                                          masterVolumeDb: c.masterVolumeDb,
                                                          limiterEnabled: c.limiterEnabled,
                                                          eqGainsDb: next.eqGainsDb) }
        if !pending.contains("input") { next = DspConfig(bypass: next.bypass,
                                                         inputGainDb: c.inputGainDb,
                                                         masterVolumeDb: next.masterVolumeDb,
                                                         limiterEnabled: next.limiterEnabled,
                                                         eqGainsDb: next.eqGainsDb) }
        if !pending.contains("eq") { next = DspConfig(bypass: next.bypass,
                                                      inputGainDb: next.inputGainDb,
                                                      masterVolumeDb: next.masterVolumeDb,
                                                      limiterEnabled: next.limiterEnabled,
                                                      eqGainsDb: c.eqGainsDb) }
        dsp = next
    }

    // MARK: - Actions

    /// `refetch` must be false for anything that already merged the speaker's confirmed reply.
    ///
    /// This is what made mute / gain / phase / delay appear broken while volume worked. Every
    /// command ended with refreshState(), which overwrote the just-merged confirmation with a
    /// fresh poll — and the streamer only re-polls the speaker every 5 s, so that poll usually
    /// still carried the OLD value. The control snapped straight back and the change looked lost.
    ///
    /// Volume escaped it because Stepper drives its own displayed value from the binding's get:,
    /// which the poll happens to update on the next cycle; a Toggle or Slider reverts visibly.
    private func run(_ key: String?, _ label: String, refetch: Bool = true,
                     _ work: @escaping () async throws -> Void) {
        if let key { pending.insert(key) }
        Task {
            do {
                try await work()
                status = "\(label) — אושר"
                errorMessage = nil
            } catch {
                errorMessage = error.localizedDescription
                status = ""
            }
            if let key { pending.remove(key) }
            if refetch { await refreshState() }
        }
    }

    func setMasterVolume(_ db: Double) {
        dsp = DspConfig(bypass: dsp.bypass, inputGainDb: dsp.inputGainDb, masterVolumeDb: db,
                        limiterEnabled: dsp.limiterEnabled, eqGainsDb: dsp.eqGainsDb)
        run("master", "עוצמת מאסטר") { [self] in
            let applied = try await api.setDsp(["master_volume_db": db])
            await MainActor.run { applyDsp(applied) }
        }
    }

    func setInputGain(_ db: Double) {
        dsp = DspConfig(bypass: dsp.bypass, inputGainDb: db, masterVolumeDb: dsp.masterVolumeDb,
                        limiterEnabled: dsp.limiterEnabled, eqGainsDb: dsp.eqGainsDb)
        run("input", "הגבר כניסה") { [self] in
            let applied = try await api.setDsp(["input_gain_db": db])
            await MainActor.run { applyDsp(applied) }
        }
    }

    func setEqBand(_ index: Int, _ db: Double) {
        var gains = dsp.eqGainsDb
        guard index < gains.count else { return }
        gains[index] = db
        dsp = DspConfig(bypass: dsp.bypass, inputGainDb: dsp.inputGainDb,
                        masterVolumeDb: dsp.masterVolumeDb, limiterEnabled: dsp.limiterEnabled,
                        eqGainsDb: gains)
        run("eq", "EQ") { [self] in
            let applied = try await api.setDsp(["eq_gains_db": gains])
            await MainActor.run { applyDsp(applied) }
        }
    }

    func toggleBypass() {
        let next = !dsp.bypass
        run(nil, next ? "Bypass פעיל" : "Bypass כבוי") { [self] in
            let applied = try await api.setDsp(["bypass": next])
            await MainActor.run { applyDsp(applied) }
        }
    }

    func flattenEq() {
        let flat = Array(repeating: 0.0, count: kEqFrequencies.count)
        run("eq", "איפוס EQ") { [self] in
            let applied = try await api.setDsp(["eq_gains_db": flat])
            await MainActor.run { applyDsp(applied) }
        }
    }

    /// Merges what the speaker confirmed into the row for that speaker, immediately.
    ///
    /// The streamer's command reply already contains the speaker's answer (~130 ms on this LAN), so
    /// waiting for the next poll would make an applied change look like it took seconds. Only the
    /// fields the speaker actually reported are written; the rest of the row is left alone.
    private func merge(_ c: Confirmed) {
        guard let i = speakers.firstIndex(where: { $0.deviceId == c.deviceId }) else { return }
        let old = speakers[i]
        speakers[i] = Speaker(
            deviceId: old.deviceId, name: old.name, host: old.host,
            online: c.online, confirmed: c.confirmed,
            volume: c.volume ?? old.volume,
            muted: c.muted ?? old.muted,
            delayMs: c.delayMs ?? old.delayMs,
            eqProfile: c.eqProfile ?? old.eqProfile,
            gainDb: c.gainDb ?? old.gainDb,
            phaseInvert: c.phaseInvert ?? old.phaseInvert,
            ageMs: 0,  // just confirmed
            telemetry: c.telemetry ?? old.telemetry,
            wifiSignalDbm: c.wifiSignalDbm ?? old.wifiSignalDbm,
            lastError: nil)
    }

    func setSpeakerVolume(_ id: String, _ v: Int) {
        run("vol", "עוצמה", refetch: false) { [self] in
            if let c = try await api.setVolume(id, v) { await MainActor.run { merge(c) } }
        }
    }
    func setSpeakerMute(_ id: String, _ m: Bool) {
        run("mute", "השתקה", refetch: false) { [self] in
            if let c = try await api.setMute(id, m) { await MainActor.run { merge(c) } }
        }
    }
    func setSpeakerGain(_ id: String, _ db: Double) {
        run("gain", "כוונון", refetch: false) { [self] in
            if let c = try await api.setGain(id, db) { await MainActor.run { merge(c) } }
        }
    }
    func setSpeakerPhase(_ id: String, _ p: Bool) {
        run("phase", "פאזה", refetch: false) { [self] in
            if let c = try await api.setPhase(id, p) { await MainActor.run { merge(c) } }
        }
    }
    func setSpeakerDelay(_ id: String, _ ms: Int) {
        run("delay", "השהיה", refetch: false) { [self] in
            if let c = try await api.setDelay(id, ms) { await MainActor.run { merge(c) } }
        }
    }
    func removeSpeaker(_ id: String) {
        // Leave the speaker's own screen first: staying on it would show controls for something
        // that no longer exists.
        if screen == .edit(id) { screen = .discovery }
        run(nil, "הוסר") { [self] in try await api.removeSpeaker(id) }
    }

    func startBroadcast() {
        guard let device = selectedDeviceId else { errorMessage = "בחר מקור אודיו"; return }
        guard let zone = selectedZoneId else { errorMessage = "אין אזור — צור אזור קודם"; return }
        run(nil, "שידור החל") { [self] in
            try await api.play(zoneId: zone, kind: "device", uri: device)
        }
    }

    func stopBroadcast() {
        guard let zone = zones.first(where: { $0.zoneId == selectedZoneId }) else { return }
        run(nil, "נעצר") { [self] in
            for member in zone.members { try? await api.transport(member, "stop") }
        }
    }

    // MARK: - Discovery

    /// This Mac's own /24, so the range field is prefilled with the network the speakers are
    /// actually on. Falls back to the most common home range rather than to an empty box.
    static func localSubnet(fallback: String = "192.168.1.0/24") -> String {
        var addr = ""
        var ifaddr: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifaddr) == 0, let first = ifaddr else { return fallback }
        defer { freeifaddrs(ifaddr) }

        for ptr in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let flags = Int32(ptr.pointee.ifa_flags)
            guard let sa = ptr.pointee.ifa_addr, sa.pointee.sa_family == UInt8(AF_INET) else { continue }
            // Skip loopback and anything that is down.
            guard (flags & IFF_UP) == IFF_UP, (flags & IFF_LOOPBACK) == 0 else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(sa, socklen_t(sa.pointee.sa_len), &host, socklen_t(host.count),
                           nil, 0, NI_NUMERICHOST) == 0 {
                let ip = String(cString: host)
                if !ip.hasPrefix("169.254") {  // link-local means no real network
                    addr = ip
                    break
                }
            }
        }
        guard !addr.isEmpty else { return fallback }
        let parts = addr.split(separator: ".")
        guard parts.count == 4 else { return fallback }
        return "\(parts[0]).\(parts[1]).\(parts[2]).0/24"
    }

    /// Sweeps the subnet for Nexus speakers.
    ///
    /// Only devices that answer like a Nexus speaker are listed. A full ARP sweep would show every
    /// host on the LAN — the TV, the router, phones — and every one of those rows would sit at
    /// "Disconnected" forever with controls that can never do anything.
    func scanNetwork() {
        guard !scanning else { return }
        scanning = true
        scanMessage = "סורק…"
        Task {
            do {
                let found = try await api.scanNetwork(range: networkRange)
                discovered = found
                scanMessage = found.isEmpty
                    ? "לא נמצאו רמקולי Nexus ברשת"
                    : "נמצאו \(found.count) רמקולים"
                // Anything already added to the registry starts ticked, so the common case —
                // scan, then broadcast — needs no extra clicking.
                for d in found where speakers.contains(where: { $0.deviceId == d.deviceId }) {
                    selectedForBroadcast.insert(d.deviceId)
                }
            } catch {
                scanMessage = "הסריקה נכשלה: \(error.localizedDescription)"
            }
            scanning = false
        }
    }

    func selectAllDiscovered() {
        selectedForBroadcast = Set(discovered.map(\.deviceId))
        applySelection()
    }

    func deselectAllDiscovered() {
        selectedForBroadcast.removeAll()
        applySelection()
    }

    func toggleSelection(_ deviceId: String) {
        if selectedForBroadcast.contains(deviceId) {
            selectedForBroadcast.remove(deviceId)
        } else {
            selectedForBroadcast.insert(deviceId)
        }
        applySelection()
    }

    /// Makes the current tick boxes true of the running stream.
    ///
    /// While audio is playing this RETARGETS — the source keeps running and only the fan-out
    /// changes, so ticking a speaker mid-track adds it without a gap. Calling play again instead
    /// would rebuild the source (a new capture process on macOS) and restart everything audibly.
    ///
    /// While nothing is playing it does nothing: there is no stream to retarget, and starting one
    /// because a box was ticked would be a surprise.
    private func applySelection() {
        guard levels.state == "playing" else { return }
        let chosen = selectedForBroadcast
        Task {
            guard !chosen.isEmpty else {
                // Everything unticked: stop rather than leave the last speaker playing.
                for id in speakers.map(\.deviceId) { try? await api.transport(id, "stop") }
                return
            }
            do {
                let zoneId = try await api.zoneMatching(members: chosen)
                try await api.retarget(zoneId: zoneId)
            } catch {
                errorMessage = error.localizedDescription
            }
        }
    }

    /// Adds a discovered speaker to the registry so it can be controlled and streamed to.
    func addDiscovered(_ d: DiscoveredDevice) {
        run(nil, "נוסף \(d.deviceId)") { [self] in
            try await api.addSpeaker(id: d.deviceId, name: d.deviceId, host: d.ip)
        }
    }

    /// Broadcasts to exactly the ticked speakers.
    ///
    /// Builds the zone from the selection rather than reusing whatever zone happens to exist: the
    /// tick boxes are the user's statement of intent, and quietly streaming to a different set
    /// would be worse than refusing.
    func broadcastToSelection() {
        guard let device = selectedDeviceId else { errorMessage = "בחר מקור אודיו"; return }
        let chosen = selectedForBroadcast
        guard !chosen.isEmpty else { errorMessage = "לא נבחרו רמקולים"; return }

        run(nil, "שידור החל") { [self] in
            // Make sure every selected speaker is registered before it can join a zone.
            for id in chosen where !speakers.contains(where: { $0.deviceId == id }) {
                if let d = discovered.first(where: { $0.deviceId == id }) {
                    try? await api.addSpeaker(id: d.deviceId, name: d.deviceId, host: d.ip)
                }
            }
            let zoneId = try await api.zoneMatching(members: chosen)
            try await api.play(zoneId: zoneId, kind: "device", uri: device)
        }
    }

    func transport(_ id: String, _ action: String) {
        run(nil, action == "play" ? "ניגון" : "עצירה", refetch: false) { [self] in
            try await api.transport(id, action)
        }
    }

    /// Measures ONE speaker. Reached from that speaker's own Edit window, so there is never a
    /// question of which speaker a reading belongs to.
    func measure(_ deviceId: String) {
        guard !measuring else { return }
        measuring = true
        errorMessage = nil
        Task {
            let name = speakers.first { $0.deviceId == deviceId }?.name ?? deviceId
            status = "מודד \(name)…"
            do {
                measurements[deviceId] = try await api.measure(
                    deviceId, hardwareRtlMs: hardwareRtlMs, networkRtlMs: networkRtlMs)
                status = "המדידה הושלמה"
            } catch {
                measurements[deviceId] = MeasurementReply(
                    ok: false, message: error.localizedDescription, mics: [],
                    synchronized: nil, limitation: nil)
                status = ""
            }
            measuring = false
        }
    }

    /// Measures every online speaker, ONE AT A TIME: two speakers chirping together would overlap
    /// in the same recording and the correlation could not tell which peak belongs to which.
    func measureAll() {
        guard !measuring else { return }
        measuring = true
        measurements.removeAll()
        errorMessage = nil
        Task {
            for s in speakers where s.online {
                status = "מודד \(s.name)…"
                do {
                    let r = try await api.measure(s.deviceId,
                                                  hardwareRtlMs: hardwareRtlMs,
                                                  networkRtlMs: networkRtlMs)
                    measurements[s.deviceId] = r
                } catch {
                    measurements[s.deviceId] = MeasurementReply(
                        ok: false, message: error.localizedDescription, mics: [],
                        synchronized: nil, limitation: nil)
                }
            }
            status = "המדידה הושלמה"
            measuring = false
        }
    }
}
