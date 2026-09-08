// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once

#include <boost/json/storage_ptr.hpp>
#include <boost/json/value.hpp>
#include <rtt/http/export.hpp>

#include <cstddef>
#include <string>
#include <string_view>

namespace RTT::http {

enum class CodecErrorCode {
  none,
  invalid_json,
  duplicate_key,
  invalid_utf8,
  type_mismatch,
  depth_limit,
  document_limit,
  allocation_limit,
  unavailable,
  invalid_data_source,
  internal_error
};

struct CodecError {
  CodecErrorCode code{CodecErrorCode::none};
  // A codec may identify a field/index, but must not put component values,
  // exception messages, or implementation details in diagnostics.
  std::string location;
};

struct JsonLimits {
  std::size_t maxDocumentBytes{1024 * 1024};
  std::size_t maxDepth{64};
  std::size_t maxAllocationBytes{8 * 1024 * 1024};
};

// One conversion owns its budget. JSON values retain the allocation resource
// when moved to a longer-lived invocation; they never refer into a request.
// Component/custom-code allocations remain the application's responsibility.
class RTT_HTTP_API CodecContext {
public:
  explicit CodecContext(JsonLimits limits = {});
  const JsonLimits &limits() const noexcept;
  const boost::json::storage_ptr &storage() const noexcept;
  bool enter(CodecError *error);
  void leave() noexcept;
  bool account(std::size_t bytes, CodecError *error);

private:
  JsonLimits limits_;
#if defined(_MSC_VER)
  // This C++ SDK requires the same Boost and shared CRT as its consumers.
  // storage_ptr is header-defined; it has no separate DLL interface to export.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
  boost::json::storage_ptr storage_;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  std::size_t depth_{0};
  std::size_t remaining_;
};

class CodecScope {
public:
  CodecScope(CodecContext &context, CodecError *error)
      : context_(context), entered_(context.enter(error)) {}
  ~CodecScope() {
    if (entered_) {
      context_.leave();
    }
  }
  explicit operator bool() const noexcept { return entered_; }
  CodecScope(const CodecScope &) = delete;
  CodecScope &operator=(const CodecScope &) = delete;

private:
  CodecContext &context_;
  bool entered_;
};

RTT_HTTP_API bool codecFailure(CodecError *error, CodecErrorCode code,
                               std::string_view location = {});
RTT_HTTP_API bool validUtf8(std::string_view text) noexcept;
RTT_HTTP_API bool parseJson(std::string_view text, CodecContext &context,
                            boost::json::value *value,
                            CodecError *error = nullptr);
RTT_HTTP_API bool serializeJson(const boost::json::value &value,
                                CodecContext &context, std::string *text,
                                CodecError *error = nullptr);

} // namespace RTT::http
