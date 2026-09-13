// SPDX-License-Identifier: LGPL-2.1-or-later
#include <boost/serialization/array_wrapper.hpp>
#include <boost/serialization/nvp.hpp>
#include <iostream>
#include <rtt/http/reflected_codec.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/CArrayTypeInfo.hpp>
#include <rtt/types/StructTypeInfo.hpp>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Batch {
  double axes[3]{3, 4, 5};
  template <class Archive> void serialize(Archive &archive, unsigned int) {
    archive &boost::serialization::make_nvp(
        "axes", boost::serialization::make_array(axes, 3));
  }
};
} // namespace
int main() {
  try {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
    RTT::types::RealTimeTypekitPlugin().loadConstructors();
    RTT::types::Types()->addType(
        new RTT::types::CArrayTypeInfo<RTT::types::carray<double>>(
            "HttpFixedDoubles"));
    RTT::types::Types()->addType(
        new RTT::types::StructTypeInfo<Batch>("HttpBatch"));
    std::string error;
    require(RTT::http::registerCanonicalTypeProtocols(&error), error.c_str());
    RTT::http::registerReflectedTypeProtocols();
    auto catalog = RTT::http::freezeTypeCatalog(&error);
    require(bool(catalog), error.c_str());
    const auto *batch = catalog->find("HttpBatch");
    const auto *array = catalog->find("HttpFixedDoubles");
    require(batch && array, "register reflected structure and array codecs");
    RTT::http::CodecContext context;
    boost::json::value encoded(context.storage());
    RTT::http::DataSourcePtr source =
        new RTT::internal::ConstantDataSource<Batch>(Batch{});
    require(batch->codec->toJson(source, &encoded, context, nullptr),
            "readonly struct containing a nonempty carray must encode");
    require(encoded.at("axes") == boost::json::array{3.0, 4.0, 5.0},
            "whole fixed array preserves values");
    require(
        array->registration.descriptor.at("length").is_null(),
        "carray type metadata does not claim every instance has length zero");
    auto staged = array->codec->makeDataSource(boost::json::array{7, 8, 9},
                                               context, nullptr);
    require(bool(staged),
            "selected fixed array decode allocates owned sized storage");
    auto *typed =
        dynamic_cast<RTT::internal::DataSource<RTT::types::carray<double>> *>(
            staged.get());
    require(typed && typed->get().count() == 3 &&
                typed->get().address()[2] == 9,
            "decoded array owns the complete sample");
    auto whole = batch->codec->makeDataSource(
        boost::json::object{{"axes", boost::json::array{10, 11, 12}}}, context,
        nullptr);
    require(bool(whole), "whole struct decoder accepts a complete fixed array");
    auto *whole_typed =
        dynamic_cast<RTT::internal::DataSource<Batch> *>(whole.get());
    require(whole_typed && whole_typed->get().axes[2] == 12,
            "whole struct decode keeps all array elements");
    require(
        !batch->codec->makeDataSource(
            boost::json::object{{"axes", boost::json::array{1, 2}}}, context,
            nullptr),
        "wrong fixed member shape cannot silently truncate during assignment");
    require(
        !batch->codec->makeDataSource(boost::json::object{}, context, nullptr),
        "partial structure decode is rejected");
    std::cout << "Reflected fixed-array observation and owned exact-shape "
                 "decoding passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
