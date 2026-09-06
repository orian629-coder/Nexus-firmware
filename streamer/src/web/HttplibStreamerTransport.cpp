#include "web/HttplibStreamerTransport.h"

#include <httplib.h>

namespace nexus::streamer::web {

using core::ErrorCode;
using core::Status;
using nexus::web::HttpRequest;
using nexus::web::HttpResponse;

HttplibStreamerTransport::HttplibStreamerTransport()
    : server_(std::make_unique<httplib::Server>()) {}

HttplibStreamerTransport::~HttplibStreamerTransport() { stop(); }

Status HttplibStreamerTransport::start(int port, Handler handler) {
  handler_ = std::move(handler);
  auto dispatch = [this](const httplib::Request& req, httplib::Response& res) {
    HttpRequest r;
    r.method = req.method;
    r.path = req.path;
    r.body = req.body;
    if (req.has_header("Authorization")) r.auth = req.get_header_value("Authorization");
    if (req.has_header("Host")) r.host = req.get_header_value("Host");
    HttpResponse out = handler_ ? handler_(r) : HttpResponse{500, "application/json", "{}", {}};
    res.status = out.status;
    for (const auto& [k, v] : out.headers) res.set_header(k.c_str(), v.c_str());
    res.set_content(out.body, out.content_type.c_str());
  };
  server_->Get(".*", dispatch);
  server_->Post(".*", dispatch);

  running_ = true;
  // Loopback by default (see setBindAddress): this API can pair a speaker, which means it accepts a
  // Wi-Fi PSK in plaintext, so LAN exposure is an explicit opt-in rather than the default.
  const std::string bind = bind_address_;
  thread_ = std::thread([this, port, bind] { server_->listen(bind.c_str(), port); });
  if (!server_->is_valid()) return Status::error(ErrorCode::IoError, "invalid httplib server");
  return Status::success();
}

Status HttplibStreamerTransport::stop() {
  if (!running_.exchange(false)) return Status::success();
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
  return Status::success();
}

}  // namespace nexus::streamer::web
