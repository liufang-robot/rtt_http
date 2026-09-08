// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <functional>
#include <rtt/http/typed_codec.hpp>
#include <rtt/types/TypeInfo.hpp>

namespace RTT::http {
using RetainedSampleReader = std::function<PortValueStatus(
    const RTT::base::OutputPortInterface *, DataSourcePtr *, CodecError *)>;

// Discover a structure/sequence from a fresh RTT value, never from live
// component data. Register dependency codecs first. Factory failure explains
// why the available RTT metadata cannot provide a complete JSON mapping.
// RTT transport callbacks hold the RTT type repository lock. Reflection and
// even typed DataSource metadata may acquire that lock: prepare these codecs
// outside registerTransport(), after importing ordinary typekits, and make
// callbacks use their supplied TypeInfo pointer and the prepared registration.
// Plugin metadata getters must not perform registration. Once a DSO has
// registered callbacks, retain it even if another codec cannot be prepared;
// do not throw out of its load entry point and let the loader unload it.
RTT_HTTP_API std::unique_ptr<TypeProtocol>
makeReflectedTypeProtocol(RTT::types::TypeInfo *type, TypeRegistration identity,
                          RetainedSampleReader reader = {},
                          std::string *error = nullptr);

template <typename T>
std::unique_ptr<TypeProtocol>
makeReflectedTypeProtocol(RTT::types::TypeInfo *type, TypeRegistration identity,
                          std::string *error = nullptr) {
  const auto sample = type ? type->buildValue() : DataSourcePtr{};
  if (!dynamic_cast<RTT::internal::DataSource<T> *>(sample.get())) {
    if (error) {
      *error = "reflected HTTP codec C++ type does not match RTT metadata";
    }
    return {};
  }
  return makeReflectedTypeProtocol(
      type, std::move(identity),
      [](const RTT::base::OutputPortInterface *port, DataSourcePtr *result,
         CodecError *failure) {
        const auto *typed = dynamic_cast<const RTT::OutputPort<T> *>(port);
        if (!typed || !result) {
          codecFailure(failure, CodecErrorCode::invalid_data_source);
          return PortValueStatus::error;
        }
        T value{};
        if (!typed->getLastWrittenValue(value)) {
          return PortValueStatus::waiting_for_initial_data;
        }
        // Use the reference assignment path to preserve every string byte.
        typename RTT::internal::ValueDataSource<T>::shared_ptr staged =
            new RTT::internal::ValueDataSource<T>();
        staged->set() = std::move(value);
        *result = staged;
        return PortValueStatus::value;
      },
      error);
}

// Best-effort fallback before the first freeze. Compatible explicit protocols
// take precedence. Types without complete finite reflection remain unsupported.
// Return one stable diagnostic per unsupported type; do not log on every GET.
RTT_HTTP_API std::map<std::string, std::string>
registerReflectedTypeProtocols();

} // namespace RTT::http
