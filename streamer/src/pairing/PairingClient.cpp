#include "pairing/PairingClient.h"

#include <nlohmann/json.hpp>

#include "core/ErrorCodes.h"
#include "identity/Crypto.h"
#include "pairing/PairingTypes.h"

namespace nexus::streamer::pairing {

using core::ErrorCode;
using core::Result;
using core::Status;

Result<PairingReply> PairingClient::pair(const PairingParams& p) {
  // 1. Seal {ssid,psk} to the speaker's X25519 public key so only that speaker can read them.
  auto box_pk = nexus::identity::crypto::fromBase64(p.speaker_box_public_key);
  if (!box_pk.ok()) return Status::error(ErrorCode::InvalidArg, "bad speaker box_public_key");
  const nlohmann::json wifi_json = {{"ssid", p.wifi_ssid}, {"psk", p.wifi_psk}};
  const std::string wifi_str = wifi_json.dump();
  auto sealed = nexus::identity::crypto::sealTo(
      box_pk.value(), std::vector<std::uint8_t>(wifi_str.begin(), wifi_str.end()));
  if (!sealed.ok()) return sealed.status();
  const std::string sealed_b64 = nexus::identity::crypto::toBase64(sealed.value());

  // 2. Build the request and sign its canonical bytes (shared PairingRequest::canonicalString()).
  nexus::pairing::PairingRequest req;
  req.streamer_id = p.streamer_id;
  req.streamer_public_key = p.streamer_public_key;
  req.site_id = p.site_id;
  req.initial_speaker_name = p.initial_speaker_name;
  req.setup_code = p.setup_code;
  req.sealed_wifi_b64 = sealed_b64;
  auto sig = nexus::identity::crypto::signEd25519Base64(req.canonicalString(), p.streamer_secret_key);
  if (!sig.ok()) return sig.status();
  req.signature_b64 = sig.value();

  // 3. Emit JSON with the wire keys the speaker's pairing handler reads (sealed_wifi / signature).
  const nlohmann::json j = {{"type", "pairing"},
                            {"streamer_id", req.streamer_id},
                            {"streamer_public_key", req.streamer_public_key},
                            {"site_id", req.site_id},
                            {"initial_speaker_name", req.initial_speaker_name},
                            {"setup_code", req.setup_code},
                            {"sealed_wifi", req.sealed_wifi_b64},
                            {"signature", req.signature_b64}};

  auto resp = transport_.request(host_, port_, j.dump());
  if (!resp.ok()) return resp.status();

  nlohmann::json rj;
  try {
    rj = nlohmann::json::parse(resp.value());
  } catch (const std::exception& e) {
    return Status::error(ErrorCode::InvalidArg, std::string("bad pairing response: ") + e.what());
  }
  PairingReply reply;
  reply.ok = rj.value("ok", false);
  reply.message = rj.value("message", "");
  reply.device_id = rj.value("device_id", "");
  return reply;
}

}  // namespace nexus::streamer::pairing
