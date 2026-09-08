// SPDX-License-Identifier: LGPL-2.1-or-later
#include <rtt/FlowStatus.hpp>
#include <rtt/base/TaskCore.hpp>
#include <rtt/http/typed_codec.hpp>
#include <rtt/rt_string.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

namespace RTT::http {
namespace {
template <typename T> boost::json::object integerDescriptor() {
  return {{"kind", "integer"},
          {"bits", static_cast<int>(sizeof(T) * 8)},
          {"signed", std::is_signed_v<T>},
          {"encoding", sizeof(T) == 8 ? "decimalString" : "number"}};
}
template <typename T> boost::json::object floatDescriptor() {
  return {{"kind", "float"},
          {"bits", static_cast<int>(sizeof(T) * 8)},
          {"nonFinite", boost::json::array{"NaN", "Infinity", "-Infinity"}}};
}

template <typename T, typename Conversion = JsonConversion<T>>
std::unique_ptr<TypeProtocol>
makeProtocol(std::string_view name, boost::json::object descriptor,
             std::vector<std::string> dependencies = {}) {
  return std::make_unique<TypeProtocol>(
      TypeRegistration{"orocos.rtt.http", "1", std::string(name), "1",
                       std::string(name), std::move(descriptor),
                       std::move(dependencies)},
      std::make_shared<TypedTypeCodec<T, Conversion>>());
}
class VoidCodec final : public TypeCodec {
public:
  bool toJson(const DataSourcePtr &, boost::json::value *result,
              CodecContext &context, CodecError *error) const override {
    if (!result || !context.account(4, error)) {
      return false;
    }
    *result = nullptr;
    return true;
  }
  DataSourcePtr makeDataSource(const boost::json::value &, CodecContext &,
                               CodecError *error) const override {
    codecFailure(error, CodecErrorCode::type_mismatch);
    return {};
  }
  bool assign(const DataSourcePtr &, const DataSourcePtr &,
              CodecError *error) const override {
    return codecFailure(error, CodecErrorCode::type_mismatch);
  }
  PortValueStatus portValue(const RTT::base::OutputPortInterface *,
                            boost::json::value *, CodecContext &,
                            CodecError *error) const override {
    codecFailure(error, CodecErrorCode::unavailable);
    return PortValueStatus::error;
  }
  bool supportsPortValue() const noexcept override { return false; }
};

std::unique_ptr<TypeProtocol> canonicalProtocol(std::string_view name) {
  if (name == "Bool") {
    return makeProtocol<bool>(name, {{"kind", "boolean"}});
  }
  if (name == "Int8") {
    return makeProtocol<std::int8_t>(name, integerDescriptor<std::int8_t>());
  }
  if (name == "UInt8") {
    return makeProtocol<std::uint8_t>(name, integerDescriptor<std::uint8_t>());
  }
  if (name == "Int16") {
    return makeProtocol<std::int16_t>(name, integerDescriptor<std::int16_t>());
  }
  if (name == "UInt16") {
    return makeProtocol<std::uint16_t>(name,
                                       integerDescriptor<std::uint16_t>());
  }
  if (name == "Int32") {
    return makeProtocol<std::int32_t>(name, integerDescriptor<std::int32_t>());
  }
  if (name == "UInt32") {
    return makeProtocol<std::uint32_t>(name,
                                       integerDescriptor<std::uint32_t>());
  }
  if (name == "Int64") {
    return makeProtocol<std::int64_t>(name, integerDescriptor<std::int64_t>());
  }
  if (name == "UInt64") {
    return makeProtocol<std::uint64_t>(name,
                                       integerDescriptor<std::uint64_t>());
  }
  if (name == "Char") {
    return makeProtocol<char>(name, integerDescriptor<char>());
  }
  if (name == "Float32") {
    return makeProtocol<float>(name, floatDescriptor<float>());
  }
  if (name == "Float64") {
    return makeProtocol<double>(name, floatDescriptor<double>());
  }
  if (name == "String") {
    return makeProtocol<std::string>(
        name, {{"kind", "string"}, {"encoding", "utf8"}});
  }
#ifdef OS_RT_MALLOC
  if (name == "RtString") {
    return makeProtocol<RTT::rt_string>(
        name, {{"kind", "string"}, {"encoding", "utf8"}});
  }
#endif
  if (name == "Float64Array") {
    return makeProtocol<std::vector<double>>(
        name,
        {{"kind", "array"}, {"elementType", "Float64"}, {"length", nullptr}},
        {"Float64"});
  }
  if (name == "Int32Array") {
    return makeProtocol<std::vector<std::int32_t>>(
        name,
        {{"kind", "array"}, {"elementType", "Int32"}, {"length", nullptr}},
        {"Int32"});
  }
  if (name == "StringArray") {
    return makeProtocol<std::vector<std::string>>(
        name,
        {{"kind", "array"}, {"elementType", "String"}, {"length", nullptr}},
        {"String"});
  }
  if (name == "FlowStatus") {
    return makeProtocol<RTT::FlowStatus,
                        EnumJsonConversion<RTT::FlowStatus, std::int32_t>>(
        name, {{"kind", "enum"}, {"underlyingType", "Int32"}}, {"Int32"});
  }
  if (name == "WriteStatus") {
    return makeProtocol<RTT::WriteStatus,
                        EnumJsonConversion<RTT::WriteStatus, std::int32_t>>(
        name, {{"kind", "enum"}, {"underlyingType", "Int32"}}, {"Int32"});
  }
  if (name == "TaskState") {
    using State = RTT::base::TaskCore::TaskState;
    return makeProtocol<State, EnumJsonConversion<State, std::int32_t>>(
        name, {{"kind", "enum"}, {"underlyingType", "Int32"}}, {"Int32"});
  }
  if (name == "Void") {
    return std::make_unique<TypeProtocol>(TypeRegistration{"orocos.rtt.http",
                                                           "1",
                                                           "Void",
                                                           "1",
                                                           "Void",
                                                           {{"kind", "void"}},
                                                           {}},
                                          std::make_shared<VoidCodec>());
  }
  return {};
}
} // namespace

bool registerCanonicalTypeProtocol(std::string_view name,
                                   RTT::types::TypeInfo *type,
                                   std::string *error) {
  auto protocol = canonicalProtocol(name);
  if (!protocol) {
    if (error) {
      *error = "no canonical HTTP protocol for RTT type: " + std::string(name);
    }
    return false;
  }
  return registerTypeProtocol(type, std::move(protocol), error);
}
bool registerCanonicalTypeProtocols(std::string *error) {
  for (const auto *name : {"Bool",
                           "Int8",
                           "UInt8",
                           "Int16",
                           "UInt16",
                           "Int32",
                           "UInt32",
                           "Int64",
                           "UInt64",
                           "Char",
                           "Float32",
                           "Float64",
                           "String",
                           "Float64Array",
                           "Int32Array",
                           "StringArray",
                           "FlowStatus",
                           "WriteStatus",
                           "TaskState",
                           "Void"
#ifdef OS_RT_MALLOC
                           ,
                           "RtString"
#endif
       }) {
    if (!registerCanonicalTypeProtocol(name, RTT::types::Types()->type(name),
                                       error)) {
      return false;
    }
  }
  return true;
}
} // namespace RTT::http
