#include <rtt/internal/PortDataAccess.hpp>
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "raw_client.hpp"
#include <boost/json.hpp>
#include <condition_variable>
#include <atomic>
#include <future>
#include <httplib.h>
#include <iostream>
#include <rtt/Activity.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/http/server.hpp>
#include <rtt/http/typed_codec.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/Types.hpp>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
struct Unsupported {
  int value{};
};
class Controller : public RTT::TaskContext {
public:
  Controller()
      : RTT::TaskContext("arm"), output("position"),
        volatileOutput("volatile"), input("target") {
    setActivity(new RTT::Activity(ORO_SCHED_OTHER, 0, 0.0));
    addProperty("speed", speed);
    addProperty("label", label);
    addProperty("unsupported", unsupported);
    addAttribute("count", count);
    addConstant("constant", constant);
    ports()->addPort(output);
    ports()->addPort(volatileOutput);
    ports()->addPort(input);
    std::size_t index = 0;
    for (const char *name : {"motion/raw", "motion%2Fraw", "axes+\xc3\xa4"}) {
      RTT::Service::shared_ptr service(new RTT::Service(name, this));
      service->addProperty("speed", nestedSpeed[index++]);
      provides()->addService(service);
    }
    addOperation("increment", &Controller::increment, this, RTT::ClientThread)
        .arg("amount", "increment size");
    addOperation("own", &Controller::own, this, RTT::OwnThread)
        .arg("value", "input/output");
    addOperation("clear", &Controller::clear, this, RTT::ClientThread);
    addOperation("hold", &Controller::hold, this, RTT::OwnThread)
        .arg("value", "retained input/output");
    addOperation("stringResult", &Controller::stringResult, this,
                 RTT::ClientThread);
    addOperation("fail", &Controller::failOperation, this, RTT::ClientThread);
  }
  ~Controller() { release(); }
  bool increment(int amount) {
    clientThread = std::this_thread::get_id();
    count += amount;
    return false;
  }
  int own(int &value) {
    ownerThread = std::this_thread::get_id();
    value += 5;
    return value * 2;
  }
  void clear() { count = 0; }
  std::string stringResult() { return std::string("a\0b", 3); }
  int failOperation() { throw std::runtime_error("private exception payload"); }
  int hold(int &value) {
    std::unique_lock lock(mutex);
    entered = true;
    wake.notify_all();
    wake.wait(lock, [&] { return released; });
    value += 9;
    return value;
  }
  void updateHook() override {
    if (!pauseCycle.load()) return;
    std::unique_lock lock(mutex);
    cycleEntered = true;
    wake.notify_all();
    wake.wait(lock, [&] { return cycleReleased; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    cycleReleased = true;
    wake.notify_all();
  }
  double speed{1.5};
  double nestedSpeed[3]{11.0, 22.0, 33.0};
  std::string label{"controller"};
  Unsupported unsupported;
  int count{0};
  const int constant{17};
  RTT::OutputPort<double> output, volatileOutput;
  RTT::InputPort<double> input;
  std::thread::id clientThread, ownerThread;
  std::mutex mutex;
  std::condition_variable wake;
  bool entered{false}, released{false};
  std::atomic<bool> pauseCycle{false};
  bool cycleEntered{false}, cycleReleased{false};
};
boost::json::value json(const httplib::Result &response, int expected = 200) {
  require(response && response->status == expected,
          "unexpected HTTP response status");
  return boost::json::parse(response->body);
}
void status(const httplib::Result &response, int expected) {
  if (!response || response->status != expected) {
    throw std::runtime_error(
        "expected status " + std::to_string(expected) + ", got " +
        (response ? std::to_string(response->status) + " " + response->body
                  : "connection failure"));
  }
}
std::string operation(const char *name) {
  return std::string("/api/v1/components/arm/operations/") + name;
}
} // namespace

int main() {
  try {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
    RTT::types::Types()->addType(
        new RTT::types::TemplateTypeInfo<Unsupported, false>(
            "HttpUnsupported"));
    Controller controller;
    RTT::http::Server server;
    // A held operation is released even when an assertion throws, before the
    // server's mandatory final drain and before the component is destroyed.
    struct Release {
      Controller &controller;
      ~Release() { controller.release(); }
    } release{controller};
    httplib::Server reservation;
    const auto port = reservation.bind_to_any_port("127.0.0.1");
    require(port > 0, "reserve local test port");
    reservation.stop();
    RTT::http::ServerOptions options;
    options.port = static_cast<std::uint32_t>(port);
    options.workerThreads = 2;
    options.maxPendingOperations = 1;
    options.operationTimeoutMs = 100;
    options.shutdownGraceMs = 100;
    options.maxRequestBodyBytes = 2048;
    options.maxJsonDepth = 16;
    std::string error;
    require(server.start(options, &error), error.c_str());
    httplib::Client browser("127.0.0.1", port);
    browser.set_read_timeout(2);
    require(
        json(browser.Get("/api/v1/components")).at("items").as_array().empty(),
        "initial HTTP publication must be empty");
    status(browser.Get("/api/v1/components/arm"), 404);
    require(server.publishComponent(controller, &error), error.c_str());
    auto discovery = json(browser.Get("/api/v1/components/arm"));
    require(discovery.at("name") == "arm" &&
                discovery.at("types").as_object().contains("Float64"),
            "discover component and type schemas");
    require(
        json(browser.Get("/api/v1/components")).at("items").as_array().size() ==
            1,
        "only explicitly published component appears");
    status(browser.Get("/api/v1/components/Deployer"), 404);
    for (const char *name : {"", ".", ".."}) {
      RTT::TaskContext invalid("unaddressable");
      RTT::Service::shared_ptr service(new RTT::Service(name, &invalid));
      service->addProperty("speed", controller.speed);
      invalid.provides()->addService(service);
      require(!server.publishComponent(invalid),
              "empty/dot-only names reject whole publication");
      status(browser.Get("/api/v1/components/unaddressable"), 404);
    }
    std::size_t index = 0;
    for (const char *name :
         {"motion%2Fraw", "motion%252Fraw", "axes%2B%C3%A4"}) {
      const auto path = std::string("/api/v1/components/arm/services/") + name +
                        "/properties/speed";
      require(json(browser.Get(path)).at("value") ==
                  controller.nestedSpeed[index],
              "encoded names address distinct RTT members");
      status(browser.Put(path,
                         "{\"value\":" + std::to_string(100 + index) + "}",
                         "application/json"),
             204);
      require(controller.nestedSpeed[index] == 100 + index,
              "write reaches the named service");
      ++index;
    }
    status(browser.Get("/api/v1/components/arm/services/motion/raw"), 404);
    status(browser.Get("/api/v1/components/arm/services/bad%ZZ"), 400);
    RTT::InputPort<double> observer("observer");
    require(
        controller.output.createConnection(
            observer, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)),
        "independent output reader");
    const std::string speed = "/api/v1/components/arm/properties/speed";
    for (const auto &request :
         {std::string("PUT ") + speed +
              "#fragment HTTP/1.1\r\nHost: localhost\r\nConnection: "
              "close\r\nContent-Type: application/json\r\nContent-Length: "
              "12\r\n\r\n{\"value\":99}",
          std::string("FROB ") + speed +
              " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n"}) {
      RawClient raw(port);
      raw.send(request);
      const auto wire = raw.receive();
      require(wire.starts_with(request.starts_with("PUT") ? "HTTP/1.1 400"
                                                          : "HTTP/1.1 405"),
              "raw malformed targets and extension methods respect resource "
              "policy");
    }
    require(json(browser.Get(speed)).at("value") == 1.5, "live property read");
    status(browser.Put(speed, "{\"value\":3.25}",
                       "application/json; charset=UTF-8"),
           204);
    require(controller.speed == 3.25, "whole property assignment");
    status(browser.Put(speed, "{\"value\":4,\"value\":5}", "application/json"),
           422);
    status(browser.Put(speed, "{\"value\":4,\"extra\":5}", "application/json"),
           422);
    status(browser.Put(speed, "{\"value\":", "application/json"), 400);
    status(browser.Put(speed, "{\"value\":null}", "application/json"), 422);
    require(controller.speed == 3.25, "invalid requests cannot assign");
    status(browser.Put(speed, "{}", "text/plain"), 415);
    status(browser.Put(speed, std::string(2049, ' '), "application/json"), 413);
    const auto constant = browser.Put(
        "/api/v1/components/arm/attributes/constant", "invalid", "text/plain");
    status(constant, 405);
    require(constant->get_header_value("Allow") == "GET, HEAD, OPTIONS",
            "immutable Allow precedes conversion");
    status(browser.Get("/api/v1/components/arm/properties/unsupported"), 501);
    status(browser.Put("/api/v1/components/arm/properties/unsupported", "{}",
                       "application/json"),
           422);
    status(browser.Put("/api/v1/components/arm/properties/unsupported",
                       "{\"value\":1}", "application/json"),
           501);
    auto head = browser.Head(speed);
    status(head, 200);
    require(head->body.empty(), "HEAD returns no body");
    status(browser.Options(speed), 204);
    status(browser.Delete(speed), 405);
    status(browser.Get(operation("increment")), 405);
    status(browser.Head(operation("increment")), 405);
    auto result = json(browser.Post(operation("increment"),
                                    "{\"arguments\":[7]}", "application/json"));
    require(result.at("result") == false &&
                result.at("outputs").as_array().empty() &&
                controller.count == 7,
            "false operation result is HTTP success");
    result = json(browser.Post(operation("own"), "{\"arguments\":[2]}",
                               "application/json"));
    require(result.at("result") == 14 &&
                result.at("outputs").as_array()[0] == 7,
            "OwnThread return and reference output");
    require(controller.clientThread != controller.ownerThread &&
                controller.ownerThread != std::this_thread::get_id(),
            "RTT execution policies use distinct contexts");
    result = json(browser.Post(operation("stringResult"), "{\"arguments\":[]}",
                               "application/json"));
    require(result.at("result").as_string().size() == 3,
            "operation string output retains embedded NUL");
    result = json(browser.Post(operation("clear"), "{\"arguments\":[]}",
                               "application/json"));
    require(result.at("result").is_null() && controller.count == 0,
            "void operations return null");
    const auto failed = browser.Post(operation("fail"), "{\"arguments\":[]}",
                                     "application/json");
    status(failed, 500);
    require(failed->body.find("private exception") == std::string::npos,
            "operation exceptions are sanitized");
    require(controller.recover(), "recover after the deliberately failing operation");
    status(browser.Post(operation("increment"), "{\"arguments\":[]}",
                        "application/json"),
           422);
    require(json(browser.Get("/api/v1/components/arm/ports/position/latest"))
                    .at("hasSample") == false,
            "initial sample is absent");
    require(RTT::internal::PortDataAccess::publish(controller.output, 12.0) == RTT::WriteSuccess,
            "publish output sample");
    for (int i = 0; i < 2; ++i) {
      require(json(browser.Get("/api/v1/components/arm/ports/position/latest"))
                      .at("value") == 12.0,
              "retained output is repeatable");
    }
    controller.output.data() = 99.0;
    require(json(browser.Get("/api/v1/components/arm/ports/position/latest"))
                    .at("value") == 12.0,
            "HTTP cannot observe an uncommitted output image");
    double sample = 0;
    require(RTT::internal::PortDataAccess::receive(observer, sample) == RTT::NewData && sample == 12.0,
            "HTTP leaves another reader's data intact");
    require(json(browser.Get("/api/v1/components/arm/ports/volatile/latest"))
                    .at("hasSample") == false,
            "every output exposes a committed snapshot");
    controller.pauseCycle = true;
    require(controller.start(), "start component before network ingress");
    require(controller.getActivity()->trigger(), "trigger paused component cycle");
    {
      std::unique_lock lock(controller.mutex);
      require(controller.wake.wait_for(lock, std::chrono::seconds(2), [&] {
                return controller.cycleEntered;
              }), "component hook entered before HTTP ingress");
    }
    status(browser.Post("/api/v1/components/arm/ports/target/samples",
                        "{\"value\":18}", "application/json"),
           204);
    require(controller.input.data() == 0.0,
            "HTTP ingress stages data without changing the input image");
    {
      std::lock_guard lock(controller.mutex);
      controller.pauseCycle = false;
      controller.cycleReleased = true;
      controller.wake.notify_all();
    }
    require(controller.stop(), "stop component for explicit test acquisition");
    require(RTT::internal::PortDataAccess::refresh(controller.input) == RTT::NewData &&
                controller.input.data() == 18.0,
            "cycle acquisition delivers the staged HTTP sample");
    controller.input.disconnect();
    status(browser.Post("/api/v1/components/arm/ports/target/samples",
                        "{\"value\":19}", "application/json"),
           503);
    int late = 5;
    controller.addProperty("late", late);
    require(server.publishComponent(controller), "repeat publication");
    status(browser.Get("/api/v1/components/arm/properties/late"), 404);
    std::vector<std::future<bool>> frontends;
    for (int i = 0; i < 3; ++i) {
      frontends.push_back(std::async(std::launch::async, [&] {
        httplib::Client client("127.0.0.1", port);
        const auto response = client.Get(speed);
        return response && response->status == 200;
      }));
    }
    for (auto &frontend : frontends) {
      require(frontend.get(), "multiple independent frontends");
    }
    status(browser.Post(operation("hold"), "{\"arguments\":[1]}",
                        "application/json"),
           504);
    {
      std::lock_guard lock(controller.mutex);
      require(controller.entered,
              "held operation entered before response timeout");
    }
    require(server.pendingOperationCount() == 1,
            "timeout retains invocation capacity");
    server.stop();
    require(server.state() == "Stopped" && server.pendingOperationCount() == 1,
            "ordinary stop retains unfinished operation");
    options.workerThreads = 3;
    require(!server.start(options),
            "unfinished work rejects executor-width change");
    options.workerThreads = 2;
    require(server.start(options, &error), error.c_str());
    status(browser.Post(operation("increment"), "{\"arguments\":[1]}",
                        "application/json"),
           503);
    status(browser.Get("/api/v1/components/arm/properties/late"), 404);
    controller.release();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.pendingOperationCount() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(server.pendingOperationCount() == 0,
            "completion reclaims timed-out capacity");
    status(browser.Post(operation("increment"), "{\"arguments\":[1]}",
                        "application/json"),
           200);
    server.stop();
    options.maxResponseBodyBytes = 64;
    require(server.start(options, &error), error.c_str());
    const auto overflow = json(browser.Get("/api/v1/components/arm"), 500);
    require(overflow.at("type") == "urn:rtt-http:error:response-too-large",
            "bounded response overflow uses separate problem budget");
    server.finishShutdown();
    require(!server.start(options), "final shutdown rejects restart");
    std::cout << "REST discovery, values, ports, dispatch, timeout retention, "
                 "restart and concurrent clients passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
