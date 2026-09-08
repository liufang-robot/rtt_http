// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <cstdint>
#include <rtt/http/export.hpp>
#include <string>

namespace RTT::http {
struct ServerOptions {
  std::string bindAddress{"127.0.0.1"};
  std::uint32_t port{8080};
  bool tlsEnabled{false};
  std::string certificateFile;
  std::string privateKeyFile;
  std::uint32_t workerThreads{4};
  std::uint32_t maxQueuedConnections{64};
  std::uint32_t maxPendingOperations{32};
  std::uint32_t operationTimeoutMs{5000};
  std::uint32_t maxRequestBodyBytes{1048576};
  std::uint32_t maxResponseBodyBytes{8388608};
  std::uint32_t maxJsonDepth{64};
  std::uint32_t socketReadTimeoutMs{5000};
  std::uint32_t socketWriteTimeoutMs{5000};
  std::uint32_t keepAliveTimeoutMs{5000};
  std::uint32_t keepAliveMaxRequests{100};
  std::uint32_t shutdownGraceMs{10000};
};
RTT_HTTP_API bool validateOptions(const ServerOptions &,
                                  std::string *error = nullptr);
RTT_HTTP_API std::string endpointUrl(const ServerOptions &);
} // namespace RTT::http
