#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "bluetooth/BluetoothManager.h"
#include "bluetooth/IBluetoothHal.h"
#include "core/EventBus.h"

using namespace nexus::bluetooth;
using nexus::core::Event;
using nexus::core::EventBus;
using nexus::core::EventType;
using nexus::core::ServiceState;

TEST(BluetoothManager, StartStopLifecycle) {
  EventBus bus;
  BluetoothManager bt(&bus, "Nexus Audio", std::make_unique<StubBluetoothHal>());
  EXPECT_EQ(bt.state(), ServiceState::Stopped);
  ASSERT_TRUE(bt.start().ok());
  EXPECT_EQ(bt.state(), ServiceState::Running);
  ASSERT_TRUE(bt.stop().ok());
  EXPECT_EQ(bt.state(), ServiceState::Stopped);
}

TEST(BluetoothManager, NoAdapterDegradesGracefully) {
  EventBus bus;
  // A tiny local HAL that reports no adapter present.
  struct NoAdapterHal : IBluetoothHal {
    nexus::core::Status enableSink(const std::string&) override {
      return nexus::core::Status::success();
    }
    nexus::core::Status openPairingWindow(int) override {
      return nexus::core::Status::success();
    }
    nexus::core::Status disable() override { return nexus::core::Status::success(); }
    nexus::core::Result<BluetoothStatus> status() override { return BluetoothStatus{}; }
  };
  BluetoothManager bt(&bus, "Nexus Audio", std::make_unique<NoAdapterHal>());
  ASSERT_TRUE(bt.start().ok());  // never fails the process
  EXPECT_EQ(bt.state(), ServiceState::Degraded);
  EXPECT_TRUE(bt.healthCheck().ok());  // degraded is healthy for an optional service
}

TEST(BluetoothManager, ConnectEmitsAudioStarted) {
  EventBus bus;
  std::atomic<int> started{0};
  std::string device;
  bus.subscribe(EventType::AudioStarted, [&](const Event& e) {
    ++started;
    device = e.data.value("device", "");
  });

  auto hal = std::make_unique<StubBluetoothHal>();
  StubBluetoothHal* raw = hal.get();
  BluetoothManager bt(&bus, "Nexus Audio", std::move(hal));
  ASSERT_TRUE(bt.start().ok());

  raw->simulateConnect("Orian's Phone");
  bt.healthCheck();  // polls the HAL and publishes on change
  bus.drain();

  EXPECT_EQ(started.load(), 1);
  EXPECT_EQ(device, "Orian's Phone");
  EXPECT_TRUE(bt.isConnected());
  EXPECT_EQ(bt.connectedDeviceName(), "Orian's Phone");
}

TEST(BluetoothManager, DisconnectEmitsAudioStopped) {
  EventBus bus;
  std::atomic<int> stopped{0};
  bus.subscribe(EventType::AudioStopped, [&](const Event&) { ++stopped; });

  auto hal = std::make_unique<StubBluetoothHal>();
  StubBluetoothHal* raw = hal.get();
  BluetoothManager bt(&bus, "Nexus Audio", std::move(hal));
  ASSERT_TRUE(bt.start().ok());

  raw->simulateConnect("Phone");
  bt.healthCheck();
  raw->simulateDisconnect();
  bt.healthCheck();
  bus.drain();

  EXPECT_EQ(stopped.load(), 1);
  EXPECT_FALSE(bt.isConnected());
  EXPECT_TRUE(bt.connectedDeviceName().empty());
}

TEST(BluetoothManager, PairingWindowRequiresRunning) {
  EventBus bus;
  BluetoothManager bt(&bus, "Nexus Audio", std::make_unique<StubBluetoothHal>());
  // Not started yet → refused.
  EXPECT_FALSE(bt.openPairingWindow(30).ok());
  ASSERT_TRUE(bt.start().ok());
  EXPECT_TRUE(bt.openPairingWindow(30).ok());
}
