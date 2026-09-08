// SPDX-License-Identifier: LGPL-2.1-or-later
#include <rtt/http/codec_context.hpp>

#include <boost/json/basic_parser.hpp>
#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/parser.hpp>
#include <boost/json/serializer.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory_resource>
#include <new>
#include <set>
#include <stdexcept>
#include <vector>

namespace RTT::http {
namespace {
class BudgetExceeded final : public std::bad_alloc {};

class BudgetResource final : public boost::json::memory_resource {
public:
  explicit BudgetResource(std::size_t limit) : limit_(limit) {}

private:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override {
    auto used = used_.load(std::memory_order_relaxed);
    do {
      if (bytes > limit_ - used) {
        throw BudgetExceeded();
      }
    } while (!used_.compare_exchange_weak(used, used + bytes,
                                          std::memory_order_relaxed));
    try {
      return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    } catch (...) {
      used_.fetch_sub(bytes, std::memory_order_relaxed);
      throw;
    }
  }
  void do_deallocate(void *pointer, std::size_t bytes,
                     std::size_t alignment) override {
    std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    used_.fetch_sub(bytes, std::memory_order_relaxed);
  }
  bool do_is_equal(
      const boost::json::memory_resource &other) const noexcept override {
    return this == &other;
  }
  std::size_t limit_;
  std::atomic<std::size_t> used_{0};
};

class PmrAdapter final : public std::pmr::memory_resource {
public:
  explicit PmrAdapter(boost::json::storage_ptr storage)
      : storage_(std::move(storage)) {}

private:
  void *do_allocate(std::size_t bytes, std::size_t alignment) override {
    return storage_->allocate(bytes, alignment);
  }
  void do_deallocate(void *pointer, std::size_t bytes,
                     std::size_t alignment) override {
    storage_->deallocate(pointer, bytes, alignment);
  }
  bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }
  boost::json::storage_ptr storage_;
};

using boost::json::error_code;
using boost::json::string_view;

// Validate duplicate decoded keys before a DOM can replace them. Continue
// after a duplicate so malformed JSON still takes precedence over schema
// errors.
struct DuplicateHandler {
  static constexpr std::size_t max_array_size = SIZE_MAX;
  static constexpr std::size_t max_object_size = SIZE_MAX;
  static constexpr std::size_t max_string_size = SIZE_MAX;
  static constexpr std::size_t max_key_size = SIZE_MAX;
  struct Frame {
    explicit Frame(std::pmr::memory_resource *resource)
        : keys(resource), key(resource) {}
    std::pmr::set<std::pmr::string> keys;
    std::pmr::string key;
  };
  explicit DuplicateHandler(boost::json::storage_ptr storage)
      : resource(std::move(storage)), frames(&resource) {}
  PmrAdapter resource;
  std::pmr::vector<Frame> frames;
  bool duplicate = false;
  bool on_document_begin(error_code &) { return true; }
  bool on_document_end(error_code &) { return true; }
  bool on_object_begin(error_code &) {
    frames.emplace_back(&resource);
    return true;
  }
  bool on_array_begin(error_code &) {
    frames.emplace_back(&resource);
    return true;
  }
  bool on_object_end(std::size_t, error_code &) {
    frames.pop_back();
    return true;
  }
  bool on_array_end(std::size_t, error_code &) {
    frames.pop_back();
    return true;
  }
  bool on_key_part(string_view part, std::size_t, error_code &) {
    frames.back().key.append(part.data(), part.size());
    return true;
  }
  bool on_key(string_view part, std::size_t size, error_code &error) {
    on_key_part(part, size, error);
    auto &frame = frames.back();
    if (!frame.keys.insert(std::move(frame.key)).second) {
      duplicate = true;
    }
    frame.key.clear();
    return true;
  }
  bool on_string_part(string_view, std::size_t, error_code &) { return true; }
  bool on_string(string_view, std::size_t, error_code &) { return true; }
  bool on_number_part(string_view, error_code &) { return true; }
  bool on_int64(std::int64_t, string_view, error_code &) { return true; }
  bool on_uint64(std::uint64_t, string_view, error_code &) { return true; }
  bool on_double(double, string_view, error_code &) { return true; }
  bool on_bool(bool, error_code &) { return true; }
  bool on_null(error_code &) { return true; }
  bool on_comment_part(string_view, error_code &) { return true; }
  bool on_comment(string_view, error_code &) { return true; }
};

CodecErrorCode parseError(const error_code &error) {
  if (error == boost::json::error::too_deep) {
    return CodecErrorCode::depth_limit;
  }
  if (error == boost::json::error::exponent_overflow) {
    return CodecErrorCode::type_mismatch;
  }
  return CodecErrorCode::invalid_json;
}

bool validateOutput(const boost::json::value &root, CodecContext &context,
                    CodecError *error) {
  struct Frame {
    const boost::json::value *value;
    std::size_t next;
  };
  PmrAdapter resource(context.storage());
  std::pmr::vector<Frame> parents(&resource);
  const auto *current = &root;
  while (true) {
    if (current->is_string()) {
      const auto &text = current->as_string();
      if (!validUtf8({text.data(), text.size()})) {
        return codecFailure(error, CodecErrorCode::invalid_utf8);
      }
    } else if (current->is_double() && !std::isfinite(current->as_double())) {
      // Codecs must explicitly use the agreed non-finite string values.
      return codecFailure(error, CodecErrorCode::type_mismatch);
    } else if (current->is_array() || current->is_object()) {
      if (parents.size() == context.limits().maxDepth) {
        return codecFailure(error, CodecErrorCode::depth_limit);
      }
      parents.push_back({current, 0});
    }
    current = nullptr;
    while (!parents.empty()) {
      auto &parent = parents.back();
      if (parent.value->is_array()) {
        const auto &array = parent.value->as_array();
        if (parent.next < array.size()) {
          current = &array[parent.next++];
        }
      } else {
        const auto &object = parent.value->as_object();
        if (parent.next < object.size()) {
          const auto &member = *(object.begin() + parent.next++);
          if (!validUtf8({member.key().data(), member.key().size()})) {
            return codecFailure(error, CodecErrorCode::invalid_utf8);
          }
          current = &member.value();
        }
      }
      if (current) {
        break;
      }
      parents.pop_back();
    }
    if (!current) {
      return true;
    }
  }
}
} // namespace

CodecContext::CodecContext(JsonLimits limits)
    : limits_(limits),
      storage_(boost::json::make_shared_resource<BudgetResource>(
          limits.maxAllocationBytes)),
      remaining_(limits.maxDocumentBytes) {
  if (!limits.maxDocumentBytes || !limits.maxDepth ||
      !limits.maxAllocationBytes) {
    throw std::invalid_argument("JSON limits must be positive");
  }
}
const JsonLimits &CodecContext::limits() const noexcept { return limits_; }
const boost::json::storage_ptr &CodecContext::storage() const noexcept {
  return storage_;
}
bool CodecContext::enter(CodecError *error) {
  if (depth_ == limits_.maxDepth) {
    return codecFailure(error, CodecErrorCode::depth_limit);
  }
  ++depth_;
  return true;
}
void CodecContext::leave() noexcept {
  if (depth_) {
    --depth_;
  }
}
bool CodecContext::account(std::size_t bytes, CodecError *error) {
  if (bytes > remaining_) {
    return codecFailure(error, CodecErrorCode::document_limit);
  }
  remaining_ -= bytes;
  return true;
}

bool codecFailure(CodecError *error, CodecErrorCode code,
                  std::string_view location) {
  if (error) {
    error->code = code;
    if (location.empty()) {
      error->location.clear();
    } else {
      error->location.assign(location.data(),
                             (std::min)(location.size(), std::size_t(256)));
    }
  }
  return false;
}

bool validUtf8(std::string_view text) noexcept {
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index++]);
    if (first < 0x80) {
      continue;
    }
    unsigned value, count, minimum;
    if (first >= 0xc2 && first <= 0xdf) {
      value = first & 0x1f;
      count = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      value = first & 0x0f;
      count = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      value = first & 0x07;
      count = 3;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (count > text.size() - index) {
      return false;
    }
    while (count--) {
      const auto next = static_cast<unsigned char>(text[index++]);
      if ((next & 0xc0) != 0x80) {
        return false;
      }
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
      return false;
    }
  }
  return true;
}

bool parseJson(std::string_view text, CodecContext &context,
               boost::json::value *value, CodecError *error) {
  if (!value) {
    return codecFailure(error, CodecErrorCode::internal_error);
  }
  if (text.size() > context.limits().maxDocumentBytes) {
    return codecFailure(error, CodecErrorCode::document_limit);
  }
  boost::json::parse_options options;
  options.max_depth = context.limits().maxDepth;
  error_code parsed;
  try {
    {
      boost::json::basic_parser<DuplicateHandler> validator(options,
                                                            context.storage());
      const auto consumed =
          validator.write_some(false, text.data(), text.size(), parsed);
      if (parsed) {
        return codecFailure(error, parseError(parsed));
      }
      if (consumed != text.size()) {
        return codecFailure(error, CodecErrorCode::invalid_json);
      }
      if (validator.handler().duplicate) {
        return codecFailure(error, CodecErrorCode::duplicate_key);
      }
    }
    // Both the DOM and its construction workspace use the allocation budget.
    boost::json::parser parser(context.storage(), options);
    parser.reset(context.storage());
    parser.write(string_view(text.data(), text.size()), parsed);
    if (parsed) {
      return codecFailure(error, parseError(parsed));
    }
    auto staged = parser.release();
    // The caller supplies a result on the same resource; swapping avoids an
    // allocator-changing copy into unbounded default storage.
    if (value->storage() != context.storage()) {
      return codecFailure(error, CodecErrorCode::invalid_data_source);
    }
    value->swap(staged);
    if (error) {
      *error = {};
    }
    return true;
  } catch (const BudgetExceeded &) {
    return codecFailure(error, CodecErrorCode::allocation_limit);
  }
}

bool serializeJson(const boost::json::value &value, CodecContext &context,
                   std::string *text, CodecError *error) {
  if (!text) {
    return codecFailure(error, CodecErrorCode::internal_error);
  }
  try {
    if (!validateOutput(value, context, error)) {
      return false;
    }
    boost::json::serializer serializer(context.storage());
    serializer.reset(&value);
    std::string staged;
    char buffer[4096];
    while (!serializer.done()) {
      const auto part = serializer.read(buffer);
      if (part.size() > context.limits().maxDocumentBytes - staged.size()) {
        return codecFailure(error, CodecErrorCode::document_limit);
      }
      staged.append(part.data(), part.size());
    }
    *text = std::move(staged);
    if (error) {
      *error = {};
    }
    return true;
  } catch (const BudgetExceeded &) {
    return codecFailure(error, CodecErrorCode::allocation_limit);
  }
}

} // namespace RTT::http
