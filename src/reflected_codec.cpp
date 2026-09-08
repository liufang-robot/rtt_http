// SPDX-License-Identifier: LGPL-2.1-or-later
#include <rtt/http/reflected_codec.hpp>
#include <rtt/types/Types.hpp>

#include <optional>
#include <set>

namespace RTT::http {
namespace {
struct Member {
  std::string name;
  const RTT::types::TypeInfo *type;
  std::shared_ptr<const TypeCodec> codec;
};

class ReflectedCodec final : public TypeCodec {
public:
  ReflectedCodec(RTT::types::TypeInfo *type, bool sequence,
                 std::optional<std::size_t> length, std::vector<Member> members,
                 RetainedSampleReader reader)
      : type_(type), sequence_(sequence), length_(length),
        members_(std::move(members)), reader_(std::move(reader)) {}
  bool toJson(const DataSourcePtr &source, boost::json::value *result,
              CodecContext &context, CodecError *error) const override {
    CodecScope scope(context, error);
    if (!scope) {
      return false;
    }
    if (!source || source->getTypeInfo() != type_ || !source->evaluate() ||
        !result || result->storage() != context.storage()) {
      return codecFailure(error, CodecErrorCode::invalid_data_source);
    }
    if (!context.account(2, error)) {
      return false;
    }
    boost::json::value staged(context.storage());
    if (sequence_) {
      const auto length_source = type_->getMember(source, "size");
      const auto *size = dynamic_cast<const RTT::internal::DataSource<int> *>(
          length_source.get());
      if (!size) {
        return codecFailure(error, CodecErrorCode::invalid_data_source);
      }
      const int count = size->get();
      if (count < 0 ||
          (length_ && static_cast<std::size_t>(count) != *length_)) {
        return codecFailure(error, CodecErrorCode::invalid_data_source);
      }
      if (!context.account(static_cast<std::size_t>(count), error)) {
        return false;
      }
      auto &array = staged.emplace_array();
      array.reserve(static_cast<std::size_t>(count));
      for (int index = 0; index < count; ++index) {
        const auto member = type_->getMember(source, std::to_string(index));
        boost::json::value value(context.storage());
        if (!member || member->getTypeInfo() != members_[0].type ||
            !members_[0].codec->toJson(member, &value, context, error)) {
          return false;
        }
        array.push_back(std::move(value));
      }
    } else {
      auto &object = staged.emplace_object();
      for (const auto &member : members_) {
        if (!accountJsonString(member.name, context, error) ||
            !context.account(2, error)) {
          return false;
        }
        const auto source_member = type_->getMember(source, member.name);
        boost::json::value value(context.storage());
        if (!source_member || source_member->getTypeInfo() != member.type ||
            !member.codec->toJson(source_member, &value, context, error)) {
          return false;
        }
        object.emplace(member.name, std::move(value));
      }
    }
    result->swap(staged);
    return true;
  }
  DataSourcePtr makeDataSource(const boost::json::value &value,
                               CodecContext &context,
                               CodecError *error) const override {
    CodecScope scope(context, error);
    if (!scope) {
      return {};
    }
    if (sequence_) {
      if (!value.is_array() ||
          (length_ && value.as_array().size() != *length_) ||
          value.as_array().size() >
              static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        codecFailure(error, CodecErrorCode::type_mismatch);
        return {};
      }
      if (!context.account(value.as_array().size(), error)) {
        return {};
      }
    } else {
      if (!value.is_object() || value.as_object().size() != members_.size()) {
        codecFailure(error, CodecErrorCode::type_mismatch);
        return {};
      }
      for (const auto &member : members_) {
        if (!value.as_object().contains(member.name)) {
          codecFailure(error, CodecErrorCode::type_mismatch, member.name);
          return {};
        }
      }
    }
    const auto staged = type_->buildValue();
    if (!staged || !staged->isAssignable()) {
      codecFailure(error, CodecErrorCode::invalid_data_source);
      return {};
    }
    if (sequence_) {
      if (!length_ &&
          !type_->resize(staged, static_cast<int>(value.as_array().size()))) {
        codecFailure(error, CodecErrorCode::invalid_data_source);
        return {};
      }
      std::size_t index = 0;
      for (const auto &element : value.as_array()) {
        const auto destination =
            type_->getMember(staged, std::to_string(index++));
        const auto decoded =
            members_[0].codec->makeDataSource(element, context, error);
        if (!decoded ||
            !members_[0].codec->assign(decoded, destination, error)) {
          return {};
        }
      }
    } else {
      for (const auto &member : members_) {
        const auto decoded = member.codec->makeDataSource(
            value.as_object().at(member.name), context, error);
        const auto destination = type_->getMember(staged, member.name);
        if (!decoded || !member.codec->assign(decoded, destination, error)) {
          return {};
        }
      }
    }
    return staged;
  }
  bool assign(const DataSourcePtr &staged, const DataSourcePtr &destination,
              CodecError *error) const override {
    if (!staged || !destination || !destination->isAssignable() ||
        staged->getTypeInfo() != type_ || destination->getTypeInfo() != type_) {
      return codecFailure(error, CodecErrorCode::invalid_data_source);
    }
    if (!destination->update(staged.get())) {
      return codecFailure(error, CodecErrorCode::invalid_data_source);
    }
    return true;
  }
  PortValueStatus portValue(const RTT::base::OutputPortInterface *port,
                            boost::json::value *result, CodecContext &context,
                            CodecError *error) const override {
    if (!reader_) {
      codecFailure(error, CodecErrorCode::unavailable);
      return PortValueStatus::error;
    }
    DataSourcePtr sample;
    const auto status = reader_(port, &sample, error);
    if (status != PortValueStatus::value) {
      return status;
    }
    return toJson(sample, result, context, error) ? PortValueStatus::value
                                                  : PortValueStatus::error;
  }
  bool supportsPortValue() const noexcept override {
    return static_cast<bool>(reader_);
  }

private:
  RTT::types::TypeInfo *type_;
  bool sequence_;
  std::optional<std::size_t> length_;
  std::vector<Member> members_;
  RetainedSampleReader reader_;
};
} // namespace

std::unique_ptr<TypeProtocol>
makeReflectedTypeProtocol(RTT::types::TypeInfo *type, TypeRegistration identity,
                          RetainedSampleReader reader, std::string *error) {
  const auto fail =
      [&](const std::string &message) -> std::unique_ptr<TypeProtocol> {
    if (error) {
      *error = message;
    }
    return {};
  };
  if (!type || !type->getMemberFactory()) {
    return fail("RTT type has no structural member metadata");
  }
  const auto prototype = type->buildValue();
  if (!prototype || !prototype->isAssignable()) {
    return fail("RTT type cannot construct owned assignable storage");
  }
  const auto element =
      type->getMember(prototype, new RTT::internal::ConstantDataSource<int>(0));
  const bool sequence = static_cast<bool>(element);
  std::optional<std::size_t> length;
  std::vector<Member> members;
  std::set<std::string> dependencies;
  const auto addMember = [&](std::string name, const DataSourcePtr &value) {
    const auto *member_type = value ? value->getTypeInfo() : nullptr;
    const auto codec = registeredTypeCodec(member_type);
    if (!value || !value->isAssignable() || !codec ||
        member_type->getTypeName() == "Void") {
      return false;
    }
    dependencies.insert(member_type->getTypeName());
    members.push_back({std::move(name), member_type, codec});
    return true;
  };
  identity.rttType = type->getTypeName();
  if (sequence) {
    if (!addMember("", element)) {
      return fail(
          "missing HTTP sequence element codec or assignable element metadata");
    }
    const auto size_source = type->getMember(prototype, "size");
    const auto *size =
        dynamic_cast<const RTT::internal::DataSource<int> *>(size_source.get());
    if (!size || size->get() < 0) {
      return fail("RTT sequence has no valid size metadata");
    }
    if (!type->resize(prototype, 0)) {
      length = static_cast<std::size_t>(size->get());
    }
    identity.descriptor = {
        {"kind", "array"},
        {"elementType", members[0].type->getTypeName()},
        {"length", length
                       ? boost::json::value(static_cast<std::int64_t>(*length))
                       : boost::json::value()}};
  } else {
    const auto names = type->getMemberNames();
    if (names.empty()) {
      return fail("RTT type has no complete structure or sequence metadata");
    }
    std::set<std::string> seen;
    boost::json::array fields;
    for (const auto &name : names) {
      if (name.empty() || !validUtf8(name) || !seen.insert(name).second) {
        return fail("RTT structure has invalid or duplicate field metadata");
      }
      if (!addMember(name, type->getMember(prototype, name))) {
        return fail("missing HTTP field codec or assignable metadata: " + name);
      }
      fields.push_back(
          {{"name", name}, {"rttType", members.back().type->getTypeName()}});
    }
    identity.descriptor = {{"kind", "object"}, {"fields", std::move(fields)}};
  }
  identity.dependencies.assign(dependencies.begin(), dependencies.end());
  auto codec = std::make_shared<ReflectedCodec>(
      type, sequence, length, std::move(members), std::move(reader));
  if (error) {
    error->clear();
  }
  return std::make_unique<TypeProtocol>(std::move(identity), std::move(codec));
}

std::map<std::string, std::string> registerReflectedTypeProtocols() {
  std::map<std::string, std::string> unresolved;
  std::set<RTT::types::TypeInfo *> pending;
  for (const auto &name : RTT::types::Types()->getTypes()) {
    auto *type = RTT::types::Types()->type(name);
    if (type && !registeredTypeCodec(type) &&
        type->getTypeName() != "unknown_t") {
      pending.insert(type);
    }
  }
  bool progress;
  do {
    progress = false;
    for (auto it = pending.begin(); it != pending.end();) {
      auto *type = *it;
      auto &error = unresolved[type->getTypeName()];
      auto protocol = makeReflectedTypeProtocol(type,
                                                {"orocos.rtt.http.reflection",
                                                 "1",
                                                 type->getTypeName(),
                                                 "1",
                                                 {},
                                                 {},
                                                 {}},
                                                {}, &error);
      if (protocol && registerTypeProtocol(type, std::move(protocol), &error)) {
        unresolved.erase(type->getTypeName());
        it = pending.erase(it);
        progress = true;
      } else {
        ++it;
      }
    }
  } while (progress);
  return unresolved;
}

} // namespace RTT::http
