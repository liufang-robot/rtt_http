#include <rtt/internal/PortDataAccess.hpp>
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <rtt/InputPort.hpp>
#include <rtt/http/typed_codec.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/TypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <iostream>
#include <stdexcept>

using namespace RTT::http;
void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
template <typename T>
T decode(const TypeCodec &codec, const boost::json::value &json) {
  CodecContext context;
  CodecError error;
  auto value = codec.makeDataSource(json, context, &error);
  const auto *typed =
      dynamic_cast<const RTT::internal::DataSource<T> *>(value.get());
  require(typed != nullptr, "decode creates owned typed storage");
  return typed->get();
}
template <typename T>
boost::json::value encode(const TypeCodec &codec, const T &value) {
  DataSourcePtr source = new RTT::internal::ConstantDataSource<T>(value);
  CodecContext context;
  boost::json::value output(context.storage());
  require(codec.toJson(source, &output, context, nullptr), "encode succeeds");
  return output;
}
void rejected(const TypeCodec &codec, const boost::json::value &value) {
  CodecContext context;
  CodecError error;
  require(!codec.makeDataSource(value, context, &error) &&
              error.code == CodecErrorCode::type_mismatch,
          "invalid value must fail whole conversion");
}

enum class WideMode : std::uint64_t { maximum = UINT64_MAX };
class ForeignProtocol final : public RTT::types::TypeTransporter {
  RTT::base::ChannelElementBase::shared_ptr
  createStream(RTT::base::PortInterface *, const RTT::ConnPolicy &,
               bool) const override {
    return {};
  }
};

int main(int argc, char **argv) {
  try {
    if (!RTT::types::Types()->type("Int32")) {
      RTT::types::RealTimeTypekitPlugin().loadTypes();
    }
    std::string error;
    require(registerCanonicalTypeProtocols(&error), error);
    RTT::types::Types()->type("Int32")->addAlias("FixtureIntegerAlias");
    RTT::types::Types()->addType(
        new RTT::types::TemplateTypeInfo<WideMode, false>("FixtureMode"));
    auto *mode_type = RTT::types::Types()->type("FixtureMode");
    const std::string test = argc > 1 ? argv[1] : "values";
    const std::string dependency = test == "missing_dependency" ? "Unknown"
                                   : test == "cycle"            ? "FixtureMode"
                                                                : "UInt64";
    const TypeRegistration identity{
        "fixture",     "1",
        "mode",        "1",
        "FixtureMode", {{"kind", "enum"}, {"underlyingType", dependency}},
        {dependency}};
    auto codec = std::make_shared<TypedTypeCodec<WideMode>>();
    require(
        registerTypeProtocol(
            mode_type, std::make_unique<TypeProtocol>(identity, codec), &error),
        error);
    require(
        registerTypeProtocol(
            mode_type, std::make_unique<TypeProtocol>(identity, codec), &error),
        "same registration is an idempotent no-op");
    auto changed = identity;
    changed.codecVersion = "2";
    require(
        !registerTypeProtocol(
            mode_type, std::make_unique<TypeProtocol>(changed, codec), &error),
        "different codec identity must not replace existing registration");

    RTT::types::TypeInfo occupied("Occupied");
    occupied.addProtocol(kTransportProtocolId, new ForeignProtocol());
    auto conflict = identity;
    conflict.rttType = "Occupied";
    require(!registerTypeProtocol(
                &occupied, std::make_unique<TypeProtocol>(conflict, codec),
                &error) &&
                dynamic_cast<ForeignProtocol *>(
                    occupied.getProtocol(kTransportProtocolId)),
            "an incompatible RTT transport remains intact");

    const auto catalog = freezeTypeCatalog(&error);
    if (test != "values") {
      require(!catalog && !error.empty() && typeCatalogFrozen(),
              "invalid dependency graph rejects freeze");
      require(!freezeTypeCatalog(&error),
              "failed freeze does not accept later mutation or retry a "
              "different catalog");
      std::cout << test << " rejected\n";
      return 0;
    }
    require(catalog != nullptr, error);
    require(registerCanonicalTypeProtocols(&error),
            "identical canonical registrations remain valid after freeze");
    require(catalog == freezeTypeCatalog(&error),
            "freeze returns the same immutable catalog");
    require(catalog->find(RTT::types::Types()->type("FixtureIntegerAlias")) ==
                catalog->find("Int32"),
            "RTT aliases share one canonical binding");
    RTT::types::TypeInfo late("Late");
    changed.rttType = "Late";
    require(!registerTypeProtocol(
                &late, std::make_unique<TypeProtocol>(changed, codec), &error),
            "new registration after freeze is rejected");

    const auto &i64 = *catalog->find("Int64")->codec;
    const auto &u64 = *catalog->find("UInt64")->codec;
    require(encode(i64, INT64_MIN) == "-9223372036854775808" &&
                decode<std::int64_t>(i64, "9223372036854775807") == INT64_MAX &&
                encode(u64, UINT64_MAX) == "18446744073709551615" &&
                decode<std::uint64_t>(u64, "18446744073709551615") ==
                    UINT64_MAX,
            "64-bit boundaries retain exact browser values");
    for (const auto *value :
         {"-0", "+1", "01", " 1", "1 ", "1.0", "1e2", "9223372036854775808"}) {
      rejected(i64, value);
    }
    rejected(i64, 42);
    rejected(u64, "-1");
    rejected(u64, "18446744073709551616");
    require(encode(*codec, WideMode::maximum) == "18446744073709551615" &&
                decode<WideMode>(*codec, "18446744073709551615") ==
                    WideMode::maximum,
            "wide enums use the underlying integer representation");

    const auto &small = *catalog->find("UInt8")->codec;
    require(decode<std::uint8_t>(small, 255.0) == 255,
            "integral JSON numbers accepted");
    for (const auto &value :
         {boost::json::value(-1), boost::json::value(256),
          boost::json::value(1.5), boost::json::value(true),
          boost::json::value(nullptr), boost::json::value("1")}) {
      rejected(small, value);
    }
    rejected(*catalog->find("Bool")->codec, 1);
    const auto &float32 = *catalog->find("Float32")->codec;
    rejected(float32, 1e40);
    require(std::isnan(decode<float>(float32, "NaN")) &&
                std::isinf(decode<float>(float32, "Infinity")) &&
                encode(float32, -std::numeric_limits<float>::infinity()) ==
                    "-Infinity",
            "non-finite floats use exact agreed spellings");
    rejected(float32, "nan");

    const auto &strings = *catalog->find("String")->codec;
    const std::string nul("a\0b", 3);
    require(decode<std::string>(strings, encode(strings, nul)) == nul,
            "embedded NUL is preserved in values");
    CodecContext string_context;
    auto staged_string = strings.makeDataSource(boost::json::string(nul),
                                                string_context, nullptr);
    DataSourcePtr cloned_string = staged_string->clone();
    DataSourcePtr string_destination =
        new RTT::internal::ValueDataSource<std::string>("old");
    require(strings.assign(cloned_string, string_destination, nullptr) &&
                dynamic_cast<RTT::internal::DataSource<std::string> *>(
                    string_destination.get())
                        ->get() == nul,
            "RTT invocation copies and assignment preserve embedded NUL");
    const auto &array = *catalog->find("Int32Array")->codec;
    DataSourcePtr target =
        new RTT::internal::ValueDataSource<std::vector<std::int32_t>>({7, 8});
    CodecContext context;
    CodecError conversion_error;
    require(!array.makeDataSource(boost::json::array{1, "wrong", 3}, context,
                                  &conversion_error),
            "array validates every element before assignment");
    require(
        dynamic_cast<RTT::internal::DataSource<std::vector<std::int32_t>> *>(
            target.get())
                ->get() == std::vector<std::int32_t>({7, 8}),
        "invalid whole replacement leaves value unchanged");
    auto replacement =
        array.makeDataSource(boost::json::array{1, 2, 3}, context, nullptr);
    require(array.assign(replacement, target, nullptr),
            "valid whole array assignment succeeds");
    DataSourcePtr immutable =
        new RTT::internal::ConstantDataSource<std::vector<std::int32_t>>({9});
    require(!array.assign(replacement, immutable, nullptr),
            "constant assignment is rejected");

    TypedTypeCodec<std::array<std::uint8_t, 3>> fixed;
    rejected(fixed, boost::json::array{1, 2});
    rejected(fixed, boost::json::array{1, 256, 3});
    require(decode<std::array<std::uint8_t, 3>>(
                fixed, boost::json::array{0, 255, 7}) ==
                std::array<std::uint8_t, 3>{0, 255, 7},
            "fixed byte array is a numeric array");

    RTT::OutputPort<std::int32_t> output("sample");
    RTT::InputPort<std::int32_t> observer("observer");
    output.createConnection(
        observer, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false));
    boost::json::value sample(context.storage());
    const auto &integer = *catalog->find("Int32")->codec;
    require(integer.portValue(&output, &sample, context, nullptr) ==
                PortValueStatus::waiting_for_initial_data,
            "no invented initial sample");
    RTT::internal::PortDataAccess::publish(output, 23);
    require(integer.portValue(&output, &sample, context, nullptr) ==
                    PortValueStatus::value &&
                sample == 23,
            "read actual retained RTT sample");
    require(integer.portValue(&output, &sample, context, nullptr) ==
                    PortValueStatus::value &&
                sample == 23,
            "repeated retained reads are non-consuming");
    std::int32_t observed{};
    require(RTT::internal::PortDataAccess::receive(observer, observed) == RTT::NewData && observed == 23,
            "HTTP retained reads do not consume another reader's sample");
    std::cout << "RTT JSON fidelity, whole assignment, retained samples, and "
                 "registry contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
