#include <gtest/gtest.h>

#include <string>

#include "web/QrEncoder.h"

using nexus::web::QrEncoder;

namespace {

// Flatten a module matrix to a row-major "0"/"1" string.
std::string flatten(const std::vector<std::vector<bool>>& m) {
  std::string s;
  for (const auto& row : m)
    for (bool b : row) s += b ? '1' : '0';
  return s;
}

// Golden vector: the exact 29x29 (version 3, ECC-M) matrix for the setup-hotspot WiFi payload.
// Captured from the encoder AFTER verifying with an independent decoder (pyzbar) that a real phone
// reads it back as the exact string. If the encoder regresses, this string changes and the test
// fails — locking the byte-for-byte-correct output.
constexpr const char* kGoldWifi =
    "111111101010010110000011111111000001000110110100110100000110111010000010010110101011101101"
    "110101100110100101010111011011101011011001100000101110110000010101010100011001000001111111"
    "101010101010101011111110000000010000011010010000000010001011100110101111111111001011001001"
    "010000111000110110001110101111001101000011001100101010001011100110111010100010111010111011"
    "111000010101100100001010101101011100100011100000001011110101110100100010101000001010111110"
    "011001101110011110101111000100111011010010110110101110001111100101010100000000101100000100"
    "001010000010000000100110010110100110000011111111001111101101111110010000000001110100111101"
    "000110101111111010101111001110101010110000010000101010101100011000101110101100010011011111"
    "101101011101000000111101011001010110111010011001111111000001111100000100011100111111101010"
    "1111111110101101111100100010110";

}  // namespace

// The encoder produces the exact, decoder-verified matrix for the WiFi setup payload.
TEST(QrEncoder, WifiPayloadMatchesGoldenMatrix) {
  auto m = QrEncoder::encode("WIFI:T:WPA;S:Nexus-Setup;P:nexussetup;;");
  ASSERT_EQ(m.size(), 29u);  // version 3
  EXPECT_EQ(flatten(m), std::string(kGoldWifi));
}

// The open setup AP is advertised as T:nopass with no P: field — a T:WPA payload would make the
// phone try to join an open network with a password and fail.
TEST(QrEncoder, OpenNetworkPayloadEncodes) {
  auto m = QrEncoder::encode("WIFI:T:nopass;S:Nexus-Setup;;");
  EXPECT_GE(m.size(), 21u);
  EXPECT_EQ(m.size() % 4, 1u);  // QR sizes are 4v+17
}

// Version auto-selection: short strings pick smaller symbols, longer strings grow.
TEST(QrEncoder, VersionScalesWithLength) {
  EXPECT_EQ(QrEncoder::encode("HI").size(), 21u);                    // v1
  EXPECT_EQ(QrEncoder::encode("http://10.42.0.1/setup").size(), 25u);  // v2
  EXPECT_GE(QrEncoder::encode(std::string(100, 'X')).size(), 33u);   // higher version
}

// Finder patterns are present at the three corners (structural sanity).
TEST(QrEncoder, HasFinderPatterns) {
  auto m = QrEncoder::encode("HI");
  const int n = static_cast<int>(m.size());
  for (auto corner : {std::pair<int, int>{0, 0}, {0, n - 7}, {n - 7, 0}}) {
    int r0 = corner.first, c0 = corner.second;
    EXPECT_TRUE(m[r0][c0]);              // outer ring dark
    EXPECT_TRUE(m[r0 + 6][c0 + 6]);
    EXPECT_FALSE(m[r0 + 1][c0 + 1]);     // inner ring light
    EXPECT_TRUE(m[r0 + 3][c0 + 3]);      // center dark
  }
}

// Too-long input is rejected (beyond version 10 at ECC-M).
TEST(QrEncoder, RejectsOversizedInput) {
  EXPECT_THROW(QrEncoder::encode(std::string(400, 'A')), std::runtime_error);
}

// SVG output is well-formed and encodes at least one dark module.
TEST(QrEncoder, SvgIsWellFormed) {
  const std::string svg = QrEncoder::toSvg("WIFI:T:WPA;S:Nexus-Setup;P:nexussetup;;");
  EXPECT_EQ(svg.rfind("<svg", 0), 0u);
  EXPECT_NE(svg.find("</svg>"), std::string::npos);
  EXPECT_NE(svg.find("fill='#000'"), std::string::npos);  // dark modules drawn
  EXPECT_NE(svg.find("width='"), std::string::npos);
}
