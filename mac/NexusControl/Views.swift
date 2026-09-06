// Native SwiftUI interface for the Nexus streamer.
//
// Layout follows the AOA controller the project descends from: source and signal at the top, a
// speaker table in the middle (one row per speaker with its own gain / mute / phase / delay), and
// the measurement tool below. Right-to-left throughout, since the operator-facing language is
// Hebrew.

import AppKit
import SwiftUI

// MARK: - Palette
// Matches the streamer's own web UI so the two never look like different products.

/// The design system: warm neutral greys with a single gold accent.
///
/// Deliberately NOT the old blue-tinted palette. Greys here are neutral (equal R/G/B) so the gold
/// is the only saturated colour on screen — which is what makes an active control unmistakable in
/// a dense grid, where a second accent would compete with it.
///
/// Green/amber/red survive only as STATE colours (online, warning, fault). They never appear as
/// decoration, so seeing one always means something.
enum Nexus {
    static let bg = Color(hex: 0x2B2B2B)          // window
    static let bar = Color(hex: 0x1F1F1F)         // app bar, darkest
    static let surface = Color(hex: 0x333333)     // panels
    static let row = Color(hex: 0x3D3D3D)         // list rows
    static let rowAlt = Color(hex: 0x454545)      // hovered / selected row
    static let field = Color(hex: 0x3A3A3A)       // inputs

    static let accent = Color(hex: 0xC9A961)      // gold
    static let accentDim = Color(hex: 0x8A7440)   // gold at rest (inactive ring)

    static let text = Color(hex: 0xE8E8E8)
    static let muted = Color(hex: 0x9A9A9A)
    static let faint = Color(hex: 0x6E6E6E)
    static let border = Color.white.opacity(0.10)

    // State only — never decoration.
    static let ok = Color(hex: 0x6FCF7F)
    static let warn = Color(hex: 0xE0B341)
    static let alert = Color(hex: 0xE06C6C)

    static let cardRadius: CGFloat = 6
    static let controlRadius: CGFloat = 4
}

extension Color {
    init(hex: UInt32) {
        self.init(.sRGB,
                  red: Double((hex >> 16) & 0xFF) / 255,
                  green: Double((hex >> 8) & 0xFF) / 255,
                  blue: Double(hex & 0xFF) / 255,
                  opacity: 1)
    }
}

// MARK: - Design system components

/// The app bar: hamburger, title, and gold text actions on the right.
struct AppBar<Actions: View>: View {
    let title: String
    var onMenu: (() -> Void)? = nil
    @ViewBuilder var actions: Actions

    var body: some View {
        // ZStack rather than a three-part HStack: the logo is centred on the WINDOW, so it stays
        // put as the title text and the action buttons change width between screens. In an HStack
        // it would drift with them.
        ZStack {
            if let path = Bundle.main.path(forResource: "nexus-logo", ofType: "png"),
               let img = NSImage(contentsOfFile: path) {
                Image(nsImage: img).resizable().scaledToFit().frame(height: 52)
            }
            HStack(spacing: 14) {
                Text(title).font(.system(size: 16, weight: .medium)).foregroundColor(Nexus.text)
                Spacer()
                actions
            }
        }
        .padding(.horizontal, 18)
        // Grown with the logo: the wordmark would otherwise sit hard against the bar's edges,
        // which reads as a cropped image rather than a deliberate one.
        .frame(height: 84)
        .background(Nexus.bar)
    }
}

/// A gold text action, as in the bar's "ADD DEVICE" / "SAVE CONFIGURATION".
struct BarAction: View {
    let title: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title.uppercased())
                .font(.system(size: 11, weight: .semibold))
                .tracking(0.8)
        }
        .buttonStyle(.plain)
        .foregroundColor(Nexus.accent)
    }
}

/// Tabs underlined in gold. The underline spans the tab, not just the label, so the active region
/// is unambiguous when two labels differ a lot in width.
struct TabStrip<T: Hashable>: View {
    let tabs: [(T, String)]
    @Binding var selection: T

    var body: some View {
        HStack(spacing: 0) {
            ForEach(tabs, id: \.0) { value, title in
                Button { selection = value } label: {
                    VStack(spacing: 0) {
                        Text(title.uppercased())
                            .font(.system(size: 12, weight: selection == value ? .semibold : .regular))
                            .tracking(0.6)
                            .foregroundColor(selection == value ? Nexus.accent : Nexus.muted)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                        Rectangle()
                            .fill(selection == value ? Nexus.accent : Color.clear)
                            .frame(height: 2)
                    }
                }
                .buttonStyle(.plain)
            }
        }
        .background(Nexus.bg)
    }
}

/// Full-width search field with a leading magnifier and a trailing submit arrow.
struct SearchField: View {
    let placeholder: String
    @Binding var text: String
    var onSubmit: (() -> Void)? = nil

    var body: some View {
        HStack(spacing: 10) {
            TextField(placeholder, text: $text)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .foregroundColor(Nexus.text)
                .onSubmit { onSubmit?() }
        }
        .padding(.horizontal, 14)
        .frame(height: 42)
        .background(Nexus.field)
        .clipShape(RoundedRectangle(cornerRadius: Nexus.controlRadius))
    }
}

/// The outlined name chip that opens each row — an editable-looking label that marks the device's
/// own name apart from the type and address columns beside it.
struct NameChip: View {
    let text: String
    var width: CGFloat = 120

    var body: some View {
        Text(text)
            .font(.system(size: 12))
            .foregroundColor(Nexus.text)
            .lineLimit(1)
            .frame(width: width, height: 26)
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(Nexus.muted.opacity(0.55)))
    }
}

/// A labelled toggle from the settings grid: the outline turns gold when the function is active.
///
/// Word rather than glyph. A speaker icon with a slash and one without are easy to confuse at a
/// glance, and "מושתק" vs "פעיל" cannot be — which matters most for exactly the control whose
/// state you check before wondering why a room is silent.
struct LabelToggle: View {
    let title: String
    let active: Bool
    var enabled: Bool = true
    var width: CGFloat = 78
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(title)
                .font(.system(size: 11, weight: .medium))
                .foregroundColor(active ? Nexus.accent : Nexus.muted)
                .frame(width: width, height: 28)
                .overlay(RoundedRectangle(cornerRadius: Nexus.controlRadius)
                            .stroke(active ? Nexus.accent : Nexus.border,
                                    lineWidth: active ? 1.5 : 1))
        }
        .buttonStyle(.plain)
        .opacity(enabled ? 1 : 0.4)
        .disabled(!enabled)
    }
}

/// The speaker's status pill: a tinted block whose colour carries the meaning, rather than plain
/// grey text that has to be read to be understood.
struct StatusPill: View {
    enum Kind { case ok, error, info }
    let kind: Kind
    let text: String

    private var fg: Color {
        switch kind {
        case .ok: return Nexus.ok
        case .error: return Nexus.alert
        case .info: return Nexus.accent
        }
    }

    var body: some View {
        Text(text)
            .font(.system(size: 12, weight: .semibold))
            .padding(.vertical, 8).padding(.horizontal, 12)
            .frame(maxWidth: .infinity)
            .background(fg.opacity(0.12))
            .foregroundColor(fg)
            .clipShape(RoundedRectangle(cornerRadius: Nexus.controlRadius))
    }
}

/// The speaker's connection lamp: a dot with a soft ring, whose colour is the whole message.
struct Lamp: View {
    enum State { case idle, on, bad }
    let state: State

    private var color: Color {
        switch state {
        case .idle: return Nexus.muted
        case .on: return Nexus.ok
        case .bad: return Nexus.alert
        }
    }

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 12, height: 12)
            .overlay(Circle().stroke(color.opacity(0.20), lineWidth: 3))
    }
}

/// Buttons in this system are quiet: an outline by default, gold only when the action is the
/// primary one on screen. Solid fills are reserved for genuine state (a muted speaker), so a
/// screen full of buttons does not read as a screen full of alarms.
struct NexusButtonStyle: ButtonStyle {
    var ghost = false
    var tint: Color? = nil

    func makeBody(configuration: Configuration) -> some View {
        let solid = tint
        return configuration.label
            .font(.system(size: 12, weight: .medium))
            .padding(.vertical, 8).padding(.horizontal, 14)
            .background(solid ?? Color.clear)
            // White on a filled button, never black: these fills are mid-tone greys and golds on a
            // dark window, and black text on them is the one combination that stops being legible.
            .foregroundColor(solid != nil ? Nexus.text : (ghost ? Nexus.muted : Nexus.accent))
            .overlay(RoundedRectangle(cornerRadius: Nexus.controlRadius)
                        .stroke(solid != nil ? Color.clear
                                             : (ghost ? Nexus.border : Nexus.accent.opacity(0.6))))
            .clipShape(RoundedRectangle(cornerRadius: Nexus.controlRadius))
            .opacity(configuration.isPressed ? 0.7 : 1)
    }
}

/// A device row: a flat panel with a subtle lift when selected. Rows sit on the window background
/// with a small gap, the way the reference list does — no outline per row, so a long list stays
/// calm.
struct NexusRow<Content: View>: View {
    var selected = false
    @ViewBuilder var content: Content

    var body: some View {
        content
            .padding(.vertical, 10).padding(.horizontal, 14)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(selected ? Nexus.rowAlt : Nexus.row)
            .overlay(alignment: .leading) {
                // Selection reads as a gold edge rather than a full border: it marks the row
                // without boxing every item on screen.
                Rectangle()
                    .fill(selected ? Nexus.accent : Color.clear)
                    .frame(width: 3)
            }
            .clipShape(RoundedRectangle(cornerRadius: Nexus.cardRadius))
    }
}

/// Card with a titled header rule — the shape every section uses.
struct Card<Content: View>: View {
    let title: String
    var subtitle: String? = nil
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(alignment: .firstTextBaseline) {
                Text(title)
                    .font(.system(size: 12, weight: .semibold))
                    .tracking(0.6)
                    .foregroundColor(Nexus.accent)
                Spacer()
                if let subtitle {
                    Text(subtitle).font(.system(size: 11)).foregroundColor(Nexus.muted)
                }
            }
            .padding(.bottom, 10)
            Divider().background(Nexus.border)
            VStack(alignment: .leading, spacing: 12) { content }
                .padding(.top, 12)
        }
        .padding(16)
        .background(Nexus.surface)
        .overlay(RoundedRectangle(cornerRadius: 14).stroke(Nexus.border))
        .clipShape(RoundedRectangle(cornerRadius: 14))
    }
}

// MARK: - VU meter

/// One channel of the input/output meter. The bar fraction is computed by the streamer so the app
/// and the C++ engine cannot disagree about where the bottom of the scale sits.
struct MeterBar: View {
    let label: String
    let fraction: Double
    let db: Double
    let clipping: Bool

    var body: some View {
        HStack(spacing: 8) {
            Text(label).font(.system(size: 11)).foregroundColor(Nexus.muted)
                .frame(width: 56, alignment: .trailing)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 4).fill(Color.black.opacity(0.35))
                    RoundedRectangle(cornerRadius: 4)
                        .fill(LinearGradient(colors: [Nexus.ok, Nexus.ok, Nexus.warn, Nexus.alert],
                                             startPoint: .leading, endPoint: .trailing))
                        .frame(width: max(0, geo.size.width * fraction))
                        .animation(.linear(duration: 0.08), value: fraction)
                }
            }
            .frame(height: 9)
            Text(db <= -60 ? "−∞" : String(format: "%.1f", db))
                .font(.system(size: 11, design: .monospaced))
                .foregroundColor(clipping ? Nexus.alert : Nexus.muted)
                .frame(width: 46, alignment: .leading)
        }
    }
}

// MARK: - Discovery (first screen)

/// The screen the app opens on: find speakers, tick the ones to use, broadcast.
///
/// Only Nexus speakers are listed. A full ARP sweep would fill the table with the TV, the router
/// and every phone on the network — rows that would sit at "Disconnected" forever with controls
/// that can never do anything.
struct DiscoveryView: View {
    @EnvironmentObject var state: AppState

    /// SAVED shows speakers already in the registry; DISCOVERY shows what the last sweep found.
    /// Two lists rather than one with mixed states: "the speakers I have" and "what is on the
    /// network" are different questions, and answering both in one list is what made every row
    /// need a "found / added / offline" qualifier.
    enum Mode: Hashable { case saved, discovery }
    @State private var mode: Mode = .saved
    @State private var search = ""

    var body: some View {
        VStack(spacing: 0) {
            TabStrip(tabs: [(Mode.saved, "שמורים"), (Mode.discovery, "גילוי")],
                     selection: $mode)

            SearchField(placeholder: "חפש לפי שם, כתובת IP או מזהה", text: $search)
                .padding(.horizontal, 16).padding(.vertical, 12)

            if mode == .discovery { scanBar }

            deviceList
            actionBar
        }
    }

    /// Only shown on DISCOVERY: the range and the sweep are meaningless for saved devices.
    private var scanBar: some View {
        HStack(spacing: 10) {
            Text("טווח רשת").font(.system(size: 12)).foregroundColor(Nexus.muted)
            TextField("192.168.1.0/24", text: $state.networkRange)
                .textFieldStyle(.plain)
                .font(.system(size: 12, design: .monospaced))
                .foregroundColor(Nexus.text)
                .environment(\.layoutDirection, .leftToRight)
                .padding(.vertical, 7).padding(.horizontal, 10)
                .background(Nexus.field)
                .clipShape(RoundedRectangle(cornerRadius: Nexus.controlRadius))
                .frame(width: 180)

            Button { state.scanNetwork() } label: {
                HStack(spacing: 6) {
                    if state.scanning { ProgressView().controlSize(.small) }
                    Text(state.scanning ? "סורק…" : "סרוק רשת")
                }
            }
            .buttonStyle(NexusButtonStyle())
            .disabled(state.scanning)

            if !state.scanMessage.isEmpty {
                Text(state.scanMessage).font(.system(size: 12)).foregroundColor(Nexus.muted)
            }
            Spacer()
        }
        .padding(.horizontal, 16).padding(.bottom, 12)
    }

    /// What the current tab is listing, after the search filter.
    private var rows: [DiscoveredDevice] {
        let base: [DiscoveredDevice]
        switch mode {
        case .discovery:
            base = state.discovered
        case .saved:
            // Saved speakers rendered through the same row type, so both tabs look identical.
            base = state.speakers.map {
                DiscoveredDevice(ip: $0.host, deviceId: $0.deviceId, state: "",
                                 softwareVersion: nil, paired: true, setupMode: false)
            }
        }
        guard !search.isEmpty else { return base }
        let q = search.lowercased()
        return base.filter { device in
            if device.deviceId.lowercased().contains(q) || device.ip.contains(q) { return true }
            // Also match the friendly name, which is what the operator actually reads in the list.
            let name = state.speakers.first { $0.deviceId == device.deviceId }?.name ?? ""
            return name.lowercased().contains(q)
        }
    }

    private var deviceList: some View {
        Group {
            if rows.isEmpty {
                VStack(spacing: 10) {
                    Text(emptyMessage).font(.system(size: 13)).foregroundColor(Nexus.muted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    VStack(spacing: 8) {
                        ForEach(rows) { d in
                            DiscoveryRow(device: d)
                        }
                    }
                    .padding(.horizontal, 16)
                    .padding(.bottom, 12)
                }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var emptyMessage: String {
        if !search.isEmpty { return "אין תוצאות לחיפוש" }
        switch mode {
        case .saved: return "אין רמקולים שמורים — עבור ל\"גילוי\" כדי למצוא"
        case .discovery: return state.scanning ? "סורק את הרשת…" : "לחץ \"סרוק רשת\" כדי למצוא רמקולים"
        }
    }

    /// Fixed at the bottom, like the reference: source on one line, then the actions.
    private var actionBar: some View {
        VStack(spacing: 10) {
            HStack(spacing: 10) {
                Text("מקור אודיו").font(.system(size: 12)).foregroundColor(Nexus.muted)
                Picker("", selection: Binding(
                    get: { state.selectedDeviceId ?? "" },
                    set: { state.selectedDeviceId = $0 })) {
                    ForEach(state.devices) { d in
                        Text(d.loopback ? d.name + "  (מערכת)" : d.name).tag(d.id)
                    }
                }
                .labelsHidden()
                .frame(width: 260)
                Spacer()
            }

            HStack(spacing: 8) {
                // One toggle, not a start button: with selection applied live, the only remaining
                // question is whether audio is flowing at all.
                if state.levels.state == "playing" {
                    Button("עצור שידור") { state.stopBroadcast() }
                        .buttonStyle(NexusButtonStyle(tint: Nexus.accent))
                } else {
                    Button("נגן") { state.broadcastToSelection() }
                        .buttonStyle(NexusButtonStyle())
                        .disabled(state.selectedForBroadcast.isEmpty)
                }

                Button("בחר הכל") { state.selectAllDiscovered() }
                    .buttonStyle(NexusButtonStyle(ghost: true))
                Button("נקה בחירה") { state.deselectAllDiscovered() }
                    .buttonStyle(NexusButtonStyle(ghost: true))

                Spacer()
                Text(state.levels.state == "playing"
                     ? "משדר ל-\(state.levels.targets)"
                     : "\(state.selectedForBroadcast.count)/\(state.discovered.count) נבחרו")
                    .font(.system(size: 11)).foregroundColor(Nexus.muted)
            }
        }
        .padding(16)
    }
}

/// Column widths shared by the header and every row. They have to come from one place: headers and
/// cells that each carry their own numbers drift apart the moment either is edited, and a table
/// whose labels do not sit over their columns is worse than one with no labels at all.
enum DiscoveryLayout {
    static let gap: CGFloat = 8
    static let select: CGFloat = 26
    static let name: CGFloat = 180
    static let mute: CGFloat = 96
    static let volume: CGFloat = 150
    static let edit: CGFloat = 72
    static let status: CGFloat = 72
}

struct DiscoveryTable: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        VStack(spacing: 8) {
            ForEach(state.discovered) { d in
                DiscoveryRow(device: d)
            }
        }
    }
}

/// Uses the speaker's own row treatment: an inset panel that takes an accent border when picked,
/// the same affordance the speaker's Wi-Fi list uses for a selected network.
struct DiscoveryRow: View {
    let device: DiscoveredDevice
    @EnvironmentObject var state: AppState

    /// Already added to the registry — i.e. controllable, not merely visible.
    private var isRegistered: Bool {
        state.speakers.contains { $0.deviceId == device.deviceId }
    }

    private var isOnline: Bool {
        state.speakers.first { $0.deviceId == device.deviceId }?.online ?? false
    }

    private var picked: Bool { state.selectedForBroadcast.contains(device.deviceId) }

    /// The registry entry, when this device has been added. Everything controllable comes from
    /// here — a device that is merely visible on the network has no state to control.
    private var speaker: Speaker? {
        state.speakers.first { $0.deviceId == device.deviceId }
    }

    var body: some View {
        NexusRow(selected: picked) {
            HStack(spacing: 14) {
                // Selection dot, not a checkbox: the row already carries buttons, and a tick box
                // among them reads as one more control rather than as the row's own state.
                Circle()
                    .fill(picked ? Nexus.accent : Nexus.faint)
                    .frame(width: 8, height: 8)

                // The device's own name, in the outlined chip the reference uses to set it apart
                // from the read-only columns beside it.
                NameChip(text: speaker?.name ?? device.deviceId)

                Text(device.deviceId)
                    .font(.system(size: 12)).foregroundColor(Nexus.text)
                    .frame(width: 120, alignment: .leading).lineLimit(1)

                Text(device.ip)
                    .font(.system(size: 12, design: .monospaced))
                    .foregroundColor(Nexus.muted)
                    .environment(\.layoutDirection, .leftToRight)
                    .frame(width: 110, alignment: .leading)

                if let s = speaker {
                    // Mute renders the speaker's CONFIRMED state, so the colour is the speaker's
                    // answer rather than the last thing that was clicked.
                    LabelToggle(title: s.muted == true ? "מושתק" : "פעיל",
                                active: s.muted == true, enabled: isOnline) {
                        state.setSpeakerMute(s.deviceId, !(s.muted ?? false))
                    }

                    HStack(spacing: 8) {
                        Text("\(s.volume ?? 0)")
                            .font(.system(size: 12, design: .monospaced))
                            .frame(width: 26, alignment: .trailing)
                            .foregroundColor(s.volume == nil ? Nexus.faint : Nexus.text)
                        RowVolumeSlider(value: Double(s.volume ?? 0)) {
                            state.setSpeakerVolume(s.deviceId, Int($0.rounded()))
                        }
                        .disabled(!isOnline)
                    }
                    .frame(width: 150)

                    Spacer(minLength: 0)

                    Text(isOnline ? "מחובר" : "לא מגיב")
                        .font(.system(size: 11))
                        .foregroundColor(isOnline ? Nexus.ok : Nexus.alert)

                    // Into the device's own screen. A word, so the destination is stated rather
                    // than implied by an arrow's direction.
                    Button("עריכה") { state.screen = .edit(s.deviceId) }
                        .buttonStyle(NexusButtonStyle(ghost: true))
                } else {
                    Spacer(minLength: 0)
                    if device.setupMode {
                        Text("setup").font(.system(size: 11, weight: .semibold))
                            .foregroundColor(Nexus.warn)
                    }
                    // Not in the registry: there is no state to control, so the only real action
                    // is adding it. Rendering dead volume/mute controls would imply otherwise.
                    Button("הוסף") { state.addDiscovered(device) }
                        .buttonStyle(NexusButtonStyle())
                }
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { if isRegistered { state.toggleSelection(device.deviceId) } }
    }
}

/// Volume slider that follows the drag and commits once on release — the same rule every other
/// slider here follows, so a drag does not queue a signed command per pixel of travel.
struct RowVolumeSlider: View {
    let value: Double
    let onCommit: (Double) -> Void
    @State private var live: Double?

    var body: some View {
        Slider(value: Binding(get: { live ?? value }, set: { live = $0 }),
               in: 0...100,
               onEditingChanged: { editing in
                   if !editing, let v = live { onCommit(v); live = nil }
               })
        .frame(minWidth: 90)
    }
}

/// Everything for ONE speaker: its own control settings and its own measurement.
///
/// Edit opens this because the discovery list answers "which speakers, and are they playing", while
/// the work of setting a speaker up — trim, delay, phase, and measuring where it actually is —
/// belongs to that speaker alone. Reaching it from the row keeps the question "which speaker am I
/// adjusting?" from ever arising, which a separate global tab could not.
struct SpeakerEditScreen: View {
    let speaker: Speaker
    @EnvironmentObject var state: AppState

    private var measurement: MeasurementReply? { state.measurements[speaker.deviceId] }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            header
            Divider().background(Nexus.border)

            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    controlSection
                    Divider().background(Nexus.border)
                    measureSection
                    Divider().background(Nexus.border)
                    // Master processing lives here too. It is global — it applies to every speaker
                    // at once — but it is part of the same job as tuning this one, and putting it
                    // behind a separate tab meant leaving the speaker you were adjusting to reach
                    // the EQ that shapes what it plays.
                    masterSection
                    Divider().background(Nexus.border)
                    TelemetryLine(speaker: speaker)
                    Button("הסר רמקול") { state.removeSpeaker(speaker.deviceId) }
                        .buttonStyle(NexusButtonStyle(ghost: true))
                }
                .padding(16)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    }

    private var header: some View {
        HStack(spacing: 10) {
            Lamp(state: speaker.online ? .on : .bad)
            VStack(alignment: .leading, spacing: 1) {
                Text(speaker.name).font(.system(size: 15, weight: .bold))
                Text(speaker.host)
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundColor(Nexus.muted)
                    .environment(\.layoutDirection, .leftToRight)
            }
            Spacer()
            Text(speaker.online ? "מחובר" : "לא מגיב")
                .font(.system(size: 12))
                .foregroundColor(speaker.online ? Nexus.ok : Nexus.alert)
        }
        .padding(16)
        .background(Nexus.surface)
    }

    private var masterSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text("עיבוד מרכזי")
                    .font(.system(size: 12, weight: .bold)).foregroundColor(Nexus.accent)
                Text("משפיע על כל הרמקולים")
                    .font(.system(size: 10)).foregroundColor(Nexus.muted)
            }

            LabeledSlider(label: "עוצמת מאסטר", value: state.dsp.masterVolumeDb,
                          range: -40...12, unit: "dB") { state.setMasterVolume($0) }
            LabeledSlider(label: "הגבר כניסה", value: state.dsp.inputGainDb,
                          range: -20...20, unit: "dB") { state.setInputGain($0) }

            HStack(spacing: 8) {
                Button(state.dsp.bypass ? "Bypass: פעיל" : "Bypass: כבוי") { state.toggleBypass() }
                    .buttonStyle(NexusButtonStyle(ghost: true))
                Button("איפוס EQ") { state.flattenEq() }
                    .buttonStyle(NexusButtonStyle(ghost: true))
                Spacer()
            }

            Text("אקולייזר 32 פסים").font(.system(size: 11)).foregroundColor(Nexus.muted)
            EqualizerView()
        }
    }

    // ── control ──

    private var controlSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("שליטה")
                .font(.system(size: 12, weight: .bold)).foregroundColor(Nexus.accent)

            LabeledSlider(label: "עוצמה", value: Double(speaker.volume ?? 0),
                          range: 0...100, unit: "%") {
                state.setSpeakerVolume(speaker.deviceId, Int($0.rounded()))
            }

            LabeledSlider(label: "כוונון (trim)", value: speaker.gainDb ?? 0,
                          range: -20...20, unit: "dB") {
                state.setSpeakerGain(speaker.deviceId, $0)
            }

            HStack {
                Text("השהיה").font(.system(size: 11)).foregroundColor(Nexus.muted)
                Stepper("\(speaker.delayMs ?? 0) ms", value: Binding(
                    get: { speaker.delayMs ?? 0 },
                    set: { new in
                        if new != (speaker.delayMs ?? 0) {
                            state.setSpeakerDelay(speaker.deviceId, new)
                        }
                    }), in: 0...500, step: 5)
                .font(.system(size: 11))
                Spacer()
            }

            Toggle("היפוך פאזה", isOn: Binding(
                get: { speaker.phaseInvert ?? false },
                set: { new in
                    if new != (speaker.phaseInvert ?? false) {
                        state.setSpeakerPhase(speaker.deviceId, new)
                    }
                }))
                .font(.system(size: 12))
                .toggleStyle(.switch)

            HStack(spacing: 8) {
                Button("נגן") { state.transport(speaker.deviceId, "play") }
                    .buttonStyle(NexusButtonStyle(ghost: true))
                Button("עצור") { state.transport(speaker.deviceId, "stop") }
                    .buttonStyle(NexusButtonStyle(ghost: true))
                Spacer()
            }
        }
    }

    // ── measurement ──

    private var measureSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("מדידת מרחק")
                .font(.system(size: 12, weight: .bold)).foregroundColor(Nexus.accent)
            Text("הרמקול משמיע chirp ומודד מה שהמיקרופונים שלו קלטו.")
                .font(.system(size: 11)).foregroundColor(Nexus.muted)

            HStack(spacing: 10) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("RTL חומרה (ms)").font(.system(size: 10)).foregroundColor(Nexus.muted)
                    TextField("0", value: $state.hardwareRtlMs, format: .number)
                        .textFieldStyle(.roundedBorder)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text("RTL רשת (ms)").font(.system(size: 10)).foregroundColor(Nexus.muted)
                    TextField("0", value: $state.networkRtlMs, format: .number)
                        .textFieldStyle(.roundedBorder)
                }
            }
            // 1 ms is 34 cm, so a wrong budget is not a rounding error.
            Text("שני הערכים נדרשים. שגיאה של 1ms = 34 ס\"מ.")
                .font(.system(size: 10)).foregroundColor(Nexus.muted)

            Button(state.measuring ? "מודד…" : "מדוד רמקול זה") {
                state.measure(speaker.deviceId)
            }
            .buttonStyle(NexusButtonStyle())
            .disabled(state.measuring || !speaker.online)

            if let r = measurement {
                MeasurementRow(name: speaker.name, reply: r)
            }
        }
    }
}

// MARK: - Source & signal

struct LabeledSlider: View {
    let label: String
    let value: Double
    let range: ClosedRange<Double>
    let unit: String
    let onChange: (Double) -> Void

    @State private var live: Double?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(label).font(.system(size: 11)).foregroundColor(Nexus.muted)
                Spacer()
                Text(String(format: "%.1f %@", live ?? value, unit))
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                    .foregroundColor(Nexus.accent)
            }
            Slider(value: Binding(get: { live ?? value }, set: { live = $0 }),
                   in: range,
                   // Commit only when the drag ends: sending on every frame would flood the
                   // streamer and fight the poll for control of the slider.
                   onEditingChanged: { editing in
                       if !editing, let v = live { onChange(v); live = nil }
                   })
        }
    }
}

/// Inline slider that follows the finger while dragging and commits once, on release.
///
/// While a drag is in progress the local value wins; the moment it ends the value is sent and the
/// local override is dropped, so the control settles on whatever the speaker actually confirmed.
struct LiveSlider: View {
    let label: String
    let value: Double
    let range: ClosedRange<Double>
    let unit: String
    var width: CGFloat = 140
    let onCommit: (Double) -> Void

    @State private var live: Double?

    var body: some View {
        HStack(spacing: 6) {
            Text(String(format: "%@ %.1f %@", label, live ?? value, unit))
                .font(.system(size: 11)).foregroundColor(Nexus.muted)
                .frame(width: 96, alignment: .trailing)
            Slider(value: Binding(get: { live ?? value }, set: { live = $0 }),
                   in: range,
                   onEditingChanged: { editing in
                       if !editing, let v = live { onCommit(v); live = nil }
                   })
            .frame(width: width)
        }
    }
}

struct EqualizerView: View {
    @EnvironmentObject var state: AppState

    var body: some View {
        ScrollView(.horizontal, showsIndicators: true) {
            HStack(alignment: .bottom, spacing: 4) {
                ForEach(Array(kEqFrequencies.enumerated()), id: \.offset) { idx, freq in
                    VStack(spacing: 3) {
                        EqBandSlider(
                            value: idx < state.dsp.eqGainsDb.count ? state.dsp.eqGainsDb[idx] : 0
                        ) { state.setEqBand(idx, $0) }
                        Text(freq >= 1000 ? "\(Int(freq / 1000))k" : "\(Int(freq))")
                            .font(.system(size: 8)).foregroundColor(Nexus.muted)
                    }
                }
            }
            .padding(.vertical, 4)
        }
        .frame(height: 130)
    }
}

struct EqBandSlider: View {
    let value: Double
    let onChange: (Double) -> Void
    @State private var live: Double?

    var body: some View {
        // A rotated slider keeps its ORIGINAL bounds for hit testing — rotationEffect is a drawing
        // transform, not a layout one. The previous `.frame(width: 100).frame(width: 18, ...)`
        // therefore clipped the interactive area to an 18pt-wide box while the track was drawn
        // 100pt tall, so most of the band was simply not touchable and the EQ looked dead.
        //
        // Fixing it needs the rotation applied INSIDE a fixed-size container, and the hit area
        // restored explicitly with contentShape.
        VStack {
            Slider(value: Binding(get: { live ?? value }, set: { live = $0 }),
                   in: -10...10,
                   onEditingChanged: { editing in
                       if !editing, let v = live { onChange(v); live = nil }
                   })
                .frame(width: 100)
                .rotationEffect(.degrees(-90))
        }
        .frame(width: 24, height: 100)
        .contentShape(Rectangle())
    }
}

// MARK: - Speakers

/// Playback health. A speaker that reported nothing says so — showing zeros would draw a perfectly
/// healthy stream for a speaker that may not be playing at all.
struct TelemetryLine: View {
    let speaker: Speaker

    var body: some View {
        HStack(spacing: 10) {
            if let t = speaker.telemetry {
                if let loss = t.packetLossPct {
                    Text(String(format: "אובדן %.2f%%", loss))
                        .foregroundColor(loss >= 1 ? Nexus.alert : (loss > 0 ? Nexus.warn : Nexus.ok))
                }
                if let b = t.bufferDepth { Text("buffer \(b)").foregroundColor(Nexus.muted) }
                if let l = t.latencyMs, l > 0 {
                    Text(String(format: "השהיה %.1fms", l)).foregroundColor(Nexus.muted)
                }
                if t.xruns > 0 { Text("XRUN \(t.xruns)").foregroundColor(Nexus.alert) }
            } else {
                Text("אין נתוני ניגון").foregroundColor(Nexus.muted).italic()
            }
            if let rssi = speaker.wifiSignalDbm {
                Text("\(rssi)dBm").foregroundColor(Nexus.muted)
            }
        }
        .font(.system(size: 10))
    }
}

// MARK: - Measurement

struct MeasurementRow: View {
    let name: String
    let reply: MeasurementReply

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(name).font(.system(size: 12, weight: .semibold))
            if !reply.ok {
                Text(reply.message ?? "המדידה נכשלה")
                    .font(.system(size: 11)).foregroundColor(Nexus.alert)
            }
            ForEach(reply.mics, id: \.mic) { m in
                HStack(spacing: 10) {
                    Text("Mic\(m.mic)").font(.system(size: 11)).foregroundColor(Nexus.muted)
                    if m.valid, let d = m.distanceM {
                        Text(String(format: "%.2f m", d))
                            .font(.system(size: 11, design: .monospaced))
                        if let c = m.confidence {
                            // Under 2× the winning peak barely beat an unrelated one, which is the
                            // signature of a room reflection rather than the direct arrival.
                            Text(String(format: "%.1f×", c))
                                .font(.system(size: 10))
                                .foregroundColor(c >= 2 ? Nexus.ok : Nexus.warn)
                        }
                    } else {
                        Text(m.note ?? "לא נמדד")
                            .font(.system(size: 10)).foregroundColor(Nexus.muted).italic()
                    }
                }
            }
            if reply.synchronized == false, let limitation = reply.limitation {
                Text(limitation)
                    .font(.system(size: 10)).foregroundColor(Nexus.warn)
            }
        }
        .padding(.vertical, 4)
    }
}
