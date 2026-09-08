// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <memory>
#include <rtt/http/server_options.hpp>
#include <vector>

namespace RTT {
class TaskContext;
}
namespace RTT::http {
// Component lifetime is supplied by the embedding deployment. Published RTT
// objects and their interface storage must remain valid until shutdown
// finishes. The application must ensure safe concurrent property/attribute
// access.
class RTT_HTTP_API Server final {
public:
  Server();
  ~Server();
  Server(const Server &) = delete;
  Server &operator=(const Server &) = delete;
  bool start(const ServerOptions &, std::string *error = nullptr);
  void requestStop() noexcept;
  void stop() noexcept;
  bool isRunning() const noexcept;
  std::string state() const;
  bool publishComponent(RTT::TaskContext &, std::string *error = nullptr);
  bool isPublished(const RTT::TaskContext *) const;
  std::vector<std::string> publicationDiagnostics(const std::string &) const;
  std::uint32_t pendingOperationCount() const noexcept;
  // Begin closes admission without draining. Finish requires component engines
  // to remain alive and may wait indefinitely for application operations.
  void beginShutdown() noexcept;
  void finishShutdown() noexcept;

private:
  class Impl;
#if defined(_MSC_VER)
  // Impl construction and destruction stay in this DLL; unique_ptr itself
  // is header-defined and has no separately exported DLL interface.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
  std::unique_ptr<Impl> impl_;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
};
} // namespace RTT::http
