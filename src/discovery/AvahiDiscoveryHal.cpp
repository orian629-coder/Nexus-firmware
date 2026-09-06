#include "discovery/AvahiDiscoveryHal.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <cstring>

#include "logging/Logger.h"

namespace nexus::discovery {

using core::ErrorCode;
using core::Result;
using core::Status;

namespace {

// Browse context collects discovered streamer records and stops the poll when the browser signals
// ALL_FOR_NOW (the initial cache is complete).
struct BrowseCtx {
  AvahiSimplePoll* poll = nullptr;
  AvahiClient* client = nullptr;
  std::vector<StreamerRecord> records;
  int pending_resolves = 0;
  bool all_for_now = false;
};

void maybeQuit(BrowseCtx* ctx) {
  if (ctx->all_for_now && ctx->pending_resolves == 0) avahi_simple_poll_quit(ctx->poll);
}

void resolveCallback(AvahiServiceResolver* r, AvahiIfIndex, AvahiProtocol,
                     AvahiResolverEvent event, const char* name, const char* /*type*/,
                     const char* /*domain*/, const char* host_name, const AvahiAddress* /*addr*/,
                     uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* userdata) {
  auto* ctx = static_cast<BrowseCtx*>(userdata);
  if (event == AVAHI_RESOLVER_FOUND) {
    StreamerRecord rec;
    rec.streamer_id = name ? name : "";
    rec.host = host_name ? host_name : "";
    rec.port = port;
    // Pull the public key from the TXT record if present.
    if (AvahiStringList* pk = avahi_string_list_find(txt, "public_key")) {
      char* key = nullptr;
      char* val = nullptr;
      size_t val_size = 0;
      if (avahi_string_list_get_pair(pk, &key, &val, &val_size) == 0 && val) {
        rec.public_key.assign(val, val_size);
        avahi_free(key);
        avahi_free(val);
      }
    }
    ctx->records.push_back(std::move(rec));
  }
  avahi_service_resolver_free(r);
  --ctx->pending_resolves;
  maybeQuit(ctx);
}

void browseCallback(AvahiServiceBrowser* /*b*/, AvahiIfIndex iface, AvahiProtocol proto,
                    AvahiBrowserEvent event, const char* name, const char* type,
                    const char* domain, AvahiLookupResultFlags, void* userdata) {
  auto* ctx = static_cast<BrowseCtx*>(userdata);
  switch (event) {
    case AVAHI_BROWSER_NEW:
      ++ctx->pending_resolves;
      avahi_service_resolver_new(ctx->client, iface, proto, name, type, domain,
                                 AVAHI_PROTO_UNSPEC, static_cast<AvahiLookupFlags>(0), resolveCallback, ctx);
      break;
    case AVAHI_BROWSER_ALL_FOR_NOW:
      ctx->all_for_now = true;
      maybeQuit(ctx);
      break;
    case AVAHI_BROWSER_FAILURE:
      avahi_simple_poll_quit(ctx->poll);
      break;
    default:
      break;
  }
}

}  // namespace

AvahiDiscoveryHal::AvahiDiscoveryHal() = default;

AvahiDiscoveryHal::~AvahiDiscoveryHal() { stopBeacon(); }

Status AvahiDiscoveryHal::publishBeacon(const SpeakerBeacon& beacon) {
  stopBeacon();  // idempotent republish
  publish_poll_ = avahi_simple_poll_new();
  if (!publish_poll_) return Status::error(ErrorCode::IoError, "avahi_simple_poll_new failed");

  int err = 0;
  publish_client_ = avahi_client_new(avahi_simple_poll_get(publish_poll_),
                                     AVAHI_CLIENT_NO_FAIL, nullptr, nullptr, &err);
  if (!publish_client_) {
    return Status::error(ErrorCode::IoError,
                         std::string("avahi_client_new: ") + avahi_strerror(err));
  }
  group_ = avahi_entry_group_new(publish_client_, nullptr, nullptr);
  if (!group_) return Status::error(ErrorCode::IoError, "avahi_entry_group_new failed");

  // TXT record carries the fields a Streamer needs to pair.
  const std::string device = "device_id=" + beacon.device_id;
  const std::string model = "model=" + beacon.model;
  const std::string ver = "software_version=" + beacon.software_version;
  const std::string box = "box_public_key=" + beacon.box_public_key;
  const std::string setup = std::string("setup_mode=") + (beacon.setup_mode ? "1" : "0");

  int r = avahi_entry_group_add_service(
      group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0),
      beacon.device_id.c_str(), kSpeakerService, nullptr, nullptr,
      static_cast<uint16_t>(beacon.control_port), device.c_str(), model.c_str(), ver.c_str(),
      box.c_str(), setup.c_str(), nullptr);
  if (r < 0) {
    return Status::error(ErrorCode::IoError,
                         std::string("add_service: ") + avahi_strerror(r));
  }
  if (avahi_entry_group_commit(group_) < 0) {
    return Status::error(ErrorCode::IoError, "entry_group_commit failed");
  }
  // Pump the poll briefly so the registration is announced.
  avahi_simple_poll_iterate(publish_poll_, 100);
  NX_LOG_INFO("discovery", "avahi published beacon for " + beacon.device_id);
  return Status::success();
}

Status AvahiDiscoveryHal::stopBeacon() {
  if (group_) {
    avahi_entry_group_free(group_);
    group_ = nullptr;
  }
  if (publish_client_) {
    avahi_client_free(publish_client_);
    publish_client_ = nullptr;
  }
  if (publish_poll_) {
    avahi_simple_poll_free(publish_poll_);
    publish_poll_ = nullptr;
  }
  return Status::success();
}

Result<std::vector<StreamerRecord>> AvahiDiscoveryHal::browseStreamers() {
  BrowseCtx ctx;
  ctx.poll = avahi_simple_poll_new();
  if (!ctx.poll) return Status::error(ErrorCode::IoError, "avahi_simple_poll_new failed");

  int err = 0;
  ctx.client = avahi_client_new(avahi_simple_poll_get(ctx.poll), AVAHI_CLIENT_NO_FAIL, nullptr,
                                nullptr, &err);
  if (!ctx.client) {
    avahi_simple_poll_free(ctx.poll);
    return Status::error(ErrorCode::IoError,
                         std::string("avahi_client_new: ") + avahi_strerror(err));
  }

  AvahiServiceBrowser* browser = avahi_service_browser_new(
      ctx.client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, kStreamerService, nullptr,
      static_cast<AvahiLookupFlags>(0), browseCallback, &ctx);
  if (!browser) {
    avahi_client_free(ctx.client);
    avahi_simple_poll_free(ctx.poll);
    return Status::error(ErrorCode::IoError, "service_browser_new failed");
  }

  // Run the poll until ALL_FOR_NOW + resolves complete, with an overall timeout guard.
  for (int i = 0; i < 50; ++i) {  // ~5s max (100ms iterations)
    if (avahi_simple_poll_iterate(ctx.poll, 100) != 0) break;
    if (ctx.all_for_now && ctx.pending_resolves == 0) break;
  }

  avahi_service_browser_free(browser);
  avahi_client_free(ctx.client);
  avahi_simple_poll_free(ctx.poll);
  return ctx.records;
}

}  // namespace nexus::discovery
