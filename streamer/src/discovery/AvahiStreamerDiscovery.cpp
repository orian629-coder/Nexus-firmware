#include "discovery/AvahiStreamerDiscovery.h"

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/simple-watch.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "logging/Logger.h"

namespace nexus::streamer::discovery {

using core::ErrorCode;
using core::Result;
using core::Status;

// State shared with the Avahi publish callbacks (via userdata). Lives as long as the advertisement;
// AvahiStreamerDiscovery owns it and frees it in stopAdvertising() once the poll thread is stopped.
struct PublishCtx {
  AvahiEntryGroup* group = nullptr;
  std::string streamer_id;
  std::string public_key;
  int port = 0;
};

namespace {

void entryGroupCallback(AvahiEntryGroup* g, AvahiEntryGroupState state, void* userdata);

// (Re)register the streamer service into the entry group. Called from the client callback once the
// daemon is running, so it also re-publishes automatically after an avahi-daemon restart.
void addService(AvahiClient* client, PublishCtx* ctx) {
  if (!ctx->group) {
    ctx->group = avahi_entry_group_new(client, entryGroupCallback, ctx);
    if (!ctx->group) {
      NX_LOG_ERROR("discovery", ErrorCode::IoError,
                   std::string("avahi_entry_group_new: ") + avahi_strerror(avahi_client_errno(client)));
      return;
    }
  }
  if (!avahi_entry_group_is_empty(ctx->group)) return;  // already registered

  // TXT carries the fields the speaker's browseStreamers() reads (streamer_id + public_key). The
  // service INSTANCE NAME is the streamer_id — part of the wire contract, so it must not be renamed.
  const std::string sid = "streamer_id=" + ctx->streamer_id;
  const std::string pk = "public_key=" + ctx->public_key;
  int r = avahi_entry_group_add_service(
      ctx->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0),
      ctx->streamer_id.c_str(), AvahiStreamerDiscovery::kStreamerService, nullptr, nullptr,
      static_cast<uint16_t>(ctx->port), sid.c_str(), pk.c_str(), nullptr);
  if (r < 0) {
    NX_LOG_ERROR("discovery", ErrorCode::IoError, std::string("add_service: ") + avahi_strerror(r));
    return;
  }
  if (avahi_entry_group_commit(ctx->group) < 0) {
    NX_LOG_ERROR("discovery", ErrorCode::IoError,
                 std::string("entry_group_commit: ") +
                     avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(ctx->group))));
  }
}

void entryGroupCallback(AvahiEntryGroup* g, AvahiEntryGroupState state, void* userdata) {
  auto* ctx = static_cast<PublishCtx*>(userdata);
  switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
      NX_LOG_INFO("discovery", "streamer service announced: " + ctx->streamer_id);
      break;
    case AVAHI_ENTRY_GROUP_COLLISION:
      // The instance name IS the streamer_id (wire contract), so we deliberately do NOT rename to an
      // alternative — a collision means two streamers claim the same id on this LAN, a real
      // misconfiguration the operator must resolve, not something to paper over with a rename.
      NX_LOG_ERROR("discovery", ErrorCode::AlreadyExists,
                   "streamer_id collision on the network: " + ctx->streamer_id);
      break;
    case AVAHI_ENTRY_GROUP_FAILURE:
      NX_LOG_ERROR("discovery", ErrorCode::IoError,
                   std::string("entry group failure: ") +
                       avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(g))));
      break;
    default:  // UNCOMMITED / REGISTERING — transient
      break;
  }
}

void clientCallback(AvahiClient* client, AvahiClientState state, void* userdata) {
  auto* ctx = static_cast<PublishCtx*>(userdata);
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      // Daemon is up (initial connect, or reconnect after a restart) — (re)publish our service.
      addService(client, ctx);
      break;
    case AVAHI_CLIENT_FAILURE:
      NX_LOG_ERROR("discovery", ErrorCode::IoError,
                   std::string("avahi client failure: ") + avahi_strerror(avahi_client_errno(client)));
      break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
      // The server is reconfiguring (e.g. host name change); drop our group and it will be re-added
      // when the client returns to S_RUNNING.
      if (ctx->group) avahi_entry_group_reset(ctx->group);
      break;
    case AVAHI_CLIENT_CONNECTING:
    default:
      break;
  }
}

// ---- browse (one-shot snapshot) ------------------------------------------------------------------

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

  publish_ = new PublishCtx{};
  publish_->streamer_id = ad.streamer_id;
  publish_->public_key = ad.public_key;
  publish_->port = ad.port;

  publish_poll_ = avahi_threaded_poll_new();
  if (!publish_poll_) {
    stopAdvertising();
    return Status::error(ErrorCode::IoError, "avahi_threaded_poll_new failed");
  }

  int err = 0;
  // NO_FAIL: tolerate avahi-daemon being down at start; the client callback publishes once it comes
  // up (and re-publishes after a restart). The group is created inside that callback, on the poll
  // thread — not here — which is the canonical avahi-threaded-poll publish pattern.
  publish_client_ = avahi_client_new(avahi_threaded_poll_get(publish_poll_), AVAHI_CLIENT_NO_FAIL,
                                     clientCallback, publish_, &err);
  if (!publish_client_) {
    stopAdvertising();
    return Status::error(ErrorCode::IoError, std::string("avahi_client_new: ") + avahi_strerror(err));
  }

  if (avahi_threaded_poll_start(publish_poll_) < 0) {
    stopAdvertising();
    return Status::error(ErrorCode::IoError, "avahi_threaded_poll_start failed");
  }

  NX_LOG_INFO("discovery", "avahi advertising streamer service " + ad.streamer_id + " (threaded)");
  return Status::success();
}

Status AvahiStreamerDiscovery::stopAdvertising() {
  // Stop the event-loop thread first so no callback runs while we tear the objects down.
  if (publish_poll_) avahi_threaded_poll_stop(publish_poll_);
  if (publish_ && publish_->group) {
    avahi_entry_group_free(publish_->group);
    publish_->group = nullptr;
  }
  if (publish_client_) {
    avahi_client_free(publish_client_);
    publish_client_ = nullptr;
  }
  if (publish_poll_) {
    avahi_threaded_poll_free(publish_poll_);
    publish_poll_ = nullptr;
  }
  delete publish_;
  publish_ = nullptr;
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
