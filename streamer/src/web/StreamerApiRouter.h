#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "control/CommandClient.h"
#include "discovery/IStreamerDiscovery.h"
#include "group/SpeakerRegistry.h"
#include "group/ZoneManager.h"
#include "pairing/PairingClient.h"
#include "sources/SourceFactory.h"
#include "state/SpeakerStateStore.h"
#include "web/Authentication.h"
#include "web/IWebTransport.h"

namespace nexus::streamer::web {

// Pure request router for the streamer's control API. Like the speaker's ApiRouter, it is a
// (method, path, body) → HttpResponse function with no socket, so it is fully unit-testable. It
// turns UI actions into signed commands sent to the target speaker via an injected sender, and
// serves the self-contained control UI at "/".
//
// Routes (all speaker-scoped commands take {"speaker": "<device_id>"} in the JSON body):
//   GET  /                       → control UI (HTML)
//   GET  /api/speakers           → registry list [{device_id,name,host,online}]
//   POST /api/speakers           → add/update a speaker {device_id,name,host[,control_port]}
//   POST /api/speakers/remove    → {device_id}
//   POST /api/status             → {speaker} → live GET_STATUS from that speaker
//   POST /api/volume             → {speaker, volume 0-100}
//   POST /api/mute               → {speaker, muted bool}
//   POST /api/eq                 → {speaker, eq_profile}
//   POST /api/delay              → {speaker, delay_ms}
//   POST /api/transport          → {speaker, action: play|pause|stop}  (START/RESUME/PAUSE/STOP_AUDIO)
//   POST /api/pair               → pair a new speaker and add it to the registry:
//        {host, setup_code, box_public_key, wifi_ssid, wifi_psk[, name, site_id]}
class StreamerApiRouter {
 public:
  // Sends one command to a speaker and returns the parsed reply. Injected so tests can drive the
  // real CommandClient over a loopback transport, and production wires a TCP transport.
  using CommandSender = std::function<nexus::core::Result<nexus::streamer::control::CommandReply>(
      const group::Speaker& target, const std::string& command, const nlohmann::json& payload)>;

  // Runs the pairing handshake with a speaker at `host` using the given params. Injected for the
  // same reason as CommandSender. Null means pairing is unavailable (returns 501).
  using PairingSender = std::function<nexus::core::Result<nexus::streamer::pairing::PairingReply>(
      const std::string& host, const nexus::streamer::pairing::PairingParams& params)>;

  // Browses the LAN for speakers in setup mode (mDNS). Null means discovery is unavailable (501).
  using Discoverer = std::function<
      nexus::core::Result<std::vector<nexus::streamer::discovery::DiscoveredSpeaker>>()>;

  // Installer "Scan Network": sweep the whole subnet over HTTP for nexus speakers. Returns a JSON
  // array [{ip,device_id,state,paired,setup_mode,software_version}]. Null means unavailable (501).
  using NetworkScanner = std::function<nlohmann::json()>;
  // Scans the streamer's own Wi-Fi radio for speaker setup APs — the only way to see a speaker that
  // has never held credentials, since mDNS and the subnet sweep both need it to be on the network
  // already. Returns the AP list as JSON.
  using ApScanner = std::function<nlohmann::json()>;
  // Runs the full onboarding of one not-yet-networked speaker: join its setup AP, pair over it
  // (handing across the Wi-Fi credentials), then leave the AP. Returns {ok,message[,device_id]}.
  using ApOnboarder = std::function<nlohmann::json(const nlohmann::json& body)>;
  // Resolves an IPv4 address to a MAC for display. Injected so the router stays a pure function
  // with no dependency on the host's ARP table.
  using MacLookup = std::function<std::string(const std::string& ip)>;

  // Drives the streamer's own audio engine for a transport action ("play"|"pause"|"resume"|"stop")
  // aimed at `target`. This is the seam that makes the play button actually emit audio: before it
  // existed the router only sent a START_AUDIO *command*, and a process serving the UI never sent a
  // single packet. Injected as a std::function so the router stays a pure request→response function
  // with no dependency on the audio module (audio must not depend on the interface, or vice versa).
  // Null means local audio is unavailable and only the command is sent.
  using TransportControl =
      std::function<core::Status(const std::string& action, const group::Speaker& target)>;

  // Forwards one HTTP call to a speaker's own web API and returns (status, body).
  //
  // The signed control channel deliberately covers only the parameters a speaker persists —
  // volume, mute, delay, EQ. Everything else it "accepts" is deferred: calibration, self-test,
  // audio test, reboot and factory reset ack with ok:true and then do nothing. Those features DO
  // work over the speaker's HTTP API, so full control means proxying that API rather than sending
  // a command that will be silently dropped.
  using SpeakerHttp = std::function<std::pair<int, std::string>(
      const group::Speaker& target, const std::string& method, const std::string& path,
      const std::string& body)>;

  StreamerApiRouter(group::SpeakerRegistry& registry, CommandSender sender,
                    PairingSender pairing_sender = nullptr, Discoverer discoverer = nullptr,
                    NetworkScanner scanner = nullptr, TransportControl transport = nullptr,
                    const state::SpeakerStateStore* store = nullptr,
                    const nexus::web::Authentication* auth = nullptr,
                    ApScanner ap_scanner = nullptr, ApOnboarder ap_onboarder = nullptr)
      : registry_(registry),
        sender_(std::move(sender)),
        pairing_sender_(std::move(pairing_sender)),
        discoverer_(std::move(discoverer)),
        scanner_(std::move(scanner)),
        transport_(std::move(transport)),
        store_(store),
        auth_(auth),
        ap_scanner_(std::move(ap_scanner)),
        ap_onboarder_(std::move(ap_onboarder)) {}

  // Starts a zone playing a source. Injected so the router stays a pure function with no dependency
  // on the audio module.
  using PlayZone = std::function<core::Status(const std::string& zone_id,
                                              const sources::SourceSpec& spec)>;
  // Called after any user-initiated change to the registry or zones, to write them to disk. Not
  // called for state/online updates — those are deliberately ephemeral and must come from the
  // speaker on every run.
  using Persist = std::function<void()>;

  // Master DSP access, as two plain JSON seams. Deliberately NOT a MasterDsp* — keeping the router
  // free of the dsp module is what lets it stay a pure request→response function and be tested
  // without an audio chain at all.
  using DspGet = std::function<nlohmann::json()>;
  // Returns the config actually in effect after applying (clamped), so the UI shows what the engine
  // will really do rather than echoing back what was requested.
  using DspSet = std::function<nlohmann::json(const nlohmann::json&)>;

  void setSpeakerHttp(SpeakerHttp fn) { speaker_http_ = std::move(fn); }
  void setZones(group::ZoneManager* zones) { zones_ = zones; }
  void setPlayZone(PlayZone fn) { play_zone_ = std::move(fn); }

  /// Repoint the running stream at a zone's current members without restarting the source.
  using RetargetZone = std::function<core::Status(const std::string& zone_id)>;
  void setRetargetZone(RetargetZone fn) { retarget_zone_ = std::move(fn); }
  void setPersist(Persist fn) { persist_ = std::move(fn); }
  void setDspAccess(DspGet get, DspSet set) {
    dsp_get_ = std::move(get);
    dsp_set_ = std::move(set);
  }

  // Live signal levels (input/output meters + transport state). Polled frequently by the UI, so it
  // must stay cheap and must never block the audio thread.
  using LevelsGet = std::function<nlohmann::json()>;
  void setLevels(LevelsGet fn) { levels_ = std::move(fn); }
  // Optional: annotate the speaker list with each speaker's MAC. Display only — see macForIp().
  void setMacLookup(MacLookup fn) { mac_lookup_ = std::move(fn); }

  nexus::web::HttpResponse route(const nexus::web::HttpRequest& req) const;

 private:
  // `host` is the request's Host header. /api/control-qr encodes the URL the phone should open, and
  // the only address known to actually reach this streamer is the one the caller just used — the
  // bind address may be 0.0.0.0, and a machine with several interfaces has no single "own IP".
  nexus::web::HttpResponse handleGet(const std::string& path, const std::string& host) const;
  nexus::web::HttpResponse handlePost(const nexus::web::HttpRequest& req) const;

  // Resolve {"speaker": id} from the body, run `command`, and shape the reply into an HTTP response.
  nexus::web::HttpResponse sendToSpeaker(const nlohmann::json& body, const std::string& command,
                                         const nlohmann::json& payload) const;

  group::SpeakerRegistry& registry_;
  CommandSender sender_;
  PairingSender pairing_sender_;
  Discoverer discoverer_;
  NetworkScanner scanner_;
  TransportControl transport_;
  // Read-only on purpose: the router reports confirmed state but must never write it. Only the
  // gateway (from a reply) and the monitor (from a poll) may mutate the store.
  const state::SpeakerStateStore* store_;
  // Null disables auth entirely (tests, and the pre-token first run). When set, every API route
  // requires the bearer token — including GETs, because the speaker list leaks the site's topology
  // and /api/pair carries a Wi-Fi PSK.
  const nexus::web::Authentication* auth_;
  // Wi-Fi onboarding of speakers that are not on the network yet. Null on hosts with no spare
  // radio, where those two routes report "not available" rather than pretending to work.
  ApScanner ap_scanner_;
  ApOnboarder ap_onboarder_;
  MacLookup mac_lookup_;
  group::ZoneManager* zones_ = nullptr;
  SpeakerHttp speaker_http_;
  PlayZone play_zone_;
  RetargetZone retarget_zone_;
  Persist persist_;
  DspGet dsp_get_;
  DspSet dsp_set_;
  LevelsGet levels_;
};

}  // namespace nexus::streamer::web
