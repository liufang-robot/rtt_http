// SPDX-License-Identifier: LGPL-2.1-or-later
#include <boost/json.hpp>
#include <rtt/http/type_protocol.hpp>
#include <rtt/http/typed_codec.hpp>
#include <rtt/types/TypeInfo.hpp>

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <stdexcept>

namespace RTT::http {
namespace {
struct Registry {
  std::mutex mutex;
  bool frozen{false};
  std::map<RTT::types::TypeInfo *, const TypeProtocol *> registrations;
  std::shared_ptr<const TypeCatalog> catalog;
  std::string freezeError;
};
Registry &registry() {
  static Registry instance;
  return instance;
}
bool fail(std::string *error, std::string message) {
  if (error) {
    *error = std::move(message);
  }
  return false;
}
bool validName(std::string_view value) {
  return !value.empty() && value.size() <= 1024 && validUtf8(value) &&
         value.find('\0') == std::string_view::npos;
}
bool hasString(const boost::json::object &object, std::string_view key) {
  const auto *value = object.if_contains({key.data(), key.size()});
  return value && value->is_string() &&
         validName({value->as_string().data(), value->as_string().size()});
}

boost::json::value canonicalJson(const boost::json::value &value,
                                 const boost::json::storage_ptr &storage) {
  if (value.is_object()) {
    std::map<std::string, const boost::json::value *> sorted;
    for (const auto &entry : value.as_object()) {
      sorted.emplace(entry.key(), &entry.value());
    }
    boost::json::object result(storage);
    for (const auto &[key, item] : sorted) {
      result.emplace(key, canonicalJson(*item, storage));
    }
    return result;
  }
  if (value.is_array()) {
    boost::json::array result(storage);
    for (const auto &item : value.as_array()) {
      result.push_back(canonicalJson(item, storage));
    }
    return result;
  }
  return boost::json::value(value, storage);
}

std::string fingerprint(const TypeRegistration &registration) {
  CodecContext context({65536, 32, 1024 * 1024});
  std::string result;
  // Validate bounded depth/size before recursively normalizing a trusted
  // plugin's descriptor. Fingerprints are canonical identities, not code
  // hashes.
  if (!serializeJson(registration.descriptor, context, &result)) {
    throw std::invalid_argument("invalid HTTP codec descriptor");
  }
  boost::json::array identity(context.storage());
  for (const auto *part : {&registration.providerId,
                           &registration.providerVersion, &registration.codecId,
                           &registration.codecVersion, &registration.rttType}) {
    identity.emplace_back(*part);
  }
  identity.push_back(canonicalJson(registration.descriptor, context.storage()));
  boost::json::array dependencies(context.storage());
  for (const auto &name : registration.dependencies) {
    dependencies.emplace_back(name);
  }
  identity.push_back(std::move(dependencies));
  if (!serializeJson(identity, context, &result)) {
    throw std::invalid_argument("invalid HTTP codec registration");
  }
  return result;
}

bool validateRegistration(const TypeRegistration &registration,
                          std::string *error) {
  for (const auto *part : {&registration.providerId,
                           &registration.providerVersion, &registration.codecId,
                           &registration.codecVersion, &registration.rttType}) {
    if (!validName(*part)) {
      return fail(error, "invalid HTTP codec identity");
    }
  }
  const auto &descriptor = registration.descriptor;
  if (!hasString(descriptor, "kind")) {
    return fail(error, "missing HTTP descriptor kind");
  }
  const auto &kind = descriptor.at("kind").as_string();
  std::set<std::string> references;
  const auto addReference = [&](const boost::json::object &object,
                                const char *field) {
    if (!hasString(object, field)) {
      return false;
    }
    references.emplace(object.at(field).as_string());
    return true;
  };
  if (kind == "integer" || kind == "float") {
    const auto *bits = descriptor.if_contains("bits");
    if (!bits || !bits->is_int64() ||
        (bits->as_int64() != 8 && bits->as_int64() != 16 &&
         bits->as_int64() != 32 && bits->as_int64() != 64)) {
      return fail(error, "invalid HTTP numeric descriptor width");
    }
    if (kind == "integer") {
      const auto *sign = descriptor.if_contains("signed");
      const auto *encoding = descriptor.if_contains("encoding");
      if (!sign || !sign->is_bool() || !encoding || !encoding->is_string() ||
          encoding->as_string() !=
              (bits->as_int64() == 64 ? "decimalString" : "number")) {
        return fail(error, "invalid HTTP integer descriptor");
      }
    } else {
      const auto *special = descriptor.if_contains("nonFinite");
      if ((bits->as_int64() != 32 && bits->as_int64() != 64) || !special ||
          *special != boost::json::array{"NaN", "Infinity", "-Infinity"}) {
        return fail(error, "invalid HTTP floating-point descriptor");
      }
    }
  } else if (kind == "string") {
    const auto *encoding = descriptor.if_contains("encoding");
    if (!encoding || *encoding != "utf8") {
      return fail(error, "invalid HTTP string descriptor");
    }
  } else if (kind == "enum") {
    if (!addReference(descriptor, "underlyingType")) {
      return fail(error, "invalid HTTP enum descriptor");
    }
  } else if (kind == "array") {
    const auto *length = descriptor.if_contains("length");
    if (!addReference(descriptor, "elementType") || !length ||
        !(length->is_null() ||
          (length->is_int64() && length->as_int64() >= 0))) {
      return fail(error, "invalid HTTP array descriptor");
    }
  } else if (kind == "object") {
    const auto *fields = descriptor.if_contains("fields");
    if (!fields || !fields->is_array()) {
      return fail(error, "invalid HTTP object descriptor");
    }
    std::set<std::string> names;
    for (const auto &field : fields->as_array()) {
      if (!field.is_object() || !hasString(field.as_object(), "name") ||
          !addReference(field.as_object(), "rttType") ||
          !names.emplace(field.as_object().at("name").as_string()).second) {
        return fail(error, "invalid HTTP object field descriptor");
      }
    }
  } else if (kind != "boolean" && kind != "void") {
    return fail(error, "unsupported HTTP descriptor kind");
  }
  const std::set<std::string> dependencies(registration.dependencies.begin(),
                                           registration.dependencies.end());
  if (references != dependencies) {
    return fail(error, "HTTP descriptor dependencies do not match references");
  }
  return true;
}
} // namespace

bool accountJsonString(std::string_view value, CodecContext &context,
                       CodecError *error) {
  if (!validUtf8(value)) {
    return codecFailure(error, CodecErrorCode::invalid_utf8);
  }
  if (!context.account(2, error)) {
    return false;
  }
  for (const unsigned char character : value) {
    const std::size_t size = character == '"' || character == '\\' ? 2
                             : character < 0x20                    ? 6
                                                                   : 1;
    if (!context.account(size, error)) {
      return false;
    }
  }
  return true;
}

TypeProtocol::TypeProtocol(TypeRegistration registration,
                           std::shared_ptr<const TypeCodec> codec)
    : registration_(std::move(registration)), codec_(std::move(codec)) {
  std::sort(registration_.dependencies.begin(),
            registration_.dependencies.end());
  registration_.dependencies.erase(
      std::unique(registration_.dependencies.begin(),
                  registration_.dependencies.end()),
      registration_.dependencies.end());
  registration_.descriptor["jsonSupported"] = true;
  registration_.descriptor["reason"] = nullptr;
  fingerprint_ = ::RTT::http::fingerprint(registration_);
}
TypeProtocol::~TypeProtocol() = default;
const TypeRegistration &TypeProtocol::registration() const noexcept {
  return registration_;
}
const std::string &TypeProtocol::fingerprint() const noexcept {
  return fingerprint_;
}
const std::shared_ptr<const TypeCodec> &TypeProtocol::codec() const noexcept {
  return codec_;
}
RTT::base::ChannelElementBase::shared_ptr
TypeProtocol::createStream(RTT::base::PortInterface *, const RTT::ConnPolicy &,
                           bool) const {
  return {};
}

bool registerTypeProtocol(RTT::types::TypeInfo *type,
                          std::unique_ptr<TypeProtocol> protocol,
                          std::string *error) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (!type || !protocol || !protocol->codec() ||
      type->getTypeName() != protocol->registration().rttType) {
    return fail(error, "invalid HTTP type protocol registration");
  }
  if (!validateRegistration(protocol->registration(), error)) {
    return false;
  }
  if (type->hasProtocol(kTransportProtocolId)) {
    const auto *existing = dynamic_cast<const TypeProtocol *>(
        type->getProtocol(kTransportProtocolId));
    const auto known = state.registrations.find(type);
    if (!existing || existing->fingerprint() != protocol->fingerprint() ||
        known == state.registrations.end() || known->second != existing) {
      return fail(error, "conflicting HTTP type protocol registration: " +
                             type->getTypeName());
    }
    if (error) {
      error->clear();
    }
    return true;
  }
  if (state.frozen) {
    return fail(error, "HTTP type catalog is frozen: " + type->getTypeName());
  }
  // Allocate the registry entry before transferring ownership to RTT.
  auto [entry, inserted] = state.registrations.emplace(type, protocol.get());
  if (!inserted) {
    return fail(error,
                "HTTP protocol slot changed outside the registration API");
  }
  try {
    if (!type->addProtocol(kTransportProtocolId, protocol.get())) {
      state.registrations.erase(entry);
      return fail(error, "RTT rejected HTTP type protocol registration");
    }
  } catch (...) {
    state.registrations.erase(entry);
    throw;
  }
  protocol.release();
  if (error) {
    error->clear();
  }
  return true;
}

bool typeCatalogFrozen() {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  return state.frozen;
}

std::shared_ptr<const TypeCodec>
registeredTypeCodec(const RTT::types::TypeInfo *type) {
  if (!type) {
    return {};
  }
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  const auto found =
      state.registrations.find(const_cast<RTT::types::TypeInfo *>(type));
  if (found == state.registrations.end() ||
      type->getProtocol(kTransportProtocolId) != found->second) {
    return {};
  }
  return found->second->codec();
}

std::shared_ptr<const TypeCatalog> freezeTypeCatalog(std::string *error) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (state.frozen) {
    if (error) {
      *error = state.freezeError;
    }
    return state.catalog;
  }
  state.frozen = true;
  state.freezeError = "HTTP type catalog could not be frozen";
  auto staged = std::make_shared<TypeCatalog>();
  for (const auto &[type, protocol] : state.registrations) {
    if (type->getProtocol(kTransportProtocolId) != protocol) {
      fail(error,
           state.freezeError =
               "HTTP type protocol slot changed outside the registration API");
      return {};
    }
    staged->types_.emplace(
        type->getTypeName(),
        TypeBinding{protocol->registration(), protocol->codec()});
    staged->identities_.emplace(type, type->getTypeName());
  }
  // Iterative DFS makes cycle checks independent of the C++ call stack.
  std::map<std::string, int> marks;
  for (const auto &[name, binding] : staged->types_) {
    (void)binding;
    if (marks[name] == 2) {
      continue;
    }
    std::vector<std::pair<std::string, std::size_t>> stack{{name, 0}};
    marks[name] = 1;
    while (!stack.empty()) {
      auto &[current, next] = stack.back();
      const auto &definition = staged->types_.at(current).registration;
      if (next == definition.dependencies.size()) {
        marks[current] = 2;
        stack.pop_back();
        continue;
      }
      const auto &dependency = definition.dependencies[next++];
      const auto referenced = staged->types_.find(dependency);
      if (referenced == staged->types_.end()) {
        fail(error,
             state.freezeError = "missing HTTP type dependency: " + dependency);
        return {};
      }
      if (definition.descriptor.at("kind") == "enum" &&
          referenced->second.registration.descriptor.at("kind") != "integer") {
        fail(error,
             state.freezeError = "HTTP enum underlying type is not an integer");
        return {};
      }
      if (marks[dependency] == 1) {
        fail(error, state.freezeError =
                        "recursive HTTP type definition: " + dependency);
        return {};
      }
      if (marks[dependency] == 0) {
        marks[dependency] = 1;
        stack.emplace_back(dependency, 0);
      }
    }
  }
  state.catalog = staged;
  state.freezeError.clear();
  if (error) {
    error->clear();
  }
  return state.catalog;
}

const TypeBinding *
TypeCatalog::find(const RTT::types::TypeInfo *type) const noexcept {
  const auto found = identities_.find(type);
  return found == identities_.end() ? nullptr : find(found->second);
}
const TypeBinding *TypeCatalog::find(const std::string &name) const noexcept {
  const auto found = types_.find(name);
  return found == types_.end() ? nullptr : &found->second;
}
const std::map<std::string, TypeBinding> &TypeCatalog::types() const noexcept {
  return types_;
}

} // namespace RTT::http
