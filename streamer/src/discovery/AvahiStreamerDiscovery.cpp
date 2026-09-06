#include "discovery/AvahiStreamerDiscovery.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>

#include <cstring>

#include "logging/Logger.h"

namespace nexus::streamer::discovery {

using core::ErrorCode;
using core::Result;
using core::Status;

namespace {

// Browse context: collects discovered speakers, stops the poll on ALL_FOR_NOW once resolves finish.
struct BrowseCtx {
  AvahiSimplePoll* poll = nullptr;
  AvahiClient* client = nullptr;
  std::vector<DiscoveredSpeaker> speakers;
  int pending_resolves = 0;
  bool all_for_now = false;
};

void maybeQuit(BrowseCtx* ctx) {
  if (ctx->all_for_now && ctx->pending_resolves == 0) avahi_simple_poll_quit(ctx->poll);
}

// Read one TXT key into `out` if present.
void txtValue(AvahiStringList* txt, const char* key, std::string& out) {
  if (AvahiStringList* e = avahi_string_list_find(txt, key)) {
    char* k = nullptr;
    char* v = nullptr;
    size_t vs = 0;
    if (avahi_string_list_get_pair(e, &k, &v, &vs) == 0 && v) {
      out.assign(v, vs);
      avahi_free(k);
      avahi_free(v);
    }
  }
}

void resolveCallback(AvahiServiceResolver* r, AvahiIfIndex, AvahiProtocol,
                     AvahiResolverEvent event, const char* name, const char* /*type*/,
                     const char* /*domain*/, const char* host_name, const AvahiAddress* addr,
                     uint16_t port, AvahiStringList* txt, AvahiLookupResultFlags, void* userdata) {
  auto* ctx = static_cast<BrowseCtx*>(userdata);
  if (event == AVAHI_RESOLVER_FOUND) {
    DiscoveredSpeaker s;
    s.device_id = name ? name : "";
    s.control_port = port;
    // Prefer the numeric address (usable for TCP connect); fall back to the .local hostname.
    char addr_str[AVAHI_ADDRESS_STR_MAX];
    if (addr) {
      avahi_address_snprint(addr_str, sizeof(addr_str), addr);
      s.host = addr_str;
    } else if (host_name) {
      s.host = host_name;
    }
    txtValue(txt, "device_id", s.device_id);  // TXT wins over the service name if present
    txtValue(txt, "box_public_key", s.box_public_key);
    std::string setup;
    txtValue(txt, "setup_mode", setup);
    s.setup_mode = (setup == "1" || setup == "true");
    ctx->speakers.push_back(std::move(s));
  }
  avahi_service_resolver_free(r);
  --ctx->pending_resolves;
  maybeQuit(ctx);
}

void browseCallback(AvahiServiceBrowser* /*b*/, AvahiIfIndex iface, AvahiProtocol proto,
                    AvahiBrowserEvent event, const char* name, const char* type, const char* domain,
                    AvahiLookupResultFlags, void* userdata) {
  auto* ctx = static_cast<BrowseCtx*>(userdata);
  switch (event) {
    case AVAHI_BROWSER_NEW:
      ++ctx->pending_resolves;
      avahi_service_resolver_new(ctx->client, iface, proto, name, type, domain, AVAHI_PROTO_UNSPEC,
                                 static_cast<AvahiLookupFlags>(0), resolveCallback, ctx);
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

AvahiStreamerDiscovery::~AvahiStreamerDiscovery() { stopAdvertising(); }

Status AvahiStreamerDiscovery::advertise(const StreamerAdvertisement& ad) {
  stopAdvertising();  // idempotent republish
  publish_poll_ = avahi_simple_poll_new();
  if (!publish_poll_) return Status::error(ErrorCode::IoError, "avahi_simple_poll_new failed");

  int err = 0;
  publish_client_ = avahi_client_new(avahi_simple_poll_get(publish_poll_), AVAHI_CLIENT_NO_FAIL,
                                     nullptr, nullptr, &err);
  if (!publish_client_) {
    return Status::error(ErrorCode::IoError, std::string("avahi_client_new: ") + avahi_strerror(err));
  }
  group_ = avahi_entry_group_new(publish_client_, nullptr, nullptr);
  if (!group_) return Status::error(ErrorCode::IoError, "avahi_entry_group_new failed");

  // TXT carries the fields the speaker's browseStreamers() reads (streamer_id + public_key).
  const std::string sid = "streamer_id=" + ad.streamer_id;
  const std::string pk = "public_key=" + ad.public_key;

  int r = avahi_entry_group_add_service(
      group_, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0),
      ad.streamer_id.c_str(), kStreamerService, nullptr, nullptr, static_cast<uint16_t>(ad.port),
      sid.c_str(), pk.c_str(), nullptr);
  if (r < 0) return Status::error(ErrorCode::IoError, std::string("add_service: ") + avahi_strerror(r));
  if (avahi_entry_group_commit(group_) < 0) {
    return Status::error(ErrorCode::IoError, "entry_group_commit failed");
  }
  avahi_simple_poll_iterate(publish_poll_, 100);
  NX_LOG_INFO("discovery", "avahi published streamer service " + ad.streamer_id);
  return Status::success();
}

Status AvahiStreamerDiscovery::stopAdvertising() {
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

Result<std::vector<DiscoveredSpeaker>> AvahiStreamerDiscovery::browseSpeakers() {
  BrowseCtx ctx;
  ctx.poll = avahi_simple_poll_new();
  if (!ctx.poll) return Status::error(ErrorCode::IoError, "avahi_simple_poll_new failed");

  int err = 0;
  ctx.client = avahi_client_new(avahi_simple_poll_get(ctx.poll), AVAHI_CLIENT_NO_FAIL, nullptr,
                                nullptr, &err);
  if (!ctx.client) {
    avahi_simple_poll_free(ctx.poll);
    return Status::error(ErrorCode::IoError, std::string("avahi_client_new: ") + avahi_strerror(err));
  }

  AvahiServiceBrowser* browser = avahi_service_browser_new(
      ctx.client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, kSpeakerService, nullptr,
      static_cast<AvahiLookupFlags>(0), browseCallback, &ctx);
  if (!browser) {
    avahi_client_free(ctx.client);
    avahi_simple_poll_free(ctx.poll);
    return Status::error(ErrorCode::IoError, "service_browser_new failed");
  }

  for (int i = 0; i < 50; ++i) {  // ~5s max
    if (avahi_simple_poll_iterate(ctx.poll, 100) != 0) break;
    if (ctx.all_for_now && ctx.pending_resolves == 0) break;
  }

  avahi_service_browser_free(browser);
  avahi_client_free(ctx.client);
  avahi_simple_poll_free(ctx.poll);
  return ctx.speakers;
}

}  // namespace nexus::streamer::discovery
