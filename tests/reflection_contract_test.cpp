// SPDX-License-Identifier: LGPL-2.1-or-later
#include <rtt/http/reflected_codec.hpp>
#include <rtt/os/main.h>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/SequenceTypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace RTT::http;

namespace {
void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class ThrowingSequenceTypeInfo final
    : public RTT::types::SequenceTypeInfo<std::vector<std::uint16_t>> {
public:
  ThrowingSequenceTypeInfo() : SequenceTypeInfo("ThrowingSequence") {}

  bool resize(DataSourcePtr, int) const override {
    throw std::invalid_argument("resize probe rejected");
  }
};
} // namespace

int ORO_main(int, char **) {
  try {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
    std::string error;
    require(registerCanonicalTypeProtocols(&error), error);
    RTT::types::Types()->addType(new ThrowingSequenceTypeInfo);
    RTT::types::Types()->addType(
        new RTT::types::SequenceTypeInfo<std::vector<std::uint8_t>>(
            "HealthyReflectedSequence"));
    auto *throwing = RTT::types::Types()->type("ThrowingSequence");
    const TypeRegistration identity{"fixture", "1", "throwing", "1",
                                    "ThrowingSequence", {}, {}};
    require(!makeReflectedTypeProtocol(throwing, identity, {}, &error) &&
                error.find("resize probe rejected") != std::string::npos,
            "a throwing metadata probe returns a diagnostic");

    const auto diagnostics = registerReflectedTypeProtocols();
    require(diagnostics.contains("ThrowingSequence") &&
                diagnostics.at("ThrowingSequence").find(
                    "resize probe rejected") != std::string::npos &&
                !registeredTypeCodec(throwing),
            "the unsupported type keeps its diagnostic without a codec");
    const auto catalog = freezeTypeCatalog(&error);
    require(catalog != nullptr, error);
    const auto *healthy = catalog->find("HealthyReflectedSequence");
    require(healthy != nullptr && catalog->find("UInt8") != nullptr,
            "other reflected and explicit codecs remain available");
    CodecContext context;
    const boost::json::value expected = boost::json::array{4, 5};
    const auto value = healthy->codec->makeDataSource(expected, context, nullptr);
    boost::json::value actual(context.storage());
    require(value && healthy->codec->toJson(value, &actual, context, nullptr) &&
                actual == expected,
            "a healthy reflected codec still round-trips values");
    std::cout << "Throwing reflection probes leave other codecs usable\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
