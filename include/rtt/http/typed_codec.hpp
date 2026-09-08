// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <boost/json.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/http/type_protocol.hpp>
#include <rtt/internal/DataSources.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace RTT::http {

RTT_HTTP_API bool accountJsonString(std::string_view value,
                                    CodecContext &context, CodecError *error);

// Custom transports may specialize this converter or supply another converter
// to TypedTypeCodec. decode writes only caller-owned staging storage.
template <typename T, typename Enable = void> struct JsonConversion;

template <typename T>
struct JsonConversion<
    T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
  static bool encode(T value, boost::json::value *output, CodecContext &context,
                     CodecError *error) {
    char buffer[32];
    const auto converted =
        std::to_chars(buffer, buffer + sizeof(buffer), value);
    const auto length = static_cast<std::size_t>(converted.ptr - buffer);
    if (!context.account(length + (sizeof(T) == 8 ? 2 : 0), error)) {
      return false;
    }
    if constexpr (sizeof(T) == 8) {
      *output = boost::json::string_view(buffer, length);
    } else if constexpr (std::is_signed_v<T>) {
      *output = static_cast<std::int64_t>(value);
    } else {
      *output = static_cast<std::uint64_t>(value);
    }
    return true;
  }
  static bool decode(const boost::json::value &input, T *output, CodecContext &,
                     CodecError *error) {
    if constexpr (sizeof(T) == 8) {
      if (!input.is_string()) {
        return codecFailure(error, CodecErrorCode::type_mismatch);
      }
      const auto &text = input.as_string();
      T staged{};
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), staged);
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) {
        return codecFailure(error, CodecErrorCode::type_mismatch);
      }
      char canonical[32];
      const auto encoded =
          std::to_chars(canonical, canonical + sizeof(canonical), staged);
      if (boost::json::string_view(canonical, encoded.ptr - canonical) !=
          text) {
        return codecFailure(error, CodecErrorCode::type_mismatch);
      }
      *output = staged;
      return true;
    } else {
      if (input.is_int64()) {
        const auto value = input.as_int64();
        if constexpr (std::is_signed_v<T>) {
          if (value < (std::numeric_limits<T>::min)() ||
              value > (std::numeric_limits<T>::max)()) {
            return codecFailure(error, CodecErrorCode::type_mismatch);
          }
        } else if (value < 0 || static_cast<std::uint64_t>(value) >
                                    (std::numeric_limits<T>::max)()) {
          return codecFailure(error, CodecErrorCode::type_mismatch);
        }
        *output = static_cast<T>(value);
      } else if (input.is_uint64()) {
        const auto value = input.as_uint64();
        if (value >
            static_cast<std::uint64_t>((std::numeric_limits<T>::max)())) {
          return codecFailure(error, CodecErrorCode::type_mismatch);
        }
        *output = static_cast<T>(value);
      } else if (input.is_double()) {
        const double value = input.as_double();
        if (!std::isfinite(value) || std::trunc(value) != value ||
            value < static_cast<double>((std::numeric_limits<T>::min)()) ||
            value > static_cast<double>((std::numeric_limits<T>::max)())) {
          return codecFailure(error, CodecErrorCode::type_mismatch);
        }
        *output = static_cast<T>(value);
      } else {
        return codecFailure(error, CodecErrorCode::type_mismatch);
      }
      return true;
    }
  }
};

template <> struct JsonConversion<bool> {
  static bool encode(bool input, boost::json::value *output,
                     CodecContext &context, CodecError *error) {
    if (!context.account(input ? 4 : 5, error)) {
      return false;
    }
    *output = input;
    return true;
  }
  static bool decode(const boost::json::value &input, bool *output,
                     CodecContext &, CodecError *error) {
    if (!input.is_bool()) {
      return codecFailure(error, CodecErrorCode::type_mismatch);
    }
    *output = input.as_bool();
    return true;
  }
};

template <typename T>
struct JsonConversion<T, std::enable_if_t<std::is_floating_point_v<T>>> {
  static bool encode(T input, boost::json::value *output, CodecContext &context,
                     CodecError *error) {
    if (!context.account(1, error)) {
      return false;
    }
    if (std::isnan(input)) {
      *output = "NaN";
    } else if (std::isinf(input)) {
      *output = std::signbit(input) ? "-Infinity" : "Infinity";
    } else {
      *output = static_cast<double>(input);
    }
    return true;
  }
  static bool decode(const boost::json::value &input, T *output, CodecContext &,
                     CodecError *error) {
    if (input.is_string()) {
      if (input.as_string() == "NaN") {
        *output = std::numeric_limits<T>::quiet_NaN();
      } else if (input.as_string() == "Infinity") {
        *output = std::numeric_limits<T>::infinity();
      } else if (input.as_string() == "-Infinity") {
        *output = -std::numeric_limits<T>::infinity();
      } else {
        return codecFailure(error, CodecErrorCode::type_mismatch);
      }
      return true;
    }
    double staged;
    if (input.is_double()) {
      staged = input.as_double();
    } else if (input.is_int64()) {
      staged = static_cast<double>(input.as_int64());
    } else if (input.is_uint64()) {
      staged = static_cast<double>(input.as_uint64());
    } else {
      return codecFailure(error, CodecErrorCode::type_mismatch);
    }
    if (!std::isfinite(staged) ||
        std::abs(staged) >
            static_cast<double>((std::numeric_limits<T>::max)())) {
      return codecFailure(error, CodecErrorCode::type_mismatch);
    }
    *output = static_cast<T>(staged);
    return true;
  }
};

template <typename CharTraits, typename Allocator>
struct JsonConversion<std::basic_string<char, CharTraits, Allocator>> {
  using String = std::basic_string<char, CharTraits, Allocator>;
  static bool encode(const String &input, boost::json::value *output,
                     CodecContext &context, CodecError *error) {
    if (!accountJsonString({input.data(), input.size()}, context, error)) {
      return false;
    }
    *output = boost::json::string_view(input.data(), input.size());
    return true;
  }
  static bool decode(const boost::json::value &input, String *output,
                     CodecContext &context, CodecError *error) {
    if (!input.is_string()) {
      return codecFailure(error, CodecErrorCode::type_mismatch);
    }
    const auto &string = input.as_string();
    if (!accountJsonString({string.data(), string.size()}, context, error)) {
      return false;
    }
    output->assign(string.data(), string.size());
    return true;
  }
};

template <typename T, typename Wire = std::underlying_type_t<T>>
struct EnumJsonConversion {
  static bool encode(T input, boost::json::value *output, CodecContext &context,
                     CodecError *error) {
    return JsonConversion<Wire>::encode(static_cast<Wire>(input), output,
                                        context, error);
  }
  static bool decode(const boost::json::value &input, T *output,
                     CodecContext &context, CodecError *error) {
    Wire staged{};
    if (!JsonConversion<Wire>::decode(input, &staged, context, error)) {
      return false;
    }
    *output = static_cast<T>(staged);
    return true;
  }
};
template <typename T>
struct JsonConversion<T, std::enable_if_t<std::is_enum_v<T>>>
    : EnumJsonConversion<T> {};

template <typename T, typename Allocator>
struct JsonConversion<std::vector<T, Allocator>> {
  using Vector = std::vector<T, Allocator>;
  static bool encode(const Vector &input, boost::json::value *output,
                     CodecContext &context, CodecError *error) {
    CodecScope scope(context, error);
    if (!scope || !context.account(2, error) ||
        !context.account(input.empty() ? 0 : input.size() - 1, error)) {
      return false;
    }
    // Every array element needs at least one byte. Check before reserving.
    if (input.size() > context.limits().maxDocumentBytes) {
      return codecFailure(error, CodecErrorCode::document_limit);
    }
    boost::json::array staged(context.storage());
    staged.reserve(input.size());
    for (const auto &value : input) {
      boost::json::value encoded(context.storage());
      if (!JsonConversion<T>::encode(value, &encoded, context, error)) {
        return false;
      }
      staged.push_back(std::move(encoded));
    }
    *output = std::move(staged);
    return true;
  }
  static bool decode(const boost::json::value &input, Vector *output,
                     CodecContext &context, CodecError *error) {
    CodecScope scope(context, error);
    if (!scope) {
      return false;
    }
    if (!input.is_array()) {
      return codecFailure(error, CodecErrorCode::type_mismatch);
    }
    if (!context.account(input.as_array().size(), error)) {
      return false;
    }
    Vector staged;
    staged.reserve(input.as_array().size());
    for (const auto &value : input.as_array()) {
      T decoded{};
      if (!JsonConversion<T>::decode(value, &decoded, context, error)) {
        return false;
      }
      staged.push_back(std::move(decoded));
    }
    *output = std::move(staged);
    return true;
  }
};

template <typename T, std::size_t Size>
struct JsonConversion<std::array<T, Size>> {
  using Array = std::array<T, Size>;
  static bool encode(const Array &input, boost::json::value *output,
                     CodecContext &context, CodecError *error) {
    CodecScope scope(context, error);
    if (!scope || !context.account(Size ? Size + 1 : 2, error)) {
      return false;
    }
    boost::json::array staged(context.storage());
    staged.reserve(Size);
    for (const auto &value : input) {
      boost::json::value encoded(context.storage());
      if (!JsonConversion<T>::encode(value, &encoded, context, error)) {
        return false;
      }
      staged.push_back(std::move(encoded));
    }
    *output = std::move(staged);
    return true;
  }
  static bool decode(const boost::json::value &input, Array *output,
                     CodecContext &context, CodecError *error) {
    CodecScope scope(context, error);
    if (!scope) {
      return false;
    }
    if (!input.is_array() || input.as_array().size() != Size) {
      return codecFailure(error, CodecErrorCode::type_mismatch);
    }
    if (!context.account(Size, error)) {
      return false;
    }
    Array staged{};
    for (std::size_t index = 0; index < Size; ++index) {
      if (!JsonConversion<T>::decode(input.as_array()[index], &staged[index],
                                     context, error)) {
        return false;
      }
    }
    *output = std::move(staged);
    return true;
  }
};

template <typename T, typename Conversion = JsonConversion<T>>
class TypedTypeCodec : public TypeCodec {
  // The selected RTT String ValueDataSource uses c_str() in its constructor
  // and value setter. Own complete length-aware storage, including when RTT
  // copies an invocation's data source. Ordinary components remain unchanged.
  class OwnedValue final : public RTT::internal::ValueDataSource<T> {
  public:
    explicit OwnedValue(const T &value) { this->mdata = value; }
    using RTT::internal::ValueDataSource<T>::set;
    void set(typename RTT::internal::AssignableDataSource<T>::param_t value)
        override {
      this->mdata = value;
    }
    OwnedValue *clone() const override { return new OwnedValue(this->mdata); }
  };

public:
  bool toJson(const DataSourcePtr &source, boost::json::value *result,
              CodecContext &context, CodecError *error) const override {
    const auto *typed =
        dynamic_cast<const RTT::internal::DataSource<T> *>(source.get());
    if (!typed || !result || result->storage() != context.storage() ||
        !typed->evaluate()) {
      return codecFailure(error, CodecErrorCode::invalid_data_source);
    }
    boost::json::value staged(context.storage());
    if (!Conversion::encode(typed->rvalue(), &staged, context, error)) {
      return false;
    }
    result->swap(staged);
    return true;
  }
  DataSourcePtr makeDataSource(const boost::json::value &value,
                               CodecContext &context,
                               CodecError *error) const override {
    T staged{};
    if (!Conversion::decode(value, &staged, context, error)) {
      return {};
    }
    return new OwnedValue(staged);
  }
  bool assign(const DataSourcePtr &staged, const DataSourcePtr &destination,
              CodecError *error) const override {
    const auto *value =
        dynamic_cast<const RTT::internal::DataSource<T> *>(staged.get());
    auto *target = dynamic_cast<RTT::internal::AssignableDataSource<T> *>(
        destination.get());
    if (!value || !target || !value->evaluate()) {
      return codecFailure(error, CodecErrorCode::invalid_data_source);
    }
    // Use RTT's reference/update assignment contract; unlike the historical
    // String value setter this preserves embedded NUL bytes. Custom assignable
    // sources must honor this standard path as well as set(value).
    target->set() = value->rvalue();
    target->updated();
    return true;
  }
  PortValueStatus portValue(const RTT::base::OutputPortInterface *port,
                            boost::json::value *result, CodecContext &context,
                            CodecError *error) const override {
    const auto *typed = dynamic_cast<const RTT::OutputPort<T> *>(port);
    if (!typed || !result || result->storage() != context.storage()) {
      codecFailure(error, CodecErrorCode::invalid_data_source);
      return PortValueStatus::error;
    }
    T sample{};
    if (!typed->getLastWrittenValue(sample)) {
      return PortValueStatus::waiting_for_initial_data;
    }
    boost::json::value staged(context.storage());
    if (!Conversion::encode(sample, &staged, context, error)) {
      return PortValueStatus::error;
    }
    result->swap(staged);
    return PortValueStatus::value;
  }
};

} // namespace RTT::http
