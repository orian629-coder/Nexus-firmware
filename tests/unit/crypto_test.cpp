#include <gtest/gtest.h>

#include <sodium.h>

#include "identity/Crypto.h"

using namespace nexus::identity::crypto;

TEST(Crypto, Base64RoundTrip) {
  std::vector<std::uint8_t> data{0, 1, 2, 250, 255, 42};
  auto b64 = toBase64(data);
  auto back = fromBase64(b64);
  ASSERT_TRUE(back.ok());
  EXPECT_EQ(back.value(), data);
}

TEST(Crypto, Ed25519SignRoundTripsWithVerify) {
  ASSERT_GE(sodium_init(), 0);
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  ASSERT_EQ(crypto_sign_keypair(pk, sk), 0);
  std::vector<std::uint8_t> skv(sk, sk + sizeof(sk));
  std::vector<std::uint8_t> pkv(pk, pk + sizeof(pk));

  std::vector<std::uint8_t> msg{'s', 'i', 'g', 'n', ' ', 'm', 'e'};
  auto sig = signEd25519(msg, skv);
  ASSERT_TRUE(sig.ok());
  EXPECT_EQ(sig.value().size(), std::size_t(crypto_sign_BYTES));
  EXPECT_TRUE(verifyEd25519(msg, sig.value(), pkv));

  // Wrong-size secret key is rejected.
  std::vector<std::uint8_t> shortSk(31, 0);
  EXPECT_FALSE(signEd25519(msg, shortSk).ok());
}

TEST(Crypto, Ed25519SignBase64MatchesVerifyBase64) {
  // The Streamer signs the RAW canonical bytes and base64s the signature; the speaker's
  // verifyEd25519Base64 base64-decodes the message before verifying (toBase64/fromBase64 cancel).
  // So signEd25519Base64(canonical) must verify against verifyEd25519Base64(toBase64(canonical)).
  ASSERT_GE(sodium_init(), 0);
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  ASSERT_EQ(crypto_sign_keypair(pk, sk), 0);
  const std::string sk_b64 = toBase64(std::vector<std::uint8_t>(sk, sk + sizeof(sk)));
  const std::string pk_b64 = toBase64(std::vector<std::uint8_t>(pk, pk + sizeof(pk)));

  const std::string canonical = "cmd-1\nSPK-A104\nSET_VOLUME\n{\"volume\":70}\n100\n50";
  auto sig_b64 = signEd25519Base64(canonical, sk_b64);
  ASSERT_TRUE(sig_b64.ok());
  EXPECT_TRUE(verifyEd25519Base64(toBase64(std::vector<std::uint8_t>(canonical.begin(),
                                                                     canonical.end())),
                                  sig_b64.value(), pk_b64));
}

TEST(Crypto, Ed25519VerifyAcceptsGoodSignatureAndRejectsTampered) {
  ASSERT_GE(sodium_init(), 0);
  unsigned char pk[crypto_sign_PUBLICKEYBYTES];
  unsigned char sk[crypto_sign_SECRETKEYBYTES];
  ASSERT_EQ(crypto_sign_keypair(pk, sk), 0);

  std::vector<std::uint8_t> msg{'h', 'e', 'l', 'l', 'o'};
  std::vector<std::uint8_t> sig(crypto_sign_BYTES);
  ASSERT_EQ(crypto_sign_detached(sig.data(), nullptr, msg.data(), msg.size(), sk), 0);

  std::vector<std::uint8_t> pkv(pk, pk + sizeof(pk));
  EXPECT_TRUE(verifyEd25519(msg, sig, pkv));

  // Tamper with the message.
  auto bad = msg;
  bad[0] = 'H';
  EXPECT_FALSE(verifyEd25519(bad, sig, pkv));

  // Tamper with the signature.
  auto badsig = sig;
  badsig[0] ^= 0x01;
  EXPECT_FALSE(verifyEd25519(msg, badsig, pkv));
}

TEST(Crypto, SealedBoxRoundTrip) {
  auto kp = generateX25519KeyPair();
  ASSERT_TRUE(kp.ok());

  std::vector<std::uint8_t> secret{'w', 'i', 'f', 'i', ':', 'p', 'a', 's', 's'};
  auto sealed = sealTo(kp.value().public_key, secret);
  ASSERT_TRUE(sealed.ok());
  // Ciphertext must not contain the plaintext.
  EXPECT_NE(sealed.value(), secret);

  auto opened = sealOpen(kp.value(), sealed.value());
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value(), secret);
}

TEST(Crypto, SealedBoxRejectsWrongKey) {
  auto kp1 = generateX25519KeyPair();
  auto kp2 = generateX25519KeyPair();
  ASSERT_TRUE(kp1.ok());
  ASSERT_TRUE(kp2.ok());

  std::vector<std::uint8_t> secret{'x'};
  auto sealed = sealTo(kp1.value().public_key, secret);
  ASSERT_TRUE(sealed.ok());

  // Opening with the wrong keypair must fail.
  auto opened = sealOpen(kp2.value(), sealed.value());
  EXPECT_FALSE(opened.ok());
}
