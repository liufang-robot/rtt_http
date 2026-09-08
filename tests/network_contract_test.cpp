// SPDX-License-Identifier: LGPL-2.1-or-later
#include "raw_client.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <rtt/TaskContext.hpp>
#include <rtt/http/server.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#endif

namespace {
void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
bool rejectedConnection(const std::vector<std::unique_ptr<RawClient>> &clients) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    for (const auto &client : clients) {
      if (client->disconnected(0)) {
        return true;
      }
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}
#ifndef _WIN32
volatile std::sig_atomic_t sigpipes = 0;
void onSigpipe(int) { sigpipes = 1; }
#endif
void exercise(bool tls) {
  const auto phase = [tls](const char *name) {
    std::cerr << (tls ? "HTTPS: " : "HTTP: ") << name << std::endl;
  };
  phase("start and exclusive bind");
  RTT::TaskContext controller(tls ? "secure" : "plain");
  int value = 17;
  controller.addProperty("value", value);
  RTT::http::Server server, collision;
  httplib::Server reservation;
  const auto port = reservation.bind_to_any_port("127.0.0.1");
  require(port > 0, "reserve test port");
  reservation.stop();
  RTT::http::ServerOptions options;
  options.port = static_cast<std::uint32_t>(port);
  options.tlsEnabled = tls;
  options.certificateFile = "test.crt";
  options.privateKeyFile = "test.key";
  options.workerThreads = 1;
  options.maxQueuedConnections = 1;
  options.shutdownGraceMs = 100;
  options.socketReadTimeoutMs = 30000;
  options.socketWriteTimeoutMs = 30000;
  options.keepAliveTimeoutMs = 30000;
  std::string error;
  require(server.start(options, &error), error.c_str());
  require(server.publishComponent(controller, &error), error.c_str());
  require(!collision.start(options),
          "second HTTP listener must fail exclusive bind");
  require(collision.state() == "Stopped", "bind failure completes cleanup");
  httplib::Client browser(std::string(tls ? "https://" : "http://") +
                          "127.0.0.1:" + std::to_string(port));
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  if (tls) {
    browser.set_ca_cert_path("test.crt");
  }
#endif
  browser.set_keep_alive(true);
  browser.set_read_timeout(2);
  phase("initial keep-alive request");
  auto response = browser.Get("/api/v1/components");
  require(response && response->status == 200,
          "HTTP(S) remains usable after bind failure");
  // The completed keep-alive exchange proves the sole worker owns this
  // connection. Another accepted socket is queued; excess sockets are rejected.
  std::vector<std::unique_ptr<RawClient>> waiting;
  phase("queue overflow behind keep-alive connection");
  for (int i = 0; i != 4; ++i) {
    waiting.push_back(std::make_unique<RawClient>(port));
  }
  require(rejectedConnection(waiting),
          "bounded queue rejects excess connections");
  auto before = std::chrono::steady_clock::now();
  phase("stop with keep-alive and queued sockets");
  server.stop();
  phase("keep-alive cleanup joined");
  require(std::chrono::steady_clock::now() - before < std::chrono::seconds(2),
          "stop interrupts keep-alive and disposes queued sockets without "
          "timeout waits");
  for (auto &client : waiting) {
    require(client->disconnected(), "every queued/rejected socket closes");
  }
  waiting.clear();
  require(server.state() == "Stopped", "network joins before Stopped");
  require(server.start(options, &error), error.c_str());
  phase("request after same-port restart");
  response = browser.Get("/api/v1/components/" + controller.getName() +
                         "/properties/value");
  require(response && response->status == 200,
          "same-port restart preserves publication");
  browser.stop();
  phase("queue overflow behind stalled header or TLS handshake");
  // With no completed HTTP/TLS exchange these clients stall either in header
  // reading or TLS handshake. Queue rejection proves the accept loop reached
  // them before stop; shutdown must not perform queued TLS handshakes.
  for (int i = 0; i != 4; ++i) {
    waiting.push_back(std::make_unique<RawClient>(port));
  }
  // Worker handoff can race client arrival, so rejection need not occur on
  // the last socket. At most one active and one queued socket can survive.
  require(rejectedConnection(waiting),
          "stalled clients exhaust bounded admission");
  before = std::chrono::steady_clock::now();
  phase("stop with stalled sockets");
  server.stop();
  phase("stalled socket cleanup joined");
  require(std::chrono::steady_clock::now() - before < std::chrono::seconds(2),
          "grace interrupts stalled headers/TLS handshakes");
  for (auto &client : waiting) {
    require(client->disconnected(), "stalled connection closes");
  }
  if (tls) {
    phase("recover invalid TLS configuration");
    options.privateKeyFile = "missing-test-key.pem";
    require(!server.start(options) && server.state() == "Stopped",
            "invalid TLS configuration is recoverable");
    options.privateKeyFile = "test.key";
  }
  require(server.start(options, &error), error.c_str());
  phase("request after recovery");
  response = browser.Get("/api/v1/components");
  require(response && response->status == 200,
          "recovery leaves HTTP(S) usable");
  server.finishShutdown();
  phase("final shutdown joined");
}
} // namespace

int main() {
  try {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
#ifndef _WIN32
    const auto previous = std::signal(SIGPIPE, onSigpipe);
    struct Restore {
      decltype(previous) handler;
      ~Restore() { std::signal(SIGPIPE, handler); }
    } restore{previous};
#endif
    exercise(false);
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    exercise(true);
#endif
#ifndef _WIN32
    struct sigaction current {};
    require(sigaction(SIGPIPE, nullptr, &current) == 0 &&
                current.sa_handler == onSigpipe && !sigpipes,
            "HTTP preserves process SIGPIPE policy");
#endif
    std::cout << "HTTP/TLS exclusive bind, queue limits, stalled I/O, grace, "
                 "restart and signal isolation passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
