#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "logging/Logger.h"
#include "logging/Redaction.h"

using namespace nexus::logging;

TEST(Redaction, MasksMac) {
  EXPECT_EQ(maskMac("aa:bb:cc:dd:ee:ff"), "**:**:**:**:**:ff");
  EXPECT_EQ(maskMac("garbage"), "**:**:**:**:**:**");
}

TEST(Redaction, MasksToken) {
  EXPECT_EQ(maskToken("abcd1234"), "ab****34");
  EXPECT_EQ(maskToken("ab"), "****");
}

TEST(Logger, LevelRoundTrip) {
  EXPECT_EQ(levelFromString("debug"), Level::Debug);
  EXPECT_EQ(levelFromString("warning"), Level::Warning);
  EXPECT_EQ(levelFromString("nonsense"), Level::Info);  // default
  EXPECT_STREQ(toString(Level::Critical), "CRITICAL");
}

TEST(Logger, WritesToFileAndRedactsSecrets) {
  auto dir = std::filesystem::temp_directory_path() / "nexus_log_test";
  std::filesystem::remove_all(dir);
  auto file = dir / "speaker.log";

  LogConfig cfg;
  cfg.file_path = file.string();
  cfg.to_stderr = false;
  cfg.level = Level::Debug;

  Logger& log = Logger::instance();
  ASSERT_TRUE(log.init(cfg).ok());

  nexus::core::SecretString secret("wifi-password-1234");
  log.log(Level::Info, "test", std::string("connecting with ") + Logger::redact(secret));
  log.logCommand(Level::Error, "control", "cmd-42", "rejected",
                 nexus::core::ErrorCode::InvalidArg);
  log.flush();

  std::ifstream in(file);
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string contents = buf.str();

  EXPECT_NE(contents.find("***"), std::string::npos);
  EXPECT_EQ(contents.find("wifi-password-1234"), std::string::npos);  // secret never leaked
  EXPECT_NE(contents.find("cmd=cmd-42"), std::string::npos);          // command_id present
  EXPECT_NE(contents.find("code=1001"), std::string::npos);           // error code present

  log.shutdown();
  std::filesystem::remove_all(dir);
}
