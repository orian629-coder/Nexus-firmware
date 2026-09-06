#include "web/HttplibWebTransport.h"

#include <httplib.h>

#include "logging/Logger.h"

namespace nexus::web {

using core::ErrorCode;
using core::Status;

HttplibWebTransport::HttplibWebTransport() : server_(std::make_unique<httplib::Server>()) {}

HttplibWebTransport::~HttplibWebTransport() { stop(); }

Status HttplibWebTransport::start(int port, Handler handler) {
  handler_ = std::move(handler);

  // Route everything through a catch-all: translate to HttpRequest, dispatch, write back.
  auto dispatch = [this](const httplib::Request& req, httplib::Response& res) {
    HttpRequest r;
    r.method = req.method;
    r.path = req.path;
    r.body = req.body;
    if (req.has_header("Authorization")) r.auth = req.get_header_value("Authorization");
    if (req.has_header("Host")) r.host = req.get_header_value("Host");
    NX_LOG_DEBUG("web", "req " + r.method + " " + r.path);
    HttpResponse out = handler_ ? handler_(r) : HttpResponse{500, "application/json", "{}", {}};
    res.status = out.status;
    for (const auto& [k, v] : out.headers) res.set_header(k.c_str(), v.c_str());
    res.set_content(out.body, out.content_type.c_str());
  };
  server_->Get(".*", dispatch);
  server_->Post(".*", dispatch);

  running_ = true;
  thread_ = std::thread([this, port] { server_->listen("0.0.0.0", port); });
  // Give the listener a moment; httplib has no ready callback. If binding fails, listen returns.
  if (!server_->is_valid()) {
    return Status::error(ErrorCode::IoError, "invalid httplib server");
  }
  return Status::success();
}

Status HttplibWebTransport::stop() {
  if (!running_.exchange(false)) return Status::success();
  if (server_) server_->stop();
  if (thread_.joinable()) thread_.join();
  return Status::success();
}

}  // namespace nexus::web
