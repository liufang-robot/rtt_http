// SPDX-License-Identifier: LGPL-2.1-or-later
#include <iostream>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/http/server.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>

int main(int argc, char **argv) {
  try {
    if (argc != 2) {
      return 2;
    }
    RTT::types::RealTimeTypekitPlugin().loadTypes();
    RTT::TaskContext component("arm");
    int values[]{11, 22, 33, 44};
    std::size_t index = 0;
    for (const auto *name :
         {"motion/raw", "motion%2Fraw", "axes+\xc3\xa4", "state#raw"}) {
      RTT::Service::shared_ptr service(new RTT::Service(name, &component));
      service->addProperty("value", values[index++]);
      component.provides()->addService(service);
    }
    RTT::http::Server server;
    RTT::http::ServerOptions options;
    options.port = static_cast<std::uint32_t>(std::stoul(argv[1]));
    std::string error;
    if (!server.start(options, &error) ||
        !server.publishComponent(component, &error)) {
      throw std::runtime_error(error);
    }
    std::cout << "ready\n" << std::flush;
    std::cin.get();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
