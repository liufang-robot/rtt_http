// SPDX-License-Identifier: LGPL-2.1-or-later
#include "object_model.hpp"
#include <algorithm>
#include <cctype>
#include <rtt/PropertyBag.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/base/InputPortInterface.hpp>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/base/PropertyBase.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <stdexcept>

namespace RTT::http::detail {
namespace {
constexpr std::string_view root = "/api/v1/components";
bool validName(std::string_view name) {
  return !name.empty() && name.size() <= 1024 && name != "." && name != ".." &&
         name.find('\0') == std::string_view::npos && validUtf8(name);
}
bool unreserved(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}
int hex(unsigned char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}
std::string childPath(const std::string &parent, std::string_view name) {
  if (!validName(name)) {
    throw std::runtime_error("RTT name cannot be addressed by a browser URL");
  }
  auto result = parent + "/" + encodeSegment(name);
  if (result.size() > 4096) {
    throw std::runtime_error(
        "published RTT route exceeds the supported URL length");
  }
  return result;
}
std::string typeName(const RTT::types::TypeInfo *type) {
  return type ? type->getTypeName() : "unknown_t";
}
bool isMutableReference(std::string_view type) {
  while (!type.empty() &&
         std::isspace(static_cast<unsigned char>(type.back()))) {
    type.remove_suffix(1);
  }
  while (!type.empty() &&
         std::isspace(static_cast<unsigned char>(type.front()))) {
    type.remove_prefix(1);
  }
  return type.ends_with('&') && !type.starts_with("const ");
}
boost::json::object summary(std::string_view name, std::string_view description,
                            const std::string &href) {
  if (!validUtf8(description)) {
    throw std::runtime_error("RTT description is not valid UTF-8");
  }
  return {{"name", name}, {"description", description}, {"href", href}};
}
void insert(Publication &publication, const std::string &path,
            std::shared_ptr<Resource> resource) {
  if (!publication.routes.emplace(path, std::move(resource)).second) {
    throw std::runtime_error(
        "duplicate RTT resource identity during publication");
  }
}
void sortItems(boost::json::array &items) {
  std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
    return a.as_object().at("name").as_string() <
           b.as_object().at("name").as_string();
  });
}
} // namespace

std::string encodeSegment(std::string_view name) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string encoded;
  for (unsigned char c : name) {
    if (unreserved(c)) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += digits[c >> 4];
      encoded += digits[c & 15];
    }
  }
  return encoded;
}
bool canonicalRequestPath(std::string_view target, std::string *path) {
  if (!path || target.empty() || target.front() != '/' ||
      target.size() > 8192 || target.find('#') != std::string_view::npos) {
    return false;
  }
  // Query parameters do not select resources in v1. The origin path remains
  // authoritative; unknown parameters cannot broaden publication or dispatch.
  target = target.substr(0, target.find('?'));
  path->clear();
  std::size_t begin = 1;
  while (begin <= target.size()) {
    auto end = target.find('/', begin);
    if (end == std::string_view::npos) {
      end = target.size();
    }
    std::string decoded;
    for (std::size_t i = begin; i < end; ++i) {
      const auto c = static_cast<unsigned char>(target[i]);
      if (c == '%') {
        if (end - i < 3 || hex(target[i + 1]) < 0 || hex(target[i + 2]) < 0) {
          return false;
        }
        decoded +=
            static_cast<char>(hex(target[i + 1]) * 16 + hex(target[i + 2]));
        i += 2;
      } else {
        if (c <= 0x20 || c == 0x7f || c == '\\') {
          return false;
        }
        decoded += static_cast<char>(c);
      }
    }
    if (!validName(decoded)) {
      return false;
    }
    *path += "/" + encodeSegment(decoded);
    if (end == target.size()) {
      break;
    }
    begin = end + 1;
  }
  return true;
}

PortBridge::~PortBridge() {
  if (peer) {
    try {
      peer->disconnect();
    } catch (...) {
    }
  }
}
ObjectModel::ObjectModel(std::shared_ptr<const TypeCatalog> catalog,
                         std::map<std::string, std::string> diagnostics)
    : catalog_(std::move(catalog)), diagnostics_(std::move(diagnostics)) {}

std::string ObjectModel::unsupported(const RTT::types::TypeInfo *type) const {
  const auto diagnostic = diagnostics_.find(typeName(type));
  return diagnostic != diagnostics_.end() ? diagnostic->second
                                          : "no registered HTTP JSON codec";
}
boost::json::object
ObjectModel::types(const std::set<std::string> &references) const {
  boost::json::object result;
  std::vector<std::string> pending(references.begin(), references.end());
  while (!pending.empty()) {
    auto name = std::move(pending.back());
    pending.pop_back();
    if (result.contains(name)) {
      continue;
    }
    const auto *binding = catalog_->find(name);
    if (binding) {
      result.emplace(name, binding->registration.descriptor);
      for (const auto &dependency : binding->registration.dependencies) {
        pending.push_back(dependency);
      }
    } else {
      const auto reason = diagnostics_.find(name);
      result.emplace(name, boost::json::object{
                               {"kind", nullptr},
                               {"jsonSupported", false},
                               {"reason", reason == diagnostics_.end()
                                              ? "no registered HTTP JSON codec"
                                              : reason->second}});
    }
  }
  return result;
}

boost::json::object ObjectModel::describeService(
    Publication &publication, RTT::Service::shared_ptr service,
    const std::string &name, const std::string &path,
    std::set<RTT::Service *> &ancestry, unsigned int depth) const {
  if (!service || depth > 64 || !ancestry.insert(service.get()).second) {
    throw std::runtime_error(
        "RTT service graph is missing, cyclic, or too deep");
  }
  auto description = summary(name, service->doc(), path);
  std::set<std::string> allTypes;
  for (const std::string category :
       {"services", "properties", "attributes", "operations", "ports"}) {
    boost::json::array items;
    std::set<std::string> referenced;
    const auto noteType = [&](const RTT::types::TypeInfo *type) {
      referenced.insert(typeName(type));
    };
    const auto noteUnsupported = [&](boost::json::object &metadata,
                                     const std::string &href,
                                     std::string reason) {
      metadata["jsonSupported"] = reason.empty();
      metadata["reason"] =
          reason.empty() ? boost::json::value() : boost::json::value(reason);
      if (!reason.empty()) {
        publication.diagnostics.push_back(href + ": " + reason);
      }
    };
    if (category == "services") {
      for (const auto &childName : service->getProviderNames()) {
        if (childName == "this") {
          continue;
        }
        auto child = service->getService(childName);
        const auto href = childPath(path + "/services", childName);
        describeService(publication, child, childName, href, ancestry,
                        depth + 1);
        items.push_back(summary(childName, child->doc(), href));
      }
    } else if (category == "properties" || category == "attributes") {
      const auto names = category == "properties"
                             ? service->properties()->getPropertyNames()
                             : service->getAttributeNames();
      for (const auto &memberName : names) {
        const auto href = childPath(path + "/" + category, memberName);
        auto resource = std::make_shared<Resource>();
        resource->kind = ResourceKind::value;
        std::string doc;
        if (category == "properties") {
          auto *property = service->getProperty(memberName);
          if (!property) {
            throw std::runtime_error(
                "RTT property disappeared during publication");
          }
          resource->source = property->getDataSource();
          doc = property->getDescription();
        } else {
          auto *attribute = service->getValue(memberName);
          if (!attribute) {
            throw std::runtime_error(
                "RTT attribute disappeared during publication");
          }
          resource->source = attribute->getDataSource();
        }
        if (!resource->source) {
          throw std::runtime_error("RTT member has no data source");
        }
        const auto *type = resource->source->getTypeInfo();
        resource->binding = catalog_->find(type);
        resource->supported =
            resource->binding != nullptr && typeName(type) != "Void";
        const bool writable = resource->source->isAssignable();
        if (writable) {
          resource->allow = "GET, HEAD, PUT, OPTIONS";
        }
        auto metadata = summary(memberName, doc, href);
        metadata["rttType"] = typeName(type);
        metadata["writable"] = writable;
        noteType(type);
        noteUnsupported(metadata, href,
                        resource->supported ? "" : unsupported(type));
        items.push_back(std::move(metadata));
        insert(publication, href, std::move(resource));
      }
    } else if (category == "operations") {
      for (const auto &memberName : service->getOperationNames()) {
        const auto href = childPath(path + "/operations", memberName);
        auto operation = std::make_shared<Operation>();
        operation->service = service;
        operation->part = service->getOperation(memberName);
        if (!operation->part) {
          throw std::runtime_error(
              "RTT operation disappeared during publication");
        }
        auto &part = *operation->part;
        operation->implementation = part.getLocalOperation();
        const auto *local =
            dynamic_cast<const RTT::base::OperationCallerInterface *>(
                operation->implementation.get());
        operation->ownThread = local && local->getThread() == RTT::OwnThread;
        auto metadata = summary(memberName, part.description(), href);
        const auto arguments = part.getArgumentList();
        boost::json::array inputMetadata, outputMetadata;
        std::string reason;
        const auto bindingFor = [&](const RTT::types::TypeInfo *type,
                                    bool allowVoid = false) {
          noteType(type);
          const auto *binding = catalog_->find(type);
          if (!binding || (!allowVoid && typeName(type) == "Void")) {
            reason = unsupported(type);
          }
          return binding;
        };
        for (unsigned int i = 0; i < part.arity(); ++i) {
          const auto *type = part.getArgumentType(i + 1);
          operation->arguments.push_back(bindingFor(type));
          const auto argumentName =
              i < arguments.size() && !arguments[i].name.empty()
                  ? arguments[i].name
                  : "argument" + std::to_string(i + 1);
          const auto doc = i < arguments.size() ? arguments[i].description : "";
          if (!validUtf8(argumentName) || !validUtf8(doc)) {
            throw std::runtime_error("invalid UTF-8 operation metadata");
          }
          const bool output =
              i < arguments.size() && isMutableReference(arguments[i].type);
          inputMetadata.push_back({{"name", argumentName},
                                   {"description", doc},
                                   {"rttType", typeName(type)},
                                   {"mode", output ? "inout" : "in"}});
          if (output) {
            operation->outputs.push_back(i);
            outputMetadata.push_back({{"argumentIndex", i},
                                      {"name", argumentName},
                                      {"rttType", typeName(type)}});
          }
        }
        const auto *returnType = part.getArgumentType(0);
        operation->hasResult = typeName(returnType) != "Void";
        operation->result = bindingFor(returnType, true);
        metadata["result"] =
            boost::json::object{{"rttType", typeName(returnType)}};
        const auto expectedCollect =
            operation->outputs.size() + (operation->hasResult ? 1U : 0U);
        if (part.collectArity() != expectedCollect) {
          reason = "unsupported RTT operation output signature";
        }
        for (unsigned int i = 0; i < part.collectArity(); ++i) {
          const auto *type = part.getCollectType(i + 1);
          operation->collectTypes.push_back(type);
          operation->collectBindings.push_back(bindingFor(type));
          if (i < expectedCollect) {
            const auto *expected =
                operation->hasResult && i == 0
                    ? returnType
                    : part.getArgumentType(static_cast<unsigned int>(
                          operation
                              ->outputs[i - (operation->hasResult ? 1U : 0U)] +
                          1));
            if (expected != type) {
              reason = "inconsistent RTT operation output metadata";
            }
          }
        }
        metadata["arguments"] = std::move(inputMetadata);
        metadata["outputs"] = std::move(outputMetadata);
        noteUnsupported(metadata, href, reason);
        items.push_back(std::move(metadata));
        auto resource = std::make_shared<Resource>();
        resource->kind = ResourceKind::operation;
        resource->allow = "POST, OPTIONS";
        resource->supported = reason.empty();
        resource->operation = std::move(operation);
        insert(publication, href, std::move(resource));
      }
    } else {
      for (const auto &memberName : service->getPortNames()) {
        const auto base = childPath(path + "/ports", memberName);
        auto *port = service->getPort(memberName);
        if (!port) {
          throw std::runtime_error("RTT port disappeared during publication");
        }
        auto *output = dynamic_cast<RTT::base::OutputPortInterface *>(port);
        auto *input = dynamic_cast<RTT::base::InputPortInterface *>(port);
        if ((!output && !input) || (output && input)) {
          throw std::runtime_error("invalid RTT port direction");
        }
        const auto *type = port->getTypeInfo();
        const auto *binding = catalog_->find(type);
        const bool supported =
            binding && (!output || binding->codec->supportsPortValue());
        const bool retains = output != nullptr;
        auto metadata = summary(memberName, port->getDescription(), base);
        metadata.erase("href");
        metadata["rttType"] = typeName(type);
        metadata["direction"] = output ? "output" : "input";
        metadata["retainsLastSample"] =
            output ? boost::json::value(retains) : boost::json::value();
        metadata["latestHref"] = retains ? boost::json::value(base + "/latest")
                                         : boost::json::value();
        metadata["samplesHref"] = input ? boost::json::value(base + "/samples")
                                        : boost::json::value();
        noteType(type);
        noteUnsupported(metadata, base,
                        supported ? ""
                        : binding
                            ? "HTTP codec has no typed retained sample reader"
                            : unsupported(type));
        items.push_back(std::move(metadata));
        auto bridge = std::make_shared<PortBridge>();
        bridge->peer.reset(port->antiClone());
        bool connected = false;
        if (output) {
          auto *observer =
              dynamic_cast<RTT::base::InputPortInterface *>(bridge->peer.get());
          connected =
              observer &&
              output->createConnection(
                  *observer,
                  RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false));
        } else {
          auto *sender = dynamic_cast<RTT::base::OutputPortInterface *>(
              bridge->peer.get());
          connected = sender && sender->createConnection(*input);
        }
        if (!connected) {
          throw std::runtime_error(
              "failed to construct HTTP-owned RTT port connection");
        }
        auto resource = std::make_shared<Resource>();
        resource->binding = binding;
        resource->supported = supported;
        resource->output = output;
        resource->bridge = std::move(bridge);
        resource->kind = input ? ResourceKind::samples : ResourceKind::latest;
        if (input) {
          resource->allow = "POST, OPTIONS";
        }
        if (input || retains) {
          insert(publication, base + (input ? "/samples" : "/latest"),
                 std::move(resource));
        } else {
          // A non-retaining output still owns its observer for the entire
          // publication, although it deliberately has no executable route.
          publication.observers.push_back(std::move(resource->bridge));
        }
      }
    }
    sortItems(items);
    allTypes.insert(referenced.begin(), referenced.end());
    auto collection = std::make_shared<Resource>();
    collection->description =
        boost::json::object{{"items", items}, {"types", types(referenced)}};
    insert(publication, path + "/" + category, std::move(collection));
    description[category] = std::move(items);
  }
  description["types"] = types(allTypes);
  auto resource = std::make_shared<Resource>();
  resource->description = description;
  insert(publication, path, std::move(resource));
  ancestry.erase(service.get());
  return description;
}

std::shared_ptr<Publication>
ObjectModel::stage(RTT::TaskContext &component) const {
  auto publication = std::make_shared<Publication>();
  publication->component = &component;
  publication->service = component.provides();
  const auto path = childPath(std::string(root), component.getName());
  std::set<RTT::Service *> ancestry;
  describeService(*publication, publication->service, component.getName(), path,
                  ancestry, 0);
  publication->summary =
      summary(component.getName(), publication->service->doc(), path);
  std::sort(publication->diagnostics.begin(), publication->diagnostics.end());
  return publication;
}
bool ObjectModel::commit(std::shared_ptr<Publication> publication,
                         std::string *error) {
  std::lock_guard lock(mutex_);
  const auto name = publication->component->getName();
  auto [existing, inserted] = publications_.emplace(name, publication);
  if (!inserted && existing->second->component != publication->component) {
    if (error) {
      *error = "another RTT component instance has this published name";
    }
    return false;
  }
  if (error) {
    error->clear();
  }
  return true;
}
std::shared_ptr<const Resource>
ObjectModel::resolve(const std::string &path) const {
  std::lock_guard lock(mutex_);
  if (path == root) {
    boost::json::array items;
    for (const auto &[name, publication] : publications_) {
      (void)name;
      items.push_back(publication->summary);
    }
    auto resource = std::make_shared<Resource>();
    resource->description = boost::json::object{
        {"items", std::move(items)}, {"types", boost::json::object{}}};
    return resource;
  }
  for (const auto &[name, publication] : publications_) {
    (void)name;
    const auto found = publication->routes.find(path);
    if (found != publication->routes.end()) {
      return found->second;
    }
  }
  return {};
}
bool ObjectModel::contains(const RTT::TaskContext *component) const {
  std::lock_guard lock(mutex_);
  for (const auto &[name, publication] : publications_) {
    (void)name;
    if (publication->component == component) {
      return true;
    }
  }
  return false;
}
std::vector<std::string>
ObjectModel::diagnostics(const std::string &name) const {
  std::lock_guard lock(mutex_);
  const auto found = publications_.find(name);
  return found == publications_.end() ? std::vector<std::string>{}
                                      : found->second->diagnostics;
}
} // namespace RTT::http::detail
