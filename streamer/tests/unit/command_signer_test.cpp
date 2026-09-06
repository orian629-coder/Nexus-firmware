#include <gtest/gtest.h>

#include <sodium.h>

#include "control/CommandSigner.h"
#include "control/CommandValidator.h"
#include "identity/Crypto.h"

using nexus::streamer::control::CommandSigner;
using namespace nexus::identity::crypto;

namespace {

// A fixed Ed25519 keypair derived from a constant seed, so the streamer's signer and the speaker's
// validator share a deterministic identity across runs (a real golden vector, not a random key).
struct Keypair {
  std::string sk_b64;
  std::string pk_b64;
};

Keypair fixedKeypair() {
  EXPECT_GE(sodium_init(), 0);
  unsigned char seed[crypto_sign_SEEDBYTES];
  for (unsigned int i = 0; i < sizeof(seed); ++i) seed[i] = static_cast<unsigned char>(i + 1);
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  EXPECT_EQ(crypto_sign_seed_keypair(pk, sk, seed), 0);
  return {toBase64(std::vector<std::uint8_t>(sk, sk + sizeof(sk))),
          toBase64(std::vector<std::uint8_t>(pk, pk + sizeof(pk)))};
}

}  // namespace

// The core Phase-0 contract proof: a command signed by the streamer's CommandSigner is accepted by
// the speaker's REAL CommandValidator. This locks the raw-canonical-bytes signature scheme end to
// end — if signing and verification ever diverge byte-for-byte, this fails.
TEST(CommandSigner, SignedCommandIsAcceptedBySpeakerValidator) {
  const Keypair kp = fixedKeypair();
  CommandSigner signer(kp.sk_b64);

  const std::string device_id = "SPK-A104";
  const std::int64_t now = 1785100000;
  auto cmd = signer.build("cmd-12345", device_id, "SET_VOLUME", {{"volume", 70}},
                          /*expires_at=*/now + 300, /*timestamp=*/now);
  ASSERT_TRUE(cmd.ok());

  auto st = nexus::control::CommandValidator::validate(cmd.value(), device_id, kp.pk_b64, now + 1);
  EXPECT_TRUE(st.ok()) << st.message();
}

// A tampered payload must be rejected — proves the signature actually covers the payload.
TEST(CommandSigner, TamperedPayloadIsRejected) {
  const Keypair kp = fixedKeypair();
  CommandSigner signer(kp.sk_b64);
  const std::string device_id = "SPK-A104";
  const std::int64_t now = 1785100000;

  auto cmd = signer.build("cmd-2", device_id, "SET_VOLUME", {{"volume", 70}}, now + 300, now);
  ASSERT_TRUE(cmd.ok());
  cmd.value().payload = {{"volume", 100}};  // change after signing

  auto st = nexus::control::CommandValidator::validate(cmd.value(), device_id, kp.pk_b64, now + 1);
  EXPECT_FALSE(st.ok());
}

// A command signed for one speaker must not validate against a different device_id, because
// target_id is inside the signed canonical bytes (and the validator also checks target match).
TEST(CommandSigner, WrongTargetIsRejected) {
  const Keypair kp = fixedKeypair();
  CommandSigner signer(kp.sk_b64);
  const std::int64_t now = 1785100000;

  auto cmd = signer.build("cmd-3", "SPK-A104", "SET_MUTE", {{"muted", true}}, now + 300, now);
  ASSERT_TRUE(cmd.ok());

  auto st = nexus::control::CommandValidator::validate(cmd.value(), "SPK-B999", kp.pk_b64, now + 1);
  EXPECT_FALSE(st.ok());
}
