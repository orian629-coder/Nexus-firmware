// Capture-device source layer: what the streamer can stream FROM.
//
// The parts that must hold on ANY host (including CI with no audio backend) are the pure ones:
// shell escaping, loopback classification, and the contract that a bad SourceSpec fails cleanly
// rather than spawning something. Enumeration itself is host-dependent, so it is asserted only for
// invariants that are true whether or not devices exist.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "sources/CaptureDevices.h"
#include "sources/SourceFactory.h"

using namespace nexus::streamer::sources;

// ── loopback classification ──
// Drives which device the UI recommends. A miss costs a hint, never selectability.

TEST(CaptureDevices, RecognizesKnownLoopbackNames) {
  EXPECT_TRUE(isLoopbackName("BlackHole 2ch"));
  EXPECT_TRUE(isLoopbackName("blackhole 16ch"));  // case-insensitive
  EXPECT_TRUE(isLoopbackName("Monitor of Built-in Audio"));
  EXPECT_TRUE(isLoopbackName("ALSA Loopback"));
  EXPECT_TRUE(isLoopbackName("Soundflower (2ch)"));
}

TEST(CaptureDevices, DoesNotMisclassifyRealInputs) {
  EXPECT_FALSE(isLoopbackName("MacBook Pro Microphone"));
  EXPECT_FALSE(isLoopbackName("Scarlett 2i2 USB"));
  EXPECT_FALSE(isLoopbackName(""));
}

// Non-ASCII names must pass through untouched. macOS localizes device names, so the Hebrew
// microphone name on this very machine goes through this path; a locale-dependent tolower() on
// UTF-8 bytes could corrupt them.
TEST(CaptureDevices, HandlesNonAsciiNamesWithoutCorruption) {
  const std::string hebrew = "מיקרופון ה-MacBook Pro";
  EXPECT_FALSE(isLoopbackName(hebrew));
  // A UTF-8 name that DOES contain an ASCII keyword is still matched.
  EXPECT_TRUE(isLoopbackName("BlackHole וירטואלי"));
}

// ── shell escaping ──
// Device ids and file paths reach a shell. macOS device names are user-editable, so a name with a
// quote is reachable input, not a theoretical case.

TEST(CaptureDevices, ShellQuoteWrapsPlainStrings) {
  EXPECT_EQ(shellQuote("BlackHole 2ch"), "'BlackHole 2ch'");
}

TEST(CaptureDevices, ShellQuoteNeutralizesEmbeddedSingleQuote) {
  // The classic break-out: without escaping, the quote would close the argument and `rm -rf /`
  // would run as a command.
  const std::string evil = "x'; rm -rf / #";
  const std::string quoted = shellQuote(evil);

  EXPECT_EQ(quoted, "'x'\\''; rm -rf / #'");

  // The property that actually matters is not textual: it is that a shell parses this as ONE
  // argument equal to the original string. Verified by round-tripping it through a real shell —
  // `echo` is safe, and if the escaping were broken the injected `rm` would be a separate command
  // and the echoed text would differ.
  const std::string cmd = "printf '%s' " + quoted;
  std::FILE* p = ::popen(cmd.c_str(), "r");
  ASSERT_NE(p, nullptr);
  std::string got;
  char buf[256];
  while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) got.append(buf, n);
  ::pclose(p);
  EXPECT_EQ(got, evil) << "the shell did not receive the string as a single literal argument";
}

// ── SourceSpec contract ──

TEST(CaptureDevices, DeviceSourceWithoutIdIsRejected) {
  // Must fail as a bad argument, NOT by spawning a helper with an empty device name.
  auto r = makeSource({"device", ""});
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), nexus::core::ErrorCode::InvalidArg);
}

TEST(CaptureDevices, UnknownKindIsRejected) {
  auto r = makeSource({"telepathy", "whatever"});
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status().code(), nexus::core::ErrorCode::InvalidArg);
}

TEST(CaptureDevices, DeviceKindIsAdvertised) {
  const auto kinds = availableKinds();
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), "device"), kinds.end());
  // The pre-existing kinds must not have been dropped while adding the new one.
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), "file"), kinds.end());
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), "stdin"), kinds.end());
  EXPECT_NE(std::find(kinds.begin(), kinds.end(), "process"), kinds.end());
}

// ── enumeration invariants ──
// True with or without a capture backend, so this is safe on a headless CI box.

TEST(CaptureDevices, EnumerationNeverReturnsMalformedEntries) {
  for (const auto& d : listCaptureDevices()) {
    EXPECT_FALSE(d.id.empty()) << "a device with no id cannot be selected or persisted";
    EXPECT_FALSE(d.name.empty()) << "a device with no name cannot be shown";
  }
}

// An unbuilt helper / missing backend is a normal state and must degrade to "no devices" rather
// than to a fabricated device that would fail at play time.
TEST(CaptureDevices, MissingBackendYieldsNoDevicesRatherThanGarbage) {
  ::setenv("NEXUS_MACCAPTURE", "/nonexistent/path/to/maccapture", 1);
  const auto devices = listCaptureDevices();
#ifdef __APPLE__
  EXPECT_TRUE(devices.empty());
  // And a device spec must then fail cleanly instead of spawning a missing binary.
  auto r = makeSource({"device", "BlackHole2ch_UID"});
  EXPECT_FALSE(r.ok());
#else
  (void)devices;  // on Linux the env var is irrelevant; pw-record path is unaffected
#endif
  ::unsetenv("NEXUS_MACCAPTURE");
}
