// SPDX-License-Identifier: LGPL-2.1-or-later
#include "operation_executor.hpp"
#include <algorithm>
#include <rtt/Logger.hpp>
#include <rtt/internal/GlobalEngine.hpp>
#include <rtt/types/TypeInfo.hpp>

namespace RTT::http::detail {
Invocation::Invocation(std::shared_ptr<Operation> value)
    : operation(std::move(value)) {}

std::shared_ptr<Invocation>
prepareInvocation(std::shared_ptr<Operation> operation,
                  const boost::json::array &arguments,
                  const ServerOptions &options, CodecError *error) {
  if (arguments.size() != operation->arguments.size()) {
    codecFailure(error, CodecErrorCode::type_mismatch);
    return {};
  }
  auto invocation = std::make_shared<Invocation>(std::move(operation));
  auto &schema = *invocation->operation;
  auto &part = *schema.part;
  invocation->caller = std::make_unique<RTT::internal::OperationCallerC>(
      &part, part.getName(), RTT::internal::GlobalEngine::Instance());
  CodecContext input(
      {options.maxRequestBodyBytes, options.maxJsonDepth,
       static_cast<std::size_t>(options.maxRequestBodyBytes) * 8});
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    auto value =
        schema.arguments[i]->codec->makeDataSource(arguments[i], input, error);
    if (!value) {
      return {};
    }
    invocation->arguments.push_back(value);
    invocation->caller->arg(value);
  }
  invocation->caller->check();
  if (!invocation->caller->ready()) {
    codecFailure(error, CodecErrorCode::unavailable);
    return {};
  }
  if (schema.ownThread) {
    invocation->send = invocation->caller->getSendDataSource();
  }
  if (invocation->send) {
    // The selected RTT FusedMSendDataSource owns an assignable cached handle.
    // Its collector takes that handle by reference (set()), without evaluating
    // the sender. Bind to it before dispatch; do not allocate/copy a second
    // handle after sending or re-evaluate an operation to recover a result.
    if (!invocation->send->isAssignable()) {
      codecFailure(error, CodecErrorCode::unavailable);
      return {};
    }
    invocation->collector = std::make_unique<RTT::internal::SendHandleC>(
        invocation->send, invocation->send, &part, part.getName());
    invocation->collector->setAutoCollect(false);
  }
  CodecContext output(
      {options.maxResponseBodyBytes, options.maxJsonDepth,
       static_cast<std::size_t>(options.maxResponseBodyBytes) * 8});
  for (std::size_t i = 0; i < schema.collectTypes.size(); ++i) {
    DataSourcePtr value;
    if (!invocation->send && !(schema.hasResult && i == 0)) {
      value = invocation
                  ->arguments[schema.outputs[i - (schema.hasResult ? 1U : 0U)]];
    } else {
      // Allocate through the codec so its owned string/composite storage is
      // used for RTT outputs as well as decoded arguments.
      const auto prototype = schema.collectTypes[i]->buildValue();
      boost::json::value initial(output.storage());
      if (!prototype || !schema.collectBindings[i]->codec->toJson(
                            prototype, &initial, output, error)) {
        return {};
      }
      value = schema.collectBindings[i]->codec->makeDataSource(initial, output,
                                                               error);
    }
    if (!value) {
      return {};
    }
    invocation->values.push_back(value);
    if (invocation->collector) {
      invocation->collector->arg(value);
    } else if (schema.hasResult && i == 0) {
      invocation->caller->ret(value);
    }
  }
  if (invocation->collector) {
    invocation->collector->check();
    invocation->collector->setAutoCollect(false);
    if (!invocation->collector->ready()) {
      codecFailure(error, CodecErrorCode::unavailable);
      return {};
    }
  }
  return invocation;
}

OperationExecutor::OperationExecutor(std::uint32_t width) : width_(width) {
  try {
    workers_.reserve(width);
    for (std::uint32_t i = 0; i < width; ++i) {
      workers_.emplace_back([this] { worker(); });
    }
    reaper_ = std::thread([this] { reap(); });
  } catch (...) {
    {
      std::lock_guard lock(mutex_);
      shutdown_ = true;
    }
    wake_.notify_all();
    for (auto &worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    throw;
  }
}
OperationExecutor::~OperationExecutor() { shutdown(); }
void OperationExecutor::shutdown() noexcept {
  drain();
  {
    std::lock_guard lock(mutex_);
    shutdown_ = true;
  }
  wake_.notify_all();
  for (auto &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  if (reaper_.joinable()) {
    reaper_.join();
  }
}
bool OperationExecutor::admit(std::shared_ptr<Invocation> invocation,
                              std::uint32_t cap, std::uint32_t timeoutMs) {
  std::lock_guard lock(mutex_);
  if (shutdown_ || pending_.size() >= cap) {
    return false;
  }
  invocation->admitted = Clock::now();
  invocation->deadline =
      invocation->admitted + std::chrono::milliseconds(timeoutMs);
  pending_.push_back(invocation);
  try {
    queue_.push_back(std::move(invocation));
  } catch (...) {
    pending_.pop_back();
    throw;
  }
  wake_.notify_all();
  return true;
}
std::uint32_t OperationExecutor::count() const noexcept {
  std::lock_guard lock(mutex_);
  return static_cast<std::uint32_t>(pending_.size());
}
void OperationExecutor::finish(const std::shared_ptr<Invocation> &invocation,
                               int status) noexcept {
  {
    std::lock_guard lock(invocation->mutex);
    invocation->status = status;
    invocation->complete = true;
  }
  {
    std::lock_guard lock(mutex_);
    std::erase(pending_, invocation);
  }
  invocation->wake.notify_all();
  wake_.notify_all();
}
void OperationExecutor::worker() noexcept {
  for (;;) {
    std::shared_ptr<Invocation> invocation;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return shutdown_ || !queue_.empty(); });
      if (shutdown_ && queue_.empty()) {
        return;
      }
      invocation = std::move(queue_.front());
      queue_.pop_front();
    }
    if (!invocation->send) {
      int status = 500;
      try {
        status = invocation->caller->call() ? 200 : 503;
      } catch (RTT::SendStatus) {
        status = 503;
      } catch (...) {
      }
      finish(invocation, status);
      continue;
    }
    try {
      invocation->send->reset();
      invocation->send->evaluate();
    } catch (...) {
      // The original cached handle remains owned and is collected once below.
      // An exception cannot cause replay or unwind invocation storage early.
      std::lock_guard lock(invocation->mutex);
      invocation->status = 500;
    }
    {
      std::lock_guard lock(invocation->mutex);
      invocation->sent = true;
    }
    wake_.notify_all();
  }
}
void OperationExecutor::reap() noexcept {
  for (;;) {
    std::shared_ptr<Invocation> candidate;
    // Scan without allocating a snapshot and without holding the executor's
    // mutex while RTT copies output values. Completion never owns that lock.
    std::size_t index = 0;
    for (;;) {
      {
        std::lock_guard lock(mutex_);
        if (index >= pending_.size()) {
          break;
        }
        candidate = pending_[index++];
      }
      bool sent;
      {
        std::lock_guard lock(candidate->mutex);
        sent = candidate->sent && !candidate->complete;
      }
      if (!sent) {
        continue;
      }
      try {
        const auto status = candidate->collector->collectIfDone();
        if (status != RTT::SendNotReady) {
          int result;
          {
            std::lock_guard lock(candidate->mutex);
            result = candidate->status;
          }
          finish(candidate, result == 500                ? 500
                            : status == RTT::SendSuccess ? 200
                                                         : 503);
        }
      } catch (...) {
        finish(candidate, 500);
      }
    }
    std::unique_lock lock(mutex_);
    if (shutdown_) {
      return;
    }
    wake_.wait_for(lock, std::chrono::milliseconds(2));
  }
}
void OperationExecutor::drain() noexcept {
  auto nextDiagnostic = Clock::now() + std::chrono::seconds(5);
  std::unique_lock lock(mutex_);
  while (!pending_.empty()) {
    wake_.wait_for(lock, std::chrono::milliseconds(100));
    if (Clock::now() >= nextDiagnostic) {
      for (const auto &invocation : pending_) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                             Clock::now() - invocation->admitted)
                             .count();
        RTT::Logger::log().logf(
            RTT::Logger::Warning, "HTTP",
            "Waiting for %s (%lld ms); %zu HTTP operations remain",
            invocation->identity.c_str(), static_cast<long long>(age),
            pending_.size());
      }
      nextDiagnostic = Clock::now() + std::chrono::seconds(5);
    }
  }
}
} // namespace RTT::http::detail
