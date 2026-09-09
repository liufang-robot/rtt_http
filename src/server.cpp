// SPDX-License-Identifier: LGPL-2.1-or-later
// Include before RTT headers, whose Xenomai backend defines a read_lock macro.
#include <httplib.h>

#include "object_model.hpp"
#include "operation_executor.hpp"
#include <algorithm>
#include <charconv>
#include <climits>
#include <limits>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/http/reflected_codec.hpp>
#include <rtt/http/server.hpp>

#if CPPHTTPLIB_SERVER_CONNECTION_SUPPORT != 1 ||                               \
    CPPHTTPLIB_SIGPIPE_POLICY_SUPPORT != 1 ||                                  \
    CPPHTTPLIB_OWNED_LISTENER_SUPPORT != 1 ||                                  \
    CPPHTTPLIB_RAW_ROUTING_SUPPORT != 1
#error The maintained cpp-httplib server ownership and SIGPIPE capabilities are required
#endif
#if defined(CPPHTTPLIB_ZLIB_SUPPORT) || defined(CPPHTTPLIB_BROTLI_SUPPORT) ||  \
    defined(CPPHTTPLIB_ZSTD_SUPPORT)
#error HTTP v1 does not enable compression or request decompression
#endif

namespace RTT::http {
namespace {
using detail::Clock;
bool fail(std::string *error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}
void blockSigpipe() {
#ifndef _WIN32
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &mask, nullptr)) {
    throw std::runtime_error("cannot block HTTP thread SIGPIPE");
  }
#endif
}
class NetworkPool final : public httplib::TaskQueue {
public:
  NetworkPool(std::uint32_t width, std::uint32_t cap,
              std::function<void()> onShutdown)
      : cap_(cap), onShutdown_(std::move(onShutdown)) {
    try {
      threads_.reserve(width);
      for (std::uint32_t i = 0; i < width; ++i) {
        threads_.emplace_back([this] { work(); });
      }
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~NetworkPool() override { shutdown(); }
  bool enqueue(std::function<void()> task) override {
    std::lock_guard lock(mutex_);
    if (closing_ || queue_.size() >= cap_) {
      return false;
    }
    queue_.push_back(std::move(task));
    wake_.notify_one();
    return true;
  }
  void shutdown() override {
    if (onShutdown_) {
      onShutdown_();
    }
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
      queue_.clear(); // owned connection task destruction closes queued sockets
    }
    wake_.notify_all();
    for (auto &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
  }

private:
  void work() noexcept {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return closing_ || !queue_.empty(); });
        if (closing_) {
          return;
        }
        task = std::move(queue_.front());
        queue_.pop_front();
      }
      try {
        task();
      } catch (...) {
      }
    }
  }
  std::uint32_t cap_;
  std::function<void()> onShutdown_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool closing_{false};
  std::deque<std::function<void()>> queue_;
  std::vector<std::thread> threads_;
};
std::string lowerTrim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  std::string result(text);
  for (auto &c : result) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
  }
  return result;
}
bool jsonMediaType(const httplib::Request &request) {
  if (request.get_header_value_count("Content-Type") != 1 ||
      request.has_header("Content-Encoding")) {
    return false;
  }
  const auto raw = request.get_header_value("Content-Type");
  const auto separator = raw.find(';');
  if (lowerTrim(std::string_view(raw).substr(0, separator)) !=
      "application/json") {
    return false;
  }
  if (separator == std::string::npos) {
    return true;
  }
  const auto parameter = lowerTrim(std::string_view(raw).substr(separator + 1));
  const auto equal = parameter.find('=');
  if (equal == std::string::npos ||
      lowerTrim(std::string_view(parameter).substr(0, equal)) != "charset") {
    return false;
  }
  const auto value = lowerTrim(std::string_view(parameter).substr(equal + 1));
  return value == "utf-8" || value == "\"utf-8\"";
}
const char *suffixFor(int status) {
  switch (status) {
  case 400:
    return "invalid-request";
  case 404:
    return "not-found";
  case 405:
    return "method-not-allowed";
  case 413:
    return "request-too-large";
  case 415:
    return "unsupported-media-type";
  case 422:
    return "type-mismatch";
  case 501:
    return "json-unsupported";
  case 503:
    return "dispatch-unavailable";
  case 504:
    return "operation-timeout";
  default:
    return "internal-error";
  }
}
void problem(const httplib::Request &request, httplib::Response &response,
             int status, const char *suffix = nullptr) {
  response.status = status;
  // A separate fixed reserve reports normal-response overflow without copying
  // arbitrary exception text or reflecting an unbounded request target.
  boost::json::object body{{"type", std::string("urn:rtt-http:error:") +
                                        (suffix ? suffix : suffixFor(status))},
                           {"title", httplib::status_message(status)},
                           {"status", status}};
  const auto path =
      std::string_view(request.target).substr(0, request.target.find('?'));
  if (path.size() <= 512 && validUtf8(path)) {
    body["instance"] = path;
  }
  CodecContext reserve({4096, 8, 32768});
  std::string encoded;
  if (!serializeJson(body, reserve, &encoded)) {
    encoded = "{\"type\":\"urn:rtt-http:error:internal-error\",\"title\":"
              "\"Internal Server Error\",\"status\":500}";
    response.status = 500;
  }
  response.set_content(std::move(encoded), "application/problem+json");
}
bool methodAllowed(const detail::Resource &resource,
                   const std::string &method) {
  if (method == "OPTIONS") {
    return true;
  }
  if (method == "GET" || method == "HEAD") {
    return resource.allow.starts_with("GET");
  }
  if (method == "PUT") {
    return resource.allow.find("PUT") != std::string::npos;
  }
  return method == "POST" && resource.allow.starts_with("POST");
}
bool responseOverflow(const CodecError &error) {
  return error.code == CodecErrorCode::document_limit ||
         error.code == CodecErrorCode::allocation_limit;
}
} // namespace

bool validateOptions(const ServerOptions &o, std::string *error) {
  if (o.bindAddress.empty() || o.bindAddress.size() > 1024 ||
      o.bindAddress.find('\0') != std::string::npos) {
    return fail(error, "bindAddress must be a nonempty host or IP address");
  }
  if (!o.port || o.port > 65535) {
    return fail(error, "port must be in 1..65535");
  }
  const std::pair<const char *, std::uint32_t> positive[] = {
      {"workerThreads", o.workerThreads},
      {"maxQueuedConnections", o.maxQueuedConnections},
      {"maxPendingOperations", o.maxPendingOperations},
      {"operationTimeoutMs", o.operationTimeoutMs},
      {"maxRequestBodyBytes", o.maxRequestBodyBytes},
      {"maxResponseBodyBytes", o.maxResponseBodyBytes},
      {"maxJsonDepth", o.maxJsonDepth},
      {"socketReadTimeoutMs", o.socketReadTimeoutMs},
      {"socketWriteTimeoutMs", o.socketWriteTimeoutMs},
      {"keepAliveTimeoutMs", o.keepAliveTimeoutMs},
      {"keepAliveMaxRequests", o.keepAliveMaxRequests},
      {"shutdownGraceMs", o.shutdownGraceMs}};
  for (const auto &[name, value] : positive) {
    if (!value) {
      return fail(error, std::string(name) + " must be positive");
    }
  }
  if (o.workerThreads > 1024) {
    return fail(error,
                "workerThreads exceeds the supported thread capacity (1024)");
  }
  if (o.socketReadTimeoutMs > INT_MAX || o.socketWriteTimeoutMs > INT_MAX ||
      o.keepAliveTimeoutMs > INT_MAX) {
    return fail(
        error,
        "socket and keep-alive timeouts exceed the native polling range");
  }
  if (o.keepAliveTimeoutMs % 1000) {
    return fail(
        error,
        "keepAliveTimeoutMs must be a whole number of seconds for cpp-httplib");
  }
  const auto memoryMax = (std::numeric_limits<std::size_t>::max)();
  if (o.maxRequestBodyBytes > memoryMax / 8 ||
      o.maxResponseBodyBytes > memoryMax / 8 ||
      o.maxQueuedConnections >
          memoryMax / sizeof(std::shared_ptr<httplib::ServerConnection>) -
              o.workerThreads - 1) {
    return fail(error, "configured payload or connection capacity exceeds "
                       "addressable storage");
  }
  if (o.tlsEnabled && (o.certificateFile.empty() || o.privateKeyFile.empty() ||
                       o.certificateFile.find('\0') != std::string::npos ||
                       o.privateKeyFile.find('\0') != std::string::npos)) {
    return fail(error,
                "certificateFile and privateKeyFile are required for TLS");
  }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (o.tlsEnabled) {
    return fail(error, "tlsEnabled requires an HTTP SDK built with OpenSSL");
  }
#endif
  if (error) {
    error->clear();
  }
  return true;
}
std::string endpointUrl(const ServerOptions &options) {
  auto host = options.bindAddress;
  if (host.find(':') != std::string::npos && !host.starts_with('[')) {
    host = "[" + host + "]";
  }
  return std::string(options.tlsEnabled ? "https://" : "http://") + host + ":" +
         std::to_string(options.port);
}

class Server::Impl {
public:
  enum class State { stopped, starting, running, stopping };
  std::mutex control;
  mutable std::mutex mutex;
  std::condition_variable wake;
  State state{State::stopped};
  bool finalShutdown{false};
  bool networkDone{true};
  bool graceExpired{false};
  Clock::time_point graceDeadline;
  ServerOptions options;
  std::string error;
  std::shared_ptr<detail::ObjectModel> model;
  std::shared_ptr<detail::OperationExecutor> executor;
  httplib::Server *listener{};
  std::vector<std::shared_ptr<httplib::ServerConnection>> connections;
  std::thread network;

  void requestStopLocked() noexcept {
    if (state == State::stopped || state == State::stopping) {
      return;
    }
    state = State::stopping;
    graceDeadline =
        Clock::now() + std::chrono::milliseconds(options.shutdownGraceMs);
    if (listener) {
      listener->stop();
    }
    for (const auto &connection : connections) {
      if (!connection->is_active()) {
        connection->cancel();
      }
    }
    wake.notify_all();
  }
  void watchGrace() noexcept {
    std::unique_lock lock(mutex);
    wake.wait(lock, [&] { return networkDone || state == State::stopping; });
    if (!networkDone &&
        !wake.wait_until(lock, graceDeadline, [&] { return networkDone; })) {
      graceExpired = true;
      for (const auto &connection : connections) {
        connection->cancel();
      }
      wake.notify_all();
    }
  }
  bool preflight(const httplib::Request &, httplib::Response &);
  void handle(const httplib::Request &, httplib::Response &);
  void run() noexcept;
};

bool Server::Impl::preflight(const httplib::Request &request,
                             httplib::Response &response) {
  std::string path;
  if (!detail::canonicalRequestPath(request.target, &path)) {
    problem(request, response, 400);
    return false;
  }
  {
    std::lock_guard lock(mutex);
    if (state != State::running || finalShutdown) {
      problem(request, response, 503);
      return false;
    }
  }
  const auto resource = model->resolve(path);
  if (!resource) {
    problem(request, response, 404);
    return false;
  }
  if (!methodAllowed(*resource, request.method)) {
    response.set_header("Allow", resource->allow);
    problem(request, response, 405);
    return false;
  }
  if (request.method == "OPTIONS") {
    response.status = 204;
    response.set_header("Allow", resource->allow);
    return false;
  }
  if (request.has_header("Content-Encoding") ||
      ((request.method == "PUT" || request.method == "POST") &&
       !jsonMediaType(request))) {
    problem(request, response, 415);
    return false;
  }
  if (request.has_header("Content-Length")) {
    const auto length = request.get_header_value("Content-Length");
    std::uint64_t declared = 0;
    const auto parsed =
        std::from_chars(length.data(), length.data() + length.size(), declared);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != length.data() + length.size()) {
      problem(request, response, 400);
      return false;
    }
    if (declared > options.maxRequestBodyBytes) {
      problem(request, response, 413);
      return false;
    }
  }
  return true;
}

void Server::Impl::handle(const httplib::Request &request,
                          httplib::Response &response) {
  try {
    if (!preflight(request, response)) {
      return;
    }
    std::string path;
    detail::canonicalRequestPath(request.target, &path);
    const auto resource = model->resolve(path);
    if (!resource) {
      problem(request, response, 404);
      return;
    }
    CodecContext input(
        {options.maxRequestBodyBytes, options.maxJsonDepth,
         static_cast<std::size_t>(options.maxRequestBodyBytes) * 8});
    boost::json::value body(input.storage());
    const bool mutation = request.method == "PUT" || request.method == "POST";
    CodecError failure;
    if (mutation) {
      if (!parseJson(request.body, input, &body, &failure)) {
        problem(request, response,
                responseOverflow(failure)                       ? 413
                : failure.code == CodecErrorCode::duplicate_key ? 422
                                                                : 400);
        return;
      }
      const char *field = resource->kind == detail::ResourceKind::operation
                              ? "arguments"
                              : "value";
      if (!body.is_object() || body.as_object().size() != 1 ||
          !body.as_object().contains(field)) {
        problem(request, response, 422);
        return;
      }
      if (resource->kind == detail::ResourceKind::operation &&
          (!body.at("arguments").is_array() ||
           body.at("arguments").as_array().size() !=
               resource->operation->arguments.size())) {
        problem(request, response, 422);
        return;
      }
    }
    if (!resource->supported) {
      problem(request, response, 501);
      return;
    }
    DataSourcePtr staged;
    std::shared_ptr<detail::Invocation> invocation;
    if (mutation && resource->kind != detail::ResourceKind::operation) {
      staged = resource->binding->codec->makeDataSource(body.at("value"), input,
                                                        &failure);
      if (!staged) {
        problem(request, response, 422);
        return;
      }
    } else if (resource->kind == detail::ResourceKind::operation) {
      invocation = detail::prepareInvocation(resource->operation,
                                             body.at("arguments").as_array(),
                                             options, &failure);
      if (!invocation) {
        problem(request, response,
                failure.code == CodecErrorCode::unavailable ? 503 : 422);
        return;
      }
      invocation->identity = path;
    }
    {
      // The common admission boundary is immediately before live RTT access.
      // Keeping this separate from routing leaves one place for later policy.
      std::lock_guard lock(mutex);
      if (state != State::running || finalShutdown) {
        problem(request, response, 503);
        return;
      }
      if (invocation &&
          !executor->admit(invocation, options.maxPendingOperations,
                           options.operationTimeoutMs)) {
        problem(request, response, 503, "operation-capacity-exhausted");
        return;
      }
    }
    CodecContext output(
        {options.maxResponseBodyBytes, options.maxJsonDepth,
         static_cast<std::size_t>(options.maxResponseBodyBytes) * 8});
    boost::json::value result(output.storage());
    boost::json::value value(output.storage());
    switch (resource->kind) {
    case detail::ResourceKind::description:
      result = boost::json::value(resource->description, output.storage());
      break;
    case detail::ResourceKind::value:
      if (mutation) {
        if (!resource->binding->codec->assign(staged, resource->source,
                                              &failure)) {
          problem(request, response, 500);
          return;
        }
        response.status = 204;
        return;
      }
      if (!resource->binding->codec->toJson(resource->source, &value, output,
                                            &failure)) {
        problem(request, response, 500,
                responseOverflow(failure) ? "response-too-large"
                                          : "value-not-json-representable");
        return;
      }
      result.emplace_object().emplace("value", std::move(value));
      break;
    case detail::ResourceKind::latest: {
      const auto status = resource->binding->codec->portValue(
          resource->output, &value, output, &failure);
      if (status == PortValueStatus::error) {
        problem(request, response, 500,
                responseOverflow(failure) ? "response-too-large"
                                          : "value-not-json-representable");
        return;
      }
      auto &object = result.emplace_object();
      object.emplace("hasSample", status == PortValueStatus::value);
      object.emplace("value", status == PortValueStatus::value
                                  ? std::move(value)
                                  : boost::json::value());
      break;
    }
    case detail::ResourceKind::samples: {
      std::lock_guard lock(resource->bridge->writer);
      auto *port = dynamic_cast<RTT::base::OutputPortInterface *>(
          resource->bridge->peer.get());
      if (!port) {
        problem(request, response, 500);
        return;
      }
      switch (port->write(staged)) {
      case RTT::WriteSuccess:
        response.status = 204;
        break;
      case RTT::NotConnected:
        problem(request, response, 503, "port-not-connected");
        break;
      case RTT::WriteFailure:
        problem(request, response, 503, "port-write-failed");
        break;
      default:
        problem(request, response, 500);
        break;
      }
      return;
    }
    case detail::ResourceKind::operation: {
      std::unique_lock lock(invocation->mutex);
      while (!invocation->complete) {
        bool cutoff;
        {
          std::lock_guard lifecycle(mutex);
          cutoff = graceExpired;
        }
        if (cutoff || Clock::now() >= invocation->deadline ||
            (request.is_connection_closed && request.is_connection_closed())) {
          problem(request, response, 504);
          return;
        }
        invocation->wake.wait_until(
            lock, (std::min)(invocation->deadline,
                             Clock::now() + std::chrono::milliseconds(10)));
      }
      if (invocation->status != 200) {
        problem(request, response, invocation->status);
        return;
      }
      auto &object = result.emplace_object();
      object.emplace("result", nullptr);
      boost::json::array outputs(output.storage());
      for (std::size_t i = 0; i < invocation->values.size(); ++i) {
        if (!resource->operation->collectBindings[i]->codec->toJson(
                invocation->values[i], &value, output, &failure)) {
          problem(request, response, 500,
                  responseOverflow(failure) ? "response-too-large"
                                            : "value-not-json-representable");
          return;
        }
        if (i == 0 && resource->operation->hasResult) {
          object["result"] = std::move(value);
        } else {
          outputs.push_back(std::move(value));
        }
      }
      object.emplace("outputs", std::move(outputs));
      break;
    }
    }
    std::string encoded;
    if (!serializeJson(result, output, &encoded, &failure)) {
      problem(request, response, 500,
              responseOverflow(failure) ? "response-too-large"
                                        : "value-not-json-representable");
      return;
    }
    response.status = 200;
    response.set_content(std::move(encoded), "application/json");
  } catch (const std::bad_alloc &) {
    problem(request, response, 500, "response-too-large");
  } catch (...) {
    problem(request, response, 500);
  }
}

void Server::Impl::run() noexcept {
  std::unique_ptr<httplib::Server> instance;
  std::thread watchdog;
  try {
    blockSigpipe();
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (options.tlsEnabled) {
      instance = std::make_unique<httplib::SSLServer>(
          options.certificateFile.c_str(), options.privateKeyFile.c_str());
    } else
#endif
    {
      instance = std::make_unique<httplib::Server>();
    }
    if (!instance->is_valid()) {
      throw std::runtime_error("certificateFile/privateKeyFile do not form a "
                               "valid TLS configuration");
    }
    instance->set_socket_options([](socket_t socket) {
      bool valid;
#ifdef _WIN32
      valid =
          httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
      valid = httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#ifdef SO_REUSEPORT
      valid =
          httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEPORT, 0) && valid;
#endif
#endif
      if (!valid) {
        throw std::runtime_error("cannot configure an exclusive HTTP listener");
      }
    });
    instance->set_read_timeout(
        std::chrono::milliseconds(options.socketReadTimeoutMs));
    instance->set_write_timeout(
        std::chrono::milliseconds(options.socketWriteTimeoutMs));
    instance->set_keep_alive_timeout(
        static_cast<time_t>(options.keepAliveTimeoutMs / 1000));
    instance->set_keep_alive_max_count(options.keepAliveMaxRequests);
    instance->set_payload_max_length(options.maxRequestBodyBytes);
    instance->new_task_queue = [this] {
      return new NetworkPool(options.workerThreads,
                             options.maxQueuedConnections, [this] {
                               // Also start cleanup when the accept loop exits
                               // on its own error path.
                               std::lock_guard lock(mutex);
                               requestStopLocked();
                             });
    };
    instance->set_connection_handler([this](auto connection) {
      std::lock_guard lock(mutex);
      std::erase_if(connections,
                    [](const auto &entry) { return entry->is_closed(); });
      if (state != State::running ||
          connections.size() >= connections.capacity()) {
        connection->cancel();
        return;
      }
      connections.push_back(std::move(connection));
    });
    instance->set_pre_routing_handler(
        [this](const auto &request, auto &response) {
          try {
            if (preflight(request, response)) {
              return httplib::Server::HandlerResponse::Unhandled;
            }
          } catch (...) {
            problem(request, response, 500);
          }
          // A rejected request may have an unread body. Do not let those bytes
          // become another keep-alive request on the same connection.
          response.set_header("Connection", "close");
          return httplib::Server::HandlerResponse::Handled;
        });
    instance->set_expect_100_continue_handler(
        [this](const auto &request, auto &response) {
          return preflight(request, response) ? 100 : response.status;
        });
    instance->set_error_handler([](const auto &request, auto &response) {
      if (response.get_header_value("Content-Type") !=
          "application/problem+json") {
        problem(request, response, response.status);
      }
    });
    instance->set_exception_handler(
        [](const auto &request, auto &response, std::exception_ptr) {
          problem(request, response, 500);
        });
    const auto callback = [this](const auto &request, auto &response) {
      handle(request, response);
    };
    // Matching only selects a body-reading handler; raw target resolution in
    // preflight/handle is the sole authority for RTT identity.
    instance->Get("[\\s\\S]*", callback);
    instance->Put("[\\s\\S]*", callback);
    instance->Post("[\\s\\S]*", callback);
    instance->set_start_handler([this] {
      std::lock_guard lock(mutex);
      if (state == State::starting && !finalShutdown) {
        state = State::running;
      } else if (listener) {
        listener->stop();
      }
      wake.notify_all();
    });
    if (!instance->bind_to_port(options.bindAddress,
                                static_cast<int>(options.port))) {
      throw std::runtime_error(
          "bindAddress/port could not bind the HTTP listener");
    }
    {
      std::lock_guard lock(mutex);
      listener = instance.get();
      if (state != State::starting || finalShutdown) {
        listener->stop();
      }
    }
    watchdog = std::thread([this] { watchGrace(); });
    const bool listened = instance->listen_after_bind();
    if (!listened) {
      std::lock_guard lock(mutex);
      error = "HTTP listener failed during startup or serving";
    }
  } catch (const std::exception &exception) {
    // Only local management receives our setup diagnostics. Exception text from
    // an extension is never reflected into an HTTP response.
    std::lock_guard lock(mutex);
    error = exception.what();
    requestStopLocked();
  } catch (...) {
    std::lock_guard lock(mutex);
    error = "HTTP listener setup failed";
    requestStopLocked();
  }
  {
    std::lock_guard lock(mutex);
    listener = nullptr;
    networkDone = true;
    connections.clear();
    wake.notify_all();
  }
  if (watchdog.joinable()) {
    watchdog.join();
  }
  instance
      .reset(); // HTTP and TLS destruction stay under the thread's SIGPIPE mask
  {
    std::lock_guard lock(mutex);
    state = State::stopped;
    wake.notify_all();
  }
}

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { finishShutdown(); }
bool Server::start(const ServerOptions &options, std::string *error) {
  std::lock_guard control(impl_->control);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->finalShutdown) {
      return fail(error, "HTTP deployment is shutting down");
    }
    if (impl_->state == Impl::State::running) {
      if (error) {
        error->clear();
      }
      return true;
    }
    if (impl_->state != Impl::State::stopped) {
      return fail(error, "HTTP network cleanup is not complete");
    }
  }
  if (impl_->network.joinable()) {
    impl_->network.join();
  }
  if (!validateOptions(options, error)) {
    return false;
  }
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->executor &&
        options.maxPendingOperations < impl_->executor->count()) {
      return fail(
          error, "maxPendingOperations is below retained HTTP operation count");
    }
    if (impl_->executor && impl_->executor->count() &&
        options.workerThreads != impl_->executor->width()) {
      return fail(error, "workerThreads cannot change while HTTP operations "
                         "remain unfinished");
    }
    impl_->options = options;
    impl_->error.clear();
    impl_->state = Impl::State::starting;
    impl_->graceExpired = false;
  }
  try {
    if (!impl_->model) {
      std::string registrationError;
      if (!typeCatalogFrozen() &&
          !registerCanonicalTypeProtocols(&registrationError)) {
        throw std::runtime_error(registrationError);
      }
      auto diagnostics = typeCatalogFrozen()
                             ? std::map<std::string, std::string>{}
                             : registerReflectedTypeProtocols();
      auto catalog = freezeTypeCatalog(&registrationError);
      if (!catalog) {
        throw std::runtime_error(registrationError);
      }
      auto model = std::make_shared<detail::ObjectModel>(
          std::move(catalog), std::move(diagnostics));
      std::lock_guard lock(impl_->mutex);
      impl_->model = std::move(model);
    }
    if (!impl_->executor || impl_->executor->width() != options.workerThreads) {
      std::shared_ptr<detail::OperationExecutor> old;
      {
        std::lock_guard lock(impl_->mutex);
        old = std::move(impl_->executor);
      }
      if (old) {
        old->shutdown();
      }
      old.reset();
      auto executor =
          std::make_shared<detail::OperationExecutor>(options.workerThreads);
      std::lock_guard lock(impl_->mutex);
      impl_->executor = std::move(executor);
    }
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->state != Impl::State::starting || impl_->finalShutdown) {
        impl_->state = Impl::State::stopped;
        return fail(error, "HTTP start was stopped before binding");
      }
      impl_->connections.reserve(
          static_cast<std::size_t>(options.workerThreads) +
          options.maxQueuedConnections + 1);
      impl_->networkDone = false;
    }
    impl_->network = std::thread([this] { impl_->run(); });
    std::unique_lock lock(impl_->mutex);
    impl_->wake.wait(lock, [&] {
      return impl_->state == Impl::State::running ||
             impl_->state == Impl::State::stopped;
    });
    if (impl_->state != Impl::State::running) {
      return fail(error, impl_->error.empty() ? "HTTP startup was interrupted"
                                              : impl_->error);
    }
    if (error) {
      error->clear();
    }
    return true;
  } catch (const std::exception &exception) {
    std::lock_guard lock(impl_->mutex);
    impl_->state = Impl::State::stopped;
    impl_->networkDone = true;
    return fail(error, exception.what());
  }
}
void Server::requestStop() noexcept {
  std::lock_guard lock(impl_->mutex);
  impl_->requestStopLocked();
}
void Server::stop() noexcept {
  requestStop();
  std::lock_guard control(impl_->control);
  requestStop();
  if (impl_->network.joinable()) {
    impl_->network.join();
  }
}
bool Server::isRunning() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->state == Impl::State::running && !impl_->finalShutdown;
}
std::string Server::state() const {
  std::lock_guard lock(impl_->mutex);
  switch (impl_->state) {
  case Impl::State::stopped:
    return "Stopped";
  case Impl::State::starting:
    return "Starting";
  case Impl::State::running:
    return "Running";
  case Impl::State::stopping:
    return "Stopping";
  }
  return "Stopping";
}
bool Server::publishComponent(RTT::TaskContext &component, std::string *error) {
  std::lock_guard control(impl_->control);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != Impl::State::running || impl_->finalShutdown) {
      return fail(error, "HTTP publication requires a running service");
    }
    if (impl_->model->contains(&component)) {
      if (error) {
        error->clear();
      }
      return true;
    }
  }
  try {
    auto publication = impl_->model->stage(component);
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != Impl::State::running || impl_->finalShutdown) {
      return fail(error, "HTTP stopped during publication");
    }
    return impl_->model->commit(std::move(publication), error);
  } catch (const std::exception &exception) {
    return fail(error, exception.what());
  } catch (...) {
    return fail(error, "HTTP publication failed");
  }
}
bool Server::isPublished(const RTT::TaskContext *component) const {
  std::shared_ptr<detail::ObjectModel> model;
  {
    std::lock_guard lock(impl_->mutex);
    model = impl_->model;
  }
  return model && model->contains(component);
}
std::vector<std::string>
Server::publicationDiagnostics(const std::string &name) const {
  std::shared_ptr<detail::ObjectModel> model;
  {
    std::lock_guard lock(impl_->mutex);
    model = impl_->model;
  }
  return model ? model->diagnostics(name) : std::vector<std::string>{};
}
std::uint32_t Server::pendingOperationCount() const noexcept {
  std::shared_ptr<detail::OperationExecutor> executor;
  {
    std::lock_guard lock(impl_->mutex);
    executor = impl_->executor;
  }
  return executor ? executor->count() : 0;
}
void Server::beginShutdown() noexcept {
  std::lock_guard lock(impl_->mutex);
  impl_->finalShutdown = true;
  impl_->requestStopLocked();
}
void Server::finishShutdown() noexcept {
  beginShutdown();
  stop();
  std::lock_guard control(impl_->control);
  std::shared_ptr<detail::OperationExecutor> executor;
  {
    std::lock_guard lock(impl_->mutex);
    executor = std::move(impl_->executor);
  }
  if (executor) {
    executor->shutdown();
  }
  executor.reset(); // keeps RTT component engines available through final drain
  std::shared_ptr<detail::ObjectModel> model;
  {
    std::lock_guard lock(impl_->mutex);
    model = std::move(impl_->model);
  }
  model.reset();
}
} // namespace RTT::http
