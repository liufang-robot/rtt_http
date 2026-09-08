#include <boost/json.hpp>
#include <iostream>
#include <limits>
#include <rtt/http/codec_context.hpp>
#include <stdexcept>

using namespace RTT::http;
void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int main() {
  try {
    CodecContext context;
    boost::json::value value(context.storage());
    CodecError error;
    require(
        parseJson(R"({"key":1,"nested":{"key":2}})", context, &value, &error),
        "nested key scope");
    const auto original = value;
    require(parseJson(" \t{\"key\":1,\"nested\":{\"key\":2}}\r\n ", context,
                      &value, &error) &&
                value == original,
            "surrounding JSON whitespace accepted");
    require(!parseJson(R"({"key":1,"\u006bey":2})", context, &value, &error) &&
                error.code == CodecErrorCode::duplicate_key &&
                value == original,
            "escaped duplicate must fail without replacing output");
    require(!parseJson(R"({"key":1,"key":2,})", context, &value, &error) &&
                error.code == CodecErrorCode::invalid_json,
            "invalid JSON takes precedence over duplicates");
    require(!parseJson("{} false", context, &value, &error),
            "trailing JSON rejected");
    require(!parseJson("{", context, &value, &error),
            "incomplete JSON rejected");
    require(!parseJson("\"\xff\"", context, &value, &error),
            "malformed UTF-8 rejected");
    CodecContext shallow({1024, 2, 8192});
    boost::json::value depth_value(shallow.storage());
    require(!parseJson("[[[0]]]", shallow, &depth_value, &error) &&
                error.code == CodecErrorCode::depth_limit,
            "depth bound enforced");
    CodecContext small({8, 4, 4096});
    boost::json::value small_value(small.storage());
    require(!parseJson("[12345678]", small, &small_value, &error) &&
                error.code == CodecErrorCode::document_limit,
            "body bound enforced");
    std::string output = "unchanged";
    require(!serializeJson(original, small, &output, &error) &&
                output == "unchanged" &&
                error.code == CodecErrorCode::document_limit,
            "response overflow must not emit a truncated document");
    CodecContext starved({4096, 16, 32});
    boost::json::value no_memory(starved.storage());
    require(!parseJson(R"({"object":{"another":[1,2,3]}})", starved, &no_memory,
                       &error) &&
                error.code == CodecErrorCode::allocation_limit,
            "allocation budget enforced");
    require(validUtf8("\xe6\x9c\xba\xe5\x99\xa8") && !validUtf8("\xc0\x80") &&
                !validUtf8("\xed\xa0\x80") && !validUtf8("\xf4\x90\x80\x80"),
            "UTF-8 boundaries");
    require(serializeJson(original, context, &output, &error) &&
                boost::json::parse(output) == original,
            "valid JSON serialization");
    const auto last_output = output;
    require(!serializeJson(boost::json::parse("[[[0]]]"), shallow, &output,
                           &error) &&
                output == last_output &&
                error.code == CodecErrorCode::depth_limit,
            "serializer checks a custom codec's nesting depth");
    require(
        !serializeJson(boost::json::value("\xff"), context, &output, &error) &&
            output == last_output && error.code == CodecErrorCode::invalid_utf8,
        "invalid RTT UTF-8 must not be emitted or replaced");
    require(!serializeJson(
                boost::json::value(std::numeric_limits<double>::infinity()),
                context, &output, &error) &&
                output == last_output,
            "unmapped non-finite doubles must not silently serialize as null");
    std::cout
        << "JSON duplicate, syntax, UTF-8 and resource contracts passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
