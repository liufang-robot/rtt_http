// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "object_model.hpp"
#include <condition_variable>
#include <deque>
#include <rtt/http/server_options.hpp>
#include <rtt/internal/OperationCallerC.hpp>
#include <rtt/internal/SendHandleC.hpp>
#include <thread>

namespace RTT::http::detail {
using Clock = std::chrono::steady_clock;
struct Invocation {
  explicit Invocation(std::shared_ptr<Operation>);
  std::shared_ptr<Operation> operation;
  std::string identity;
  std::vector<DataSourcePtr> arguments;
  std::vector<DataSourcePtr> values;
  std::unique_ptr<RTT::internal::OperationCallerC> caller;
  DataSourcePtr send;
  std::unique_ptr<RTT::internal::SendHandleC> collector;
  Clock::time_point admitted;
  Clock::time_point deadline;
  std::mutex mutex;
  std::condition_variable wake;
  bool sent{false};
  bool complete{false};
  int status{200};
};
// Construct all RTT argument/result/collection ownership without dispatching.
std::shared_ptr<Invocation> prepareInvocation(std::shared_ptr<Operation>,
                                              const boost::json::array &,
                                              const ServerOptions &,
                                              CodecError *error);

class OperationExecutor {
public:
  explicit OperationExecutor(std::uint32_t width);
  ~OperationExecutor();
  OperationExecutor(const OperationExecutor &) = delete;
  OperationExecutor &operator=(const OperationExecutor &) = delete;
  bool admit(std::shared_ptr<Invocation>, std::uint32_t cap,
             std::uint32_t timeoutMs);
  std::uint32_t count() const noexcept;
  std::uint32_t width() const noexcept { return width_; }
  void drain() noexcept;
  void shutdown() noexcept;

private:
  void worker() noexcept;
  void reap() noexcept;
  void finish(const std::shared_ptr<Invocation> &, int status) noexcept;
  const std::uint32_t width_;
  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::shared_ptr<Invocation>> queue_;
  std::vector<std::shared_ptr<Invocation>> pending_;
  bool shutdown_{false};
  std::vector<std::thread> workers_;
  std::thread reaper_;
};
} // namespace RTT::http::detail
