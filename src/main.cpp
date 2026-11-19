#include <csignal>
#include <cstdio>
#include <string>

#include "http/router.h"
#include "server/config.h"
#include "server/server.h"
#include "util/log.h"

namespace {

void register_builtin_routes(hws::Server& server) {
  const hws::Config& cfg = server.config();
  hws::http::Router& r = server.router();
  if (!cfg.health_path.empty()) {
    r.add(cfg.health_path, hws::http::kGet, [&server](const hws::http::Request&, hws::http::Response& resp) {
      // Load balancers see 503 as soon as draining starts and stop routing new traffic here.
      if (server.draining()) resp.text(503, "draining\n");
      else resp.text(200, "ok\n");
      resp.add_header("Cache-Control", "no-store");
    });
  }
  if (!cfg.metrics_path.empty()) {
    r.add(cfg.metrics_path, hws::http::kGet, [&server](const hws::http::Request&, hws::http::Response& resp) {
      resp.set_content_type("text/plain; version=0.0.4; charset=utf-8");
      resp.add_header("Cache-Control", "no-store");
      resp.set_body(server.metrics_text());
    });
  }
  if (!cfg.hello_path.empty()) {
    r.add(cfg.hello_path, hws::http::kGet, [](const hws::http::Request&, hws::http::Response& resp) {
      resp.set_content_type("text/plain");
      resp.set_body_view("Hello, World!");
    });
  }
  if (!cfg.echo_path.empty()) {
    r.add(cfg.echo_path, hws::http::kPost | hws::http::kPut,
          [](const hws::http::Request& req, hws::http::Response& resp) {
            const std::string_view type = req.header("content-type");
            resp.set_content_type(type.empty() ? std::string_view("application/octet-stream") : type);
            resp.set_body(req.body);
          });
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);  // writes use MSG_NOSIGNAL; sendfile(2) can still raise it

  hws::Config cfg;
  std::string err;
  switch (hws::parse_args(argc, argv, cfg, err)) {
    case hws::ArgsResult::Exit:
      return 0;
    case hws::ArgsResult::Error:
      std::fprintf(stderr, "hws: %s\n", err.c_str());
      return 2;
    case hws::ArgsResult::Run:
      break;
  }
  if (!hws::validate_config(cfg, err)) {
    std::fprintf(stderr, "hws: %s\n", err.c_str());
    return 2;
  }

  hws::Server server(std::move(cfg));
  if (!server.init(err)) {
    LOG_ERROR("%s", err.c_str());
    return 1;
  }
  register_builtin_routes(server);
  return server.run();
}
