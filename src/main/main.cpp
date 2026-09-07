#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include "main/ApCredentialsCli.h"
#include "main/Application.h"

namespace {
nexus::Application* g_app = nullptr;

void handleSignal(int) {
  if (g_app) g_app->requestShutdown();
}

void printUsage(const char* prog) {
  std::cout << "Usage: " << prog << " [options]\n"
            << "  --config <path>    Path to config.json (default /etc/nexus-speaker/config.json)\n"
            << "  --data-dir <path>  Sandbox root for identity/secure/log (dev/testing)\n"
            << "  --ap-credentials   Print the paired streamer's AP SSID/passphrase and exit\n"
            << "  --version          Print version and exit\n"
            << "  --help             Show this help\n";
}
}  // namespace

int main(int argc, char** argv) {
  nexus::AppOptions opts;
  bool want_ap_credentials = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--config" && i + 1 < argc) {
      opts.config_path = argv[++i];
    } else if (arg == "--data-dir" && i + 1 < argc) {
      // Re-root all writable/identity paths under a sandbox — used for dev runs and testing off
      // the target, where /etc and /var are not writable.
      std::string root = argv[++i];
      opts.config_path = root + "/config.json";
      opts.identity_path = root + "/identity/factory.json";
      opts.secure_dir = root + "/secure";
      opts.log_path = root + "/speaker.log";
    } else if (arg == "--ap-credentials") {
      want_ap_credentials = true;
    } else if (arg == "--version") {
      std::cout << "nexus-speaker " << "0.1.0" << "\n";
      return 0;
    } else if (arg == "--help") {
      printUsage(argv[0]);
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      printUsage(argv[0]);
      return 2;
    }
  }

  // `--ap-credentials`: print the paired streamer's private-AP SSID/passphrase for
  // scripts/speaker-ap-join.sh, then exit without starting the app. Keeps the KDF in one place.
  if (want_ap_credentials) {
    const auto creds = nexus::app::apCredentialsFromConfig(opts.config_path);
    if (!creds) {
      std::cerr << "nexus-speaker: not paired — no streamer AP credentials\n";
      return 1;
    }
    std::cout << "NEXUS_AP_SSID=" << creds->ssid << "\n"
              << "NEXUS_AP_PASSPHRASE=" << creds->passphrase << "\n";
    return 0;
  }

  nexus::Application app(opts);
  g_app = &app;

  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  auto status = app.startup();
  if (!status.ok()) {
    std::cerr << "startup failed: " << status.message() << "\n";
    return 1;
  }

  return app.run();
}
