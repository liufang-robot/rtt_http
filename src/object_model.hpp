// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <boost/json.hpp>
#include <mutex>
#include <rtt/OperationInterfacePart.hpp>
#include <rtt/PortEndpoint.hpp>
#include <rtt/Service.hpp>
#include <rtt/base/OperationCallerInterface.hpp>
#include <rtt/base/PortInterface.hpp>
#include <rtt/http/type_protocol.hpp>
#include <set>

namespace RTT::http::detail {
std::string encodeSegment(std::string_view name);
bool canonicalRequestPath(std::string_view rawTarget, std::string *path);

struct PortBridge {
  std::shared_ptr<RTT::PortInputSource> source;
  std::mutex writer;
  bool enabled{true};
};
struct PortReader {
  std::shared_ptr<RTT::PortObservation> observation;
  std::mutex mutex;
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
  std::shared_ptr<PortReader> reader;
  std::shared_ptr<PortBridge> bridge;
  std::shared_ptr<Operation> operation;
  bool supported{true};
};
struct PublishedPort {
  RTT::PortEndpoint endpoint;
  std::string name;
  RTT::Service::shared_ptr service;
  std::string path;
  std::string servicePath;
  boost::json::object metadata;
  std::map<std::string, std::shared_ptr<PortBridge>> inputs;
};
struct Publication {
  RTT::TaskContext *component{};
  RTT::Service::shared_ptr service;
  boost::json::object summary;
  std::map<std::string, std::shared_ptr<const Resource>> routes;
  std::vector<std::string> diagnostics;
  std::map<std::string, PublishedPort> ports;
};
class ObjectModel {
public:
  explicit ObjectModel(std::shared_ptr<const TypeCatalog> catalog,
                       std::map<std::string, std::string> diagnostics);
  std::shared_ptr<Publication> stage(RTT::TaskContext &) const;
  bool commit(std::shared_ptr<Publication>, std::string *error);
  std::shared_ptr<const Resource> resolve(const std::string &) const;
  bool contains(const RTT::TaskContext *) const;
  bool enableInputWrite(RTT::TaskContext &, const std::string &, std::string *);
  bool disableInputWrite(RTT::TaskContext &, const std::string &,
                         std::string *);
  std::vector<std::string> diagnostics(const std::string &) const;

private:
  boost::json::object types(const std::set<std::string> &) const;
  boost::json::object describeService(Publication &, RTT::Service::shared_ptr,
                                      const std::string &, const std::string &,
                                      std::set<RTT::Service *> &,
                                      unsigned int) const;
  std::string unsupported(const RTT::types::TypeInfo *) const;
  std::shared_ptr<const Resource> portResource(const PublishedPort &,
                                               const RTT::PortEndpoint &,
                                               const std::string &) const;
  void updatePortMetadata(Publication &, PublishedPort &) const;
  std::shared_ptr<const TypeCatalog> catalog_;
  std::map<std::string, std::string> diagnostics_;
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<Publication>> publications_;
};
} // namespace RTT::http::detail
