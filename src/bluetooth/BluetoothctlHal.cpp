#include "bluetooth/BluetoothctlHal.h"

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <csignal>
#include <cstdio>
#include <string>

#include "logging/Logger.h"

// Real bluetoothctl bridge. Ported from the pre-redesign speaker-app; the process/agent handling
// is unchanged, adapted to return core::Status/Result and log via NX_LOG.

namespace nexus::bluetooth {

using core::ErrorCode;
using core::Result;
using core::Status;

namespace {
// Minimal shell-quote for a single argument (the alias). Wrap in single quotes, escape embedded
// single quotes. Mirrors NmcliNetworkHal::shq.
std::string shq(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}
}  // namespace

BluetoothctlHal::~BluetoothctlHal() { stopAgent(); }

BluetoothctlHal::CmdResult BluetoothctlHal::btctl(const std::string& args) {
  std::string full = "bluetoothctl " + args + " 2>&1";
  FILE* pipe = ::popen(full.c_str(), "r");
  if (!pipe) return {127, ""};
  std::string out;
  std::array<char, 256> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) out += buf.data();
  int st = ::pclose(pipe);
  int code = (st == -1) ? 127 : WEXITSTATUS(st);
  return {code, out};
}

Status BluetoothctlHal::enableSink(const std::string& alias) {
  CmdResult list = btctl("list");
  if (list.code != 0 || list.out.find("Controller") == std::string::npos) {
    return Status::error(ErrorCode::IoError, "no Bluetooth controller");
  }

  // WirePlumber routes the incoming A2DP stream to the I2S sink automatically.
  btctl("power on");
  btctl("system-alias " + shq(alias));
  btctl("discoverable-timeout 0");  // always discoverable
  btctl("pairable on");
  btctl("discoverable on");

  startAgent();  // persistent auto-accept agent

  CmdResult show = btctl("show");
  bool ok = show.out.find("Powered: yes") != std::string::npos &&
            show.out.find("Discoverable: yes") != std::string::npos;
  if (!ok) return Status::error(ErrorCode::Unknown, "adapter state uncertain after setup");
  return Status::success();
}

Status BluetoothctlHal::openPairingWindow(int seconds) {
  btctl("discoverable-timeout " + std::to_string(seconds));
  btctl("pairable on");
  btctl("discoverable on");
  if (agent_pid_ <= 0) startAgent();  // ensure agent is up if disable() ran earlier
  return Status::success();
}

Status BluetoothctlHal::disable() {
  btctl("discoverable off");
  btctl("pairable off");
  stopAgent();
  return Status::success();
}

Result<BluetoothStatus> BluetoothctlHal::status() {
  BluetoothStatus s;
  CmdResult show = btctl("show");
  s.available = show.code == 0 && show.out.find("Controller") == std::string::npos
                    ? show.out.find("Powered:") != std::string::npos
                    : show.out.find("Powered:") != std::string::npos;
  s.discoverable = show.out.find("Discoverable: yes") != std::string::npos;

  CmdResult dev = btctl("devices Connected");
  size_t pos = dev.out.find("Device ");
  if (dev.code == 0 && pos != std::string::npos) {
    s.connected = true;
    size_t nl = dev.out.find('\n', pos);
    std::string line = dev.out.substr(pos, (nl == std::string::npos ? dev.out.size() : nl) - pos);
    // "Device " (7) + MAC (17) + " " (1) = 25 → the rest is the device name.
    if (line.size() > 25) s.device_name = line.substr(25);
  }
  return s;
}

void BluetoothctlHal::startAgent() {
  if (agent_pid_ > 0) return;  // already running

  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    NX_LOG_ERROR("bluetooth", ErrorCode::IoError, "failed to create agent pipe");
    return;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    NX_LOG_ERROR("bluetooth", ErrorCode::IoError, "agent fork failed");
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return;
  }

  if (pid == 0) {
    // Child: bluetoothctl with stdin from the pipe, output to /dev/null.
    ::dup2(pipefd[0], STDIN_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    FILE* devnull = std::fopen("/dev/null", "w");
    if (devnull) {
      ::dup2(fileno(devnull), STDOUT_FILENO);
      ::dup2(fileno(devnull), STDERR_FILENO);
    }
    ::execlp("bluetoothctl", "bluetoothctl", static_cast<char*>(nullptr));
    _exit(127);  // execlp failed
  }

  // Parent: register the agent, keep the write-end open so the session (and agent) stays alive.
  ::close(pipefd[0]);
  const std::string cmds = "agent NoInputNoOutput\ndefault-agent\n";
  ssize_t w = ::write(pipefd[1], cmds.data(), cmds.size());
  (void)w;
  agent_stdin_ = pipefd[1];
  agent_pid_ = pid;
  NX_LOG_INFO("bluetooth", "auto-accept agent registered, pid=" + std::to_string(agent_pid_));
}

void BluetoothctlHal::stopAgent() {
  if (agent_pid_ <= 0) return;
  if (agent_stdin_ >= 0) {
    ::close(agent_stdin_);  // EOF → bluetoothctl exits
    agent_stdin_ = -1;
  }
  ::kill(agent_pid_, SIGTERM);
  int st;
  ::waitpid(agent_pid_, &st, 0);
  agent_pid_ = -1;
}

}  // namespace nexus::bluetooth
