// Typed client for the streamer's local control API.
//
// Everything the UI shows comes through here, and the types mirror what the streamer actually
// returns rather than what the UI wishes it had. In particular `confirmed` and the optional fields
// are modelled as optionals on purpose: the streamer distinguishes "the speaker reported this
// value" from "we have not heard from it", and collapsing that into a default would make a speaker
// that has never answered look identical to one reporting zero.

import Foundation

// MARK: - Models

struct Speaker: Identifiable, Decodable {
    let deviceId: String
    let name: String
    let host: String
    let online: Bool
    let confirmed: Bool

    // Present only once the speaker has actually confirmed them.
    let volume: Int?
    let muted: Bool?
    let delayMs: Int?
    let eqProfile: String?
    let gainDb: Double?
    let phaseInvert: Bool?
    let ageMs: Int?
    let telemetry: Telemetry?
    let wifiSignalDbm: Int?
    let lastError: String?

    var id: String { deviceId }

    enum CodingKeys: String, CodingKey {
        case deviceId = "device_id", name, host, online, confirmed
        case volume, muted
        case delayMs = "delay_ms"
        case eqProfile = "eq_profile"
        case gainDb = "gain_db"
        case phaseInvert = "phase_invert"
        case ageMs = "age_ms"
        case telemetry
        case wifiSignalDbm = "wifi_signal_dbm"
        case lastError = "last_error"
    }
}

/// What a speaker reported back inside a command reply.
///
/// A separate type from `Speaker` because the reply carries only the speaker's own state — no
/// `name` or `host`, which are the streamer's addressing rather than anything the speaker said.
/// Decoding this into `Speaker` would fail on those missing fields and silently produce nothing.
struct Confirmed: Decodable {
    let deviceId: String
    let online: Bool
    let confirmed: Bool
    let volume: Int?
    let muted: Bool?
    let delayMs: Int?
    let eqProfile: String?
    let gainDb: Double?
    let phaseInvert: Bool?
    let telemetry: Telemetry?
    let wifiSignalDbm: Int?

    enum CodingKeys: String, CodingKey {
        case deviceId = "device_id", online, confirmed, volume, muted
        case delayMs = "delay_ms"
        case eqProfile = "eq_profile"
        case gainDb = "gain_db"
        case phaseInvert = "phase_invert"
        case telemetry
        case wifiSignalDbm = "wifi_signal_dbm"
    }
}

struct Telemetry: Decodable {
    let bufferDepth: Int?
    let packetsReceived: Int?
    let packetsLost: Int?
    let packetLossPct: Double?
    let underflows: Int?
    let droppedOverflow: Int?
    let latencyMs: Double?

    enum CodingKeys: String, CodingKey {
        case bufferDepth = "buffer_depth"
        case packetsReceived = "packets_received"
        case packetsLost = "packets_lost"
        case packetLossPct = "packet_loss_pct"
        case underflows
        case droppedOverflow = "dropped_overflow"
        case latencyMs = "latency_ms"
    }

    /// Dropouts the listener would actually hear, as opposed to packets merely lost in transit.
    var xruns: Int { (underflows ?? 0) + (droppedOverflow ?? 0) }
}

struct LevelChannel: Decodable {
    let rmsDbLeft: Double
    let rmsDbRight: Double
    let barLeft: Double
    let barRight: Double
    let clipping: Bool
    let active: Bool

    enum CodingKeys: String, CodingKey {
        case rmsDbLeft = "rms_db_left", rmsDbRight = "rms_db_right"
        case barLeft = "bar_left", barRight = "bar_right"
        case clipping, active
    }
}

struct Levels: Decodable {
    let input: LevelChannel
    let output: LevelChannel
    let state: String
    let packetsSent: Int
    let targets: Int

    enum CodingKeys: String, CodingKey {
        case input, output, state
        case packetsSent = "packets_sent"
        case targets
    }

    static let idle = Levels(
        input: LevelChannel(rmsDbLeft: -60, rmsDbRight: -60, barLeft: 0, barRight: 0,
                            clipping: false, active: false),
        output: LevelChannel(rmsDbLeft: -60, rmsDbRight: -60, barLeft: 0, barRight: 0,
                             clipping: false, active: false),
        state: "idle", packetsSent: 0, targets: 0)
}

struct AudioDevice: Identifiable, Decodable {
    let id: String
    let name: String
    let loopback: Bool
}

/// A Nexus speaker found by sweeping the LAN — not yet necessarily in the registry.
struct DiscoveredDevice: Identifiable, Decodable {
    let ip: String
    let deviceId: String
    let state: String
    let softwareVersion: String?
    let paired: Bool
    let setupMode: Bool

    var id: String { deviceId }

    enum CodingKeys: String, CodingKey {
        case ip
        case deviceId = "device_id"
        case state
        case softwareVersion = "software_version"
        case paired
        case setupMode = "setup_mode"
    }
}

struct Zone: Identifiable, Decodable {
    let zoneId: String
    let name: String
    let members: [String]
    var id: String { zoneId }

    enum CodingKeys: String, CodingKey {
        case zoneId = "zone_id", name, members
    }
}

struct DspConfig: Decodable {
    let bypass: Bool
    let inputGainDb: Double
    let masterVolumeDb: Double
    let limiterEnabled: Bool
    let eqGainsDb: [Double]

    enum CodingKeys: String, CodingKey {
        case bypass
        case inputGainDb = "input_gain_db"
        case masterVolumeDb = "master_volume_db"
        case limiterEnabled = "limiter_enabled"
        case eqGainsDb = "eq_gains_db"
    }

    static let flat = DspConfig(bypass: false, inputGainDb: 0, masterVolumeDb: 0,
                                limiterEnabled: true, eqGainsDb: Array(repeating: 0, count: 32))
}

/// One microphone's reading for one speaker.
struct MicResult: Decodable {
    let mic: Int
    let valid: Bool
    let distanceM: Double?
    let lagMs: Double?
    let confidence: Double?
    let note: String?

    enum CodingKeys: String, CodingKey {
        case mic, valid
        case distanceM = "distance_m"
        case lagMs = "lag_ms"
        case confidence, note
    }
}

struct MeasurementReply: Decodable {
    let ok: Bool
    let message: String?
    let mics: [MicResult]
    /// False when the build cannot clock-lock capture to playback. The distances are then offset by
    /// an unknown scheduling gap, so they must not be fed into geometry.
    let synchronized: Bool?
    let limitation: String?
}

// MARK: - Client

/// The 32 ISO centre frequencies the speaker's equaliser uses. Fixed by the firmware, so the UI
/// labels come from here rather than being invented.
let kEqFrequencies: [Double] = [
    30, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500, 630, 800, 1000,
    1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 14000,
    16000, 18000, 19000, 20000,
]

enum APIError: LocalizedError {
    case badStatus(Int)
    case rejected(String)

    var errorDescription: String? {
        switch self {
        case .badStatus(let code): return "השרת החזיר שגיאה (\(code))"
        case .rejected(let msg): return msg
        }
    }
}

/// Talks to the streamer over loopback. The bearer token is read from the streamer's own config, so
/// the app never has to be told a secret that already exists on disk.
actor StreamerAPI {
    private let base: URL
    private let session: URLSession
    private var token: String?

    init(port: Int = kPort) {
        base = URL(string: "http://127.0.0.1:\(port)")!
        let cfg = URLSessionConfiguration.ephemeral
        // Short timeouts: this is loopback, and a hung request would freeze a UI poll.
        cfg.timeoutIntervalForRequest = 10
        session = URLSession(configuration: cfg)
    }

    /// The streamer generates its API token on first run and persists it. Reading it from there
    /// keeps the app working across token rotation with no configuration.
    private func loadToken() -> String? {
        if let t = token { return t }
        let path = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".nexus-streamer/config.json")
        guard let data = try? Data(contentsOf: path),
              let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let web = root["web"] as? [String: Any],
              let t = web["auth_token"] as? String, !t.isEmpty else { return nil }
        token = t
        return t
    }

    private func request(_ path: String, body: [String: Any]?) throws -> URLRequest {
        var req = URLRequest(url: base.appendingPathComponent(path))
        if let t = loadToken() { req.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization") }
        if let body {
            req.httpMethod = "POST"
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            req.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        return req
    }

    private func send<T: Decodable>(_ path: String, body: [String: Any]? = nil,
                                    as type: T.Type) async throws -> T {
        let (data, response) = try await session.data(for: request(path, body: body))
        if let http = response as? HTTPURLResponse, !(200...299).contains(http.statusCode) {
            throw APIError.badStatus(http.statusCode)
        }
        return try JSONDecoder().decode(T.self, from: data)
    }

    @discardableResult
    private func sendRaw(_ path: String, body: [String: Any]? = nil) async throws -> [String: Any] {
        let (data, response) = try await session.data(for: request(path, body: body))
        if let http = response as? HTTPURLResponse, !(200...299).contains(http.statusCode) {
            throw APIError.badStatus(http.statusCode)
        }
        return (try? JSONSerialization.jsonObject(with: data) as? [String: Any]) ?? [:]
    }

    // ── reads ──

    func speakers() async throws -> [Speaker] {
        struct Wrap: Decodable { let speakers: [Speaker] }
        return try await send("/api/speakers", as: Wrap.self).speakers
    }

    func levels() async throws -> Levels {
        try await send("/api/levels", as: Levels.self)
    }

    func sources() async throws -> [AudioDevice] {
        struct Wrap: Decodable { let devices: [AudioDevice]? }
        return try await send("/api/sources", as: Wrap.self).devices ?? []
    }

    func zones() async throws -> [Zone] {
        struct Wrap: Decodable { let zones: [Zone] }
        return try await send("/api/zones", as: Wrap.self).zones
    }

    func dsp() async throws -> DspConfig {
        try await send("/api/dsp", as: DspConfig.self)
    }

    // ── writes ──

    /// Applies a partial DSP change. The streamer merges it onto the config in effect and returns
    /// what was actually applied (clamped), so the UI settles on the engine's truth rather than on
    /// what it asked for.
    @discardableResult
    func setDsp(_ patch: [String: Any]) async throws -> DspConfig {
        let (data, _) = try await session.data(for: request("/api/dsp", body: patch))
        return try JSONDecoder().decode(DspConfig.self, from: data)
    }

    /// Speaker commands return the CONFIRMED state after the speaker replied. A command the speaker
    /// rejects is surfaced as an error rather than being shown as applied.
    /// Sends a command and returns the CONFIRMED state the speaker reported in the same reply.
    ///
    /// The streamer already round-trips to the speaker and includes its answer (measured at ~130 ms
    /// on this LAN), so returning it lets the UI settle on the speaker's truth immediately instead
    /// of waiting for the next poll. Discarding it — which this used to do — made a change appear
    /// to take up to 7 s (2 s app poll + 5 s streamer poll) even though it had already applied.
    @discardableResult
    private func speakerCommand(_ path: String, _ body: [String: Any]) async throws -> Confirmed? {
        let reply = try await sendRaw(path, body: body)
        if let ok = reply["ok"] as? Bool, !ok {
            throw APIError.rejected(reply["message"] as? String ?? "הרמקול דחה את הפקודה")
        }
        guard let confirmed = reply["confirmed"] as? [String: Any],
              let data = try? JSONSerialization.data(withJSONObject: confirmed) else { return nil }
        return try? JSONDecoder().decode(Confirmed.self, from: data)
    }

    func setVolume(_ id: String, _ volume: Int) async throws -> Confirmed? {
        try await speakerCommand("/api/volume", ["speaker": id, "volume": volume])
    }
    func setMute(_ id: String, _ muted: Bool) async throws -> Confirmed? {
        try await speakerCommand("/api/mute", ["speaker": id, "muted": muted])
    }
    func setGain(_ id: String, _ db: Double) async throws -> Confirmed? {
        try await speakerCommand("/api/gain", ["speaker": id, "gain_db": db])
    }
    func setPhase(_ id: String, _ inverted: Bool) async throws -> Confirmed? {
        try await speakerCommand("/api/phase", ["speaker": id, "phase_invert": inverted])
    }
    func setDelay(_ id: String, _ ms: Int) async throws -> Confirmed? {
        try await speakerCommand("/api/delay", ["speaker": id, "delay_ms": ms])
    }
    func transport(_ id: String, _ action: String) async throws {
        _ = try await speakerCommand("/api/transport", ["speaker": id, "action": action])
    }

    // MARK: - Diagnostics

    /// Runs room calibration on the speaker. Distinct from `measure`: measurement reports the raw
    /// acoustic distance, calibration derives and applies the correction.
    func calibrate(_ id: String, durationSeconds: Double? = nil) async throws -> Confirmed? {
        var body: [String: Any] = ["speaker": id]
        if let d = durationSeconds { body["duration_s"] = d }
        return try await speakerCommand("/api/calibrate", body)
    }
    func selfTest(_ id: String) async throws -> Confirmed? {
        try await speakerCommand("/api/self-test", ["speaker": id])
    }
    /// Plays a test tone. Omitted parameters are left to the speaker, which owns the defaults.
    func audioTest(_ id: String, frequency: Int? = nil,
                   durationSeconds: Double? = nil) async throws -> Confirmed? {
        var body: [String: Any] = ["speaker": id]
        if let f = frequency { body["frequency"] = f }
        if let d = durationSeconds { body["duration_s"] = d }
        return try await speakerCommand("/api/audio-test", body)
    }

    // MARK: - System (destructive)
    //
    // Each of these carries `confirm: true`, which the streamer requires before it will forward the
    // command. Callers are expected to have asked the user first — the flag documents intent over
    // the wire, it is not a substitute for a prompt in the UI.

    func reboot(_ id: String) async throws {
        _ = try await speakerCommand("/api/reboot", ["speaker": id, "confirm": true])
    }
    func resetNetwork(_ id: String) async throws {
        _ = try await speakerCommand("/api/reset-network", ["speaker": id, "confirm": true])
    }
    func updateSoftware(_ id: String, url: String? = nil, version: String? = nil) async throws {
        var body: [String: Any] = ["speaker": id, "confirm": true]
        if let u = url { body["url"] = u }
        if let v = version { body["version"] = v }
        _ = try await speakerCommand("/api/update", body)
    }
    /// Erases the speaker's pairing and settings. The speaker will stop answering this streamer and
    /// must be provisioned again, so the extra acknowledgement is required by the streamer and
    /// spelled out here rather than hidden behind a default.
    func factoryReset(_ id: String) async throws {
        _ = try await speakerCommand("/api/factory-reset",
                                     ["speaker": id, "confirm": true,
                                      "acknowledge_unpair": "yes"])
    }

    func addSpeaker(id: String, name: String, host: String) async throws {
        try await sendRaw("/api/speakers", body: ["device_id": id, "name": name, "host": host])
    }
    func removeSpeaker(_ id: String) async throws {
        try await sendRaw("/api/speakers/remove", body: ["device_id": id])
    }

    /// Sweeps the LAN for Nexus speakers. Blocking and slow by nature (it probes every host in the
    /// /24), so the caller must not run it on a timer.
    func scanNetwork(range: String = "") async throws -> [DiscoveredDevice] {
        var req = URLRequest(url: base.appendingPathComponent("/api/scan-network"))
        if let t = loadToken() { req.setValue("Bearer \(t)", forHTTPHeaderField: "Authorization") }
        // A sweep of 254 hosts takes far longer than the default request timeout.
        req.timeoutInterval = 60
        let (data, response) = try await session.data(for: req)
        if let http = response as? HTTPURLResponse, !(200...299).contains(http.statusCode) {
            throw APIError.badStatus(http.statusCode)
        }
        struct Wrap: Decodable { let speakers: [DiscoveredDevice] }
        return (try? JSONDecoder().decode(Wrap.self, from: data))?.speakers ?? []
    }

    /// Finds (or creates) a zone whose members are exactly `members`.
    ///
    /// Reuses an existing zone when the membership already matches, so repeatedly broadcasting to
    /// the same selection does not litter the config with near-identical zones.
    func zoneMatching(members: Set<String>) async throws -> String {
        let existing = try await zones()
        if let match = existing.first(where: { Set($0.members) == members }) {
            return match.zoneId
        }
        guard let zoneId = try await createZone(name: "בחירה (\(members.count))") else {
            throw APIError.rejected("לא ניתן ליצור אזור")
        }
        for m in members {
            // A speaker may belong to only one zone, so an existing membership has to be cleared
            // before it can join this one.
            try? await removeFromAnyZone(m, zones: existing)
            try await addZoneMember(zoneId: zoneId, deviceId: m)
        }
        return zoneId
    }

    private func removeFromAnyZone(_ deviceId: String, zones: [Zone]) async throws {
        for z in zones where z.members.contains(deviceId) {
            _ = try? await sendRaw("/api/zones/members",
                                   body: ["zone_id": z.zoneId, "device_id": deviceId,
                                          "action": "remove"])
        }
    }

    /// Repoint the running stream at a zone's current members WITHOUT restarting the source.
    func retarget(zoneId: String) async throws {
        let reply = try await sendRaw("/api/zones/retarget", body: ["zone_id": zoneId])
        if let ok = reply["ok"] as? Bool, !ok {
            throw APIError.rejected(reply["message"] as? String ?? "לא ניתן לעדכן יעדים")
        }
    }

    func play(zoneId: String, kind: String, uri: String) async throws {
        let reply = try await sendRaw("/api/zones/play",
                                      body: ["zone_id": zoneId, "kind": kind, "uri": uri])
        if let ok = reply["ok"] as? Bool, !ok {
            throw APIError.rejected(reply["message"] as? String ?? "לא ניתן להתחיל שידור")
        }
    }

    func createZone(name: String) async throws -> String? {
        let reply = try await sendRaw("/api/zones", body: ["name": name])
        return reply["zone_id"] as? String
    }
    func addZoneMember(zoneId: String, deviceId: String) async throws {
        try await sendRaw("/api/zones/members",
                          body: ["zone_id": zoneId, "device_id": deviceId, "action": "add"])
    }

    /// Asks one speaker to chirp and report what its microphones heard. Slow by nature (a chirp plus
    /// its capture), so callers should not block the UI on it.
    func measure(_ id: String, hardwareRtlMs: Double, networkRtlMs: Double,
                 durationS: Double = 1.0) async throws -> MeasurementReply {
        let reply = try await sendRaw("/api/measure", body: [
            "speaker": id,
            "hardware_rtl_ms": hardwareRtlMs,
            "network_rtl_ms": networkRtlMs,
            "duration_s": durationS,
        ])
        guard let data = reply["data"] as? [String: Any] else {
            throw APIError.rejected(reply["message"] as? String ?? "המדידה נכשלה")
        }
        let json = try JSONSerialization.data(withJSONObject: data)
        return try JSONDecoder().decode(MeasurementReply.self, from: json)
    }
}
