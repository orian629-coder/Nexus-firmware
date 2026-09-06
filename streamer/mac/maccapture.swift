// maccapture — capture a macOS audio input device and write raw s16le/48k/stereo PCM to stdout.
//
// This is the live-source half of "play what my Mac is playing": route system audio to a virtual
// device (BlackHole) and point this at it, then pipe into the streamer:
//
//     maccapture "BlackHole 2ch" | nexus-streamer --stream <speaker-ip>
//
// Why a 60-line Swift file instead of ffmpeg: the Mac had no ffmpeg, sox, or rec, and asking a user
// to install Homebrew before they can hear music is a poor first run. AVFoundation ships with the
// OS, so this works on a clean machine. It also lets us emit exactly the wire format (48 kHz,
// stereo, interleaved little-endian int16) with no conversion in between.
//
// `--list` prints one JSON object per line describing each capture device, which is how the
// streamer populates its source picker. Line-delimited JSON (not one array) so the C++ side can
// parse it with the existing per-line reader and a malformed line can be skipped rather than
// discarding the whole enumeration.
//
// Build: swiftc -O maccapture.swift -o maccapture

import AVFoundation
import Foundation

let wireRate = 48000.0
let wireChannels: AVAudioChannelCount = 2

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
    exit(1)
}

// JSON string escaping. Device names are user-visible and can contain quotes or backslashes, which
// would otherwise produce output the streamer cannot parse.
func jsonEscape(_ s: String) -> String {
    var out = ""
    for c in s.unicodeScalars {
        switch c {
        case "\"": out += "\\\""
        case "\\": out += "\\\\"
        case "\n": out += "\\n"
        case "\r": out += "\\r"
        case "\t": out += "\\t"
        default:
            if c.value < 0x20 {
                out += String(format: "\\u%04x", c.value)
            } else {
                out.unicodeScalars.append(c)
            }
        }
    }
    return out
}

let args = CommandLine.arguments
let listMode = args.contains("--list")
let wanted = args.count > 1 && !listMode ? args[1] : "BlackHole 2ch"

// Pick the input device by name so the caller can choose a virtual device (BlackHole) rather than
// the built-in microphone.
let discovery = AVCaptureDevice.DiscoverySession(
    deviceTypes: [.microphone, .external],
    mediaType: .audio,
    position: .unspecified)

if listMode {
    for d in discovery.devices {
        // uid is the stable identifier; localizedName is what the user recognizes. Both are
        // reported so the UI can show a name while the config stores something that survives a
        // device being renamed.
        let line = "{\"name\":\"\(jsonEscape(d.localizedName))\","
            + "\"uid\":\"\(jsonEscape(d.uniqueID))\"}"
        print(line)
    }
    exit(0)
}

// Resolve by uniqueID FIRST, then by exact name, then by substring. The streamer passes the UID it
// got from --list, because a display name changes when the user renames a device in Audio MIDI
// Setup while the UID does not. Name matching stays for hand-typed use from the command line.
guard let device = discovery.devices.first(where: { $0.uniqueID == wanted })
        ?? discovery.devices.first(where: { $0.localizedName == wanted })
        ?? discovery.devices.first(where: { $0.localizedName.contains(wanted) }) else {
    let names = discovery.devices
        .map { "  - \($0.localizedName)  [\($0.uniqueID)]" }
        .joined(separator: "\n")
    fail("No audio input matching '\(wanted)'. Available:\n\(names)")
}

let engine = AVAudioEngine()
let session = AVCaptureSession()
guard let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) else {
    fail("Cannot open '\(device.localizedName)' for capture")
}
session.addInput(input)

let output = AVCaptureAudioDataOutput()
// The wire format is fixed, so ask CoreAudio to deliver it directly rather than converting later.
output.audioSettings = [
    AVFormatIDKey: kAudioFormatLinearPCM,
    AVSampleRateKey: wireRate,
    AVNumberOfChannelsKey: wireChannels,
    AVLinearPCMBitDepthKey: 16,
    AVLinearPCMIsFloatKey: false,
    AVLinearPCMIsBigEndianKey: false,
    AVLinearPCMIsNonInterleaved: false,
]

final class Writer: NSObject, AVCaptureAudioDataOutputSampleBufferDelegate {
    private let out = FileHandle.standardOutput
    func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer,
                       from c: AVCaptureConnection) {
        guard let block = CMSampleBufferGetDataBuffer(sb) else { return }
        var length = 0
        var ptr: UnsafeMutablePointer<Int8>?
        guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil,
                                          totalLengthOut: &length, dataPointerOut: &ptr) == noErr,
              let p = ptr, length > 0 else { return }
        // A closed pipe (the streamer exited) surfaces as SIGPIPE/EPIPE — exit quietly rather than
        // spewing errors, so Ctrl-C on the pipeline looks clean.
        out.write(Data(bytes: p, count: length))
    }
}

let writer = Writer()
output.setSampleBufferDelegate(writer, queue: DispatchQueue(label: "nexus.capture"))
guard session.canAddOutput(output) else { fail("Cannot add audio output") }
session.addOutput(output)

signal(SIGPIPE, SIG_IGN)  // handled by the write failing, not by dying mid-buffer
FileHandle.standardError.write("capturing '\(device.localizedName)' → stdout (48k/2ch/s16le)\n"
                                 .data(using: .utf8)!)
session.startRunning()
RunLoop.main.run()
_ = engine  // keep AVFoundation linked for the audio session on older SDKs
