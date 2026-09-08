// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <boost/json.hpp>
#include <mutex>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/Service.hpp>
#include <rtt/base/OperationCallerInterface.hpp>
#include <rtt/base/PortInterface.hpp>
#include <rtt/http/type_protocol.hpp>
#include <set>

namespace RTT::http::detail {
std::string encodeSegment(std::string_view name);
bool canonicalRequestPath(std::string_view rawTarget, std::string *path);

struct PortBridge {
  ~PortBridge();
  std::unique_ptr<RTT::base::PortInterface> peer;
  std::mutex writer;
};
struct Operation {
  RTT::Service::shared_ptr service;
  RTT::OperationInterfacePart *part{};
  boost::shared_ptr<RTT::base::DisposableInterface> implementation;
  std::vector<const TypeBinding *> arguments;
  std::vector<const RTT::types::TypeInfo *> collectTypes;
  std::vector<const TypeBinding *> collectBindings;
  const TypeBinding *result{};
  bool hasResult{false};
  bool ownThread{false};
  std::vector<std::size_t> outputs;
};
enum class ResourceKind { description, value, operation, latest, samples };
struct Resource {
  ResourceKind kind{ResourceKind::description};
  std::string allow{"GET, HEAD, OPTIONS"};
  boost::json::value description;
  DataSourcePtr source;
  const TypeBinding *binding{};
  RTT::base::OutputPortInterface *output{};
  std::shared_ptr<PortBridge> bridge;
  std::shared_ptr<Operation> operation;
  bool supported{true};
};
struct Publication {
  RTT::TaskContext *component{};
  RTT::Service::shared_ptr service;
  boost::json::object summary;
  std::map<std::string, std::shared_ptr<const Resource>> routes;
  std::vector<std::string> diagnostics;
  std::vector<std::shared_ptr<PortBridge>> observers;
};
class ObjectModel {
public:
  explicit ObjectModel(std::shared_ptr<const TypeCatalog> catalog,
                       std::map<std::string, std::string> diagnostics);
  std::shared_ptr<Publication> stage(RTT::TaskContext &) const;
  bool commit(std::shared_ptr<Publication>, std::string *error);
  std::shared_ptr<const Resource> resolve(const std::string &) const;
  bool contains(const RTT::TaskContext *) const;
  std::vector<std::string> diagnostics(const std::string &) const;

private:
  boost::json::object types(const std::set<std::string> &) const;
  boost::json::object describeService(Publication &, RTT::Service::shared_ptr,
                                      const std::string &, const std::string &,
                                      std::set<RTT::Service *> &,
                                      unsigned int) const;
  std::string unsupported(const RTT::types::TypeInfo *) const;
  std::shared_ptr<const TypeCatalog> catalog_;
  std::map<std::string, std::string> diagnostics_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Publication>> publications_;
};
} // namespace RTT::http::detail
