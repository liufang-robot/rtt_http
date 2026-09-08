// SPDX-License-Identifier: LGPL-2.1-or-later
#include <rtt/http/type_protocol.hpp>
#include <rtt/types/TransportPlugin.hpp>
#include <rtt/types/TypekitPlugin.hpp>

namespace RTT::http {
class HttpTransportPlugin final : public RTT::types::TransportPlugin {
public:
  bool registerTransport(std::string name,
                         RTT::types::TypeInfo *type) override {
    return registerCanonicalTypeProtocol(name, type);
  }
  std::string getTransportName() const override { return "HTTP"; }
  std::string getTypekitName() const override { return "rtt-types"; }
  std::string getName() const override { return "HTTP://rtt-types"; }
};
} // namespace RTT::http
ORO_TYPEKIT_PLUGIN(RTT::http::HttpTransportPlugin)
