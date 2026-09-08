// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <boost/json/object.hpp>
#include <rtt/base/DataSourceBase.hpp>
#include <rtt/http/codec_context.hpp>
#include <rtt/types/TypeTransporter.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace RTT::base {
class OutputPortInterface;
}
namespace RTT::types {
class TypeInfo;
}

namespace RTT::http {
// Project-local allocation, separate from OPC UA (1042), Typelib (42),
// mqueue (2), and CORBA (1). Registration checks for incompatible occupants.
inline constexpr int kTransportProtocolId = 1043;
using DataSourcePtr = RTT::base::DataSourceBase::shared_ptr;
enum class PortValueStatus { value, waiting_for_initial_data, error };

class RTT_HTTP_API TypeCodec {
public:
  virtual ~TypeCodec() = default;
  virtual bool toJson(const DataSourcePtr &source, boost::json::value *result,
                      CodecContext &context, CodecError *error) const = 0;
  virtual DataSourcePtr makeDataSource(const boost::json::value &value,
                                       CodecContext &context,
                                       CodecError *error) const = 0;
  virtual bool assign(const DataSourcePtr &staged,
                      const DataSourcePtr &destination,
                      CodecError *error) const = 0;
  virtual PortValueStatus portValue(const RTT::base::OutputPortInterface *port,
                                    boost::json::value *result,
                                    CodecContext &context,
                                    CodecError *error) const = 0;
  // Reflection alone cannot query typed RTT retained-sample availability.
  virtual bool supportsPortValue() const noexcept { return true; }
};

struct TypeRegistration {
  std::string providerId;
  std::string providerVersion;
  std::string codecId;
  std::string codecVersion;
  std::string rttType;
  boost::json::object descriptor;
  std::vector<std::string> dependencies;
};

// RTT owns registered protocols. The immutable shared codec and definition
// can also be retained by publication models until deployment teardown.
class RTT_HTTP_API TypeProtocol : public RTT::types::TypeTransporter {
public:
  TypeProtocol(TypeRegistration registration,
               std::shared_ptr<const TypeCodec> codec);
  ~TypeProtocol() override;
  const TypeRegistration &registration() const noexcept;
  const std::string &fingerprint() const noexcept;
  const std::shared_ptr<const TypeCodec> &codec() const noexcept;
  RTT::base::ChannelElementBase::shared_ptr
  createStream(RTT::base::PortInterface *, const RTT::ConnPolicy &,
               bool) const override;

private:
  TypeRegistration registration_;
  std::string fingerprint_;
  std::shared_ptr<const TypeCodec> codec_;
};

struct TypeBinding {
  TypeRegistration registration;
  std::shared_ptr<const TypeCodec> codec;
};

class RTT_HTTP_API TypeCatalog {
public:
  const TypeBinding *find(const RTT::types::TypeInfo *type) const noexcept;
  const TypeBinding *find(const std::string &canonicalName) const noexcept;
  const std::map<std::string, TypeBinding> &types() const noexcept;

private:
  friend RTT_HTTP_API std::shared_ptr<const TypeCatalog>
  freezeTypeCatalog(std::string *);
  std::map<std::string, TypeBinding> types_;
  std::map<const RTT::types::TypeInfo *, std::string> identities_;
};

RTT_HTTP_API bool registerTypeProtocol(RTT::types::TypeInfo *type,
                                       std::unique_ptr<TypeProtocol> protocol,
                                       std::string *error = nullptr);
RTT_HTTP_API bool registerCanonicalTypeProtocol(std::string_view typeName,
                                                RTT::types::TypeInfo *type,
                                                std::string *error = nullptr);
RTT_HTTP_API bool registerCanonicalTypeProtocols(std::string *error = nullptr);
RTT_HTTP_API bool typeCatalogFrozen();
RTT_HTTP_API std::shared_ptr<const TypeCodec>
registeredTypeCodec(const RTT::types::TypeInfo *type);
RTT_HTTP_API std::shared_ptr<const TypeCatalog>
freezeTypeCatalog(std::string *error = nullptr);

} // namespace RTT::http
