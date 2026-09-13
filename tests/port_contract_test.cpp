// SPDX-License-Identifier: LGPL-2.1-or-later
#include <atomic>
#include <boost/json.hpp>
#include <boost/serialization/array_wrapper.hpp>
#include <boost/serialization/nvp.hpp>
#include <condition_variable>
#include <future>
#include <httplib.h>
#include <iostream>
#include <mutex>
#include <rtt/Activity.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/http/server.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/CArrayTypeInfo.hpp>
#include <rtt/types/StructTypeInfo.hpp>
#include <stdexcept>

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}
struct Pair {
  double x{1}, y{2};
  template <class Archive> void serialize(Archive &archive, unsigned int) {
    archive &boost::serialization::make_nvp("x", x);
    archive &boost::serialization::make_nvp("y", y);
  }
};
struct Batch {
  double axes[3]{3, 4, 5};
  template <class Archive> void serialize(Archive &archive, unsigned int) {
    archive &boost::serialization::make_nvp(
        "axes", boost::serialization::make_array(axes, 3));
  }
};
class Controller : public RTT::TaskContext {
public:
  Controller() : RTT::TaskContext("passive") {
    setActivity(new RTT::Activity(ORO_SCHED_OTHER, 0, 0.0));
    ports()->addPort(input);
    ports()->addPort(output);
    ports()->addPort(assembled);
    ports()->addPort(batch);
    ports()->addPort(vector);
    RTT::Service::shared_ptr io(new RTT::Service("io", this));
    io->addPort(nested);
    provides()->addService(io);
    input.setDataSample(6);
    assembled.setDataSample(Pair{});
    batch.setDataSample(Batch{});
    vector.setDataSample({1, 2, 3});
  }
  ~Controller() {
    release();
    stop();
  }
  void updateHook() override {
    std::unique_lock lock(mutex);
    ++cycles;
    entered = true;
    wake.notify_all();
    wake.wait(lock, [&] { return !hold; });
  }
  void release() {
    std::lock_guard lock(mutex);
    hold = false;
    wake.notify_all();
  }
  void cycle() {
    unsigned previous;
    {
      std::lock_guard lock(mutex);
      previous = cycles;
    }
    require(start(), "start acquisition cycle");
    require(getActivity()->trigger(), "trigger acquisition cycle");
    {
      std::unique_lock lock(mutex);
      require(wake.wait_for(lock, std::chrono::seconds(2),
                            [&] { return cycles != previous; }),
              "acquisition cycle reached updateHook");
    }
    require(stop(), "finish acquisition cycle");
  }
  RTT::InputPort<double> input{"input"};
  RTT::OutputPort<Pair> output{"output"};
  RTT::InputPort<Pair> assembled{"assembled"};
  RTT::InputPort<Batch> batch{"batch"};
  RTT::InputPort<std::vector<double>> vector{"vector"};
  RTT::InputPort<double> nested{"input"};
  std::mutex mutex;
  std::condition_variable wake;
  bool entered{false}, hold{true};
  unsigned cycles{0};
};
void status(const httplib::Result &response, int expected) {
  require(response && response->status == expected,
          "expected HTTP " + std::to_string(expected) + ", got " +
              (response
                   ? std::to_string(response->status) + ": " + response->body
                   : "connection failure"));
}
boost::json::value json(const httplib::Result &response) {
  status(response, 200);
  return boost::json::parse(response->body);
}
const std::string root = "/api/v1/components/passive";
const std::string ports = root + "/ports/";
} // namespace

int main(int argc, char **argv) {
  try {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
    RTT::types::RealTimeTypekitPlugin().loadConstructors();
    RTT::types::Types()->addType(
        new RTT::types::StructTypeInfo<Pair>("HttpPair"));
    RTT::types::Types()->addType(
        new RTT::types::CArrayTypeInfo<RTT::types::carray<double>>(
            "HttpFixedDoubles"));
    RTT::types::Types()->addType(
        new RTT::types::StructTypeInfo<Batch>("HttpBatch"));
    if (argc > 1) {
      RTT::TaskContext component("lifetime");
      auto input = std::make_unique<RTT::InputPort<Pair>>("input");
      auto output = std::make_unique<RTT::OutputPort<Pair>>("output");
      component.addPort(*input);
      component.addPort(*output);
      input->setDataSample(Pair{11, 12});
      RTT::internal::PortDataAccess::publish(*output, Pair{21, 22});
      RTT::http::Server server;
      httplib::Server reservation;
      const auto number = reservation.bind_to_any_port("127.0.0.1");
      require(number > 0, "reserve HTTP listener");
      reservation.stop();
      RTT::http::ServerOptions options;
      options.port = number;
      std::string error;
      require(server.start(options, &error), error);
      require(server.publishComponent(component, &error), error);
      httplib::Client browser("127.0.0.1", number);
      browser.set_read_timeout(2);
      const std::string base = "/api/v1/components/lifetime/ports/";
      if (std::string(argv[1]) == "source") {
        require(server.enableInputWrite(component, "input.y", &error), error);
        input->disconnect();
        require(!input->connected(),
                "external graph disconnect releases source");
        status(browser.Post(base + "input/members/y/samples", "{\"value\":31}",
                            "application/json"),
               503);
        require(server.enableInputWrite(component, "input.y", &error), error);
        require(input->connected(),
                "re-enable reconnects an externally disconnected writer");
        require(input->getSourceConnections().size() == 1,
                "re-enable registers exactly one input source");
        status(browser.Post(base + "input/members/y/samples", "{\"value\":31}",
                            "application/json"),
               204);
        require(server.disableInputWrite(component, "input.y", &error), error);
        require(server.disableInputWrite(component, "input.y", &error),
                "disable an already-disabled input is idempotent");
        require(server.disableInputWrite(component, "input.x", &error),
                "disable an unconfigured input region is idempotent");
        require(!server.disableInputWrite(component, "output", &error),
                "disable rejects output direction");
      } else {
        RTT::InputPort<Pair> replacement("input");
        replacement.setDataSample(Pair{91, 92});
        require(json(browser.Get(base + "input/latest")).at("value").at("y") ==
                    12.0,
                "initial input observation");
        require(json(browser.Get(base + "output/latest")).at("value").at("y") ==
                    22.0,
                "initial output observation");
        component.ports()->removePort("input");
        component.ports()->removePort("output");
        input.reset();
        output.reset();
        require(json(browser.Get(base + "input/latest")).at("value").at("y") ==
                    12.0,
                "retained input observation survives removed port destruction");
        require(
            json(browser.Get(base + "output/latest")).at("value").at("y") ==
                22.0,
            "retained output observation survives removed port destruction");
        for (const auto &port : {"input", "output"}) {
          status(browser.Get(base + port), 404);
          status(browser.Get(base + port + "/members/y/latest"), 404);
        }
        component.addPort(replacement);
        status(browser.Get(base + "input/members/y/latest"), 404);
        require(!server.enableInputWrite(component, "input.y", &error),
                "replacement port is absent from existing publication");
        require(json(browser.Get(base + "input/latest")).at("value").at("y") ==
                    12.0,
                "replacement does not retarget retained observation");
      }
      server.finishShutdown();
      return 0;
    }
    Controller controller;
    RTT::OutputPort<double> local("local");
    require(local.createConnection(controller.input),
            "connect ordinary input producer");
    require(RTT::internal::PortDataAccess::publish(local, 7.0) ==
                RTT::WriteSuccess,
            "stage local sample");
    require(RTT::internal::PortDataAccess::refresh(controller.input) ==
                RTT::NewData,
            "acquire local sample before publication");
    require(controller.configure() && controller.start(),
            "start observed component");
    require(controller.getActivity()->trigger(), "enter held hook");
    {
      std::unique_lock lock(controller.mutex);
      require(controller.wake.wait_for(lock, std::chrono::seconds(2),
                                       [&] { return controller.entered; }),
              "held component hook entered");
    }
    RTT::http::Server server;
    struct Release {
      Controller &c;
      ~Release() { c.release(); }
    } release{controller};
    httplib::Server reservation;
    const auto number = reservation.bind_to_any_port("127.0.0.1");
    require(number > 0, "reserve HTTP listener");
    reservation.stop();
    RTT::http::ServerOptions options;
    options.port = number;
    options.workerThreads = 4;
    std::string error;
    require(server.start(options, &error), error);
    require(server.publishComponent(controller, &error),
            "publish already-connected running input: " + error);
    httplib::Client browser("127.0.0.1", number);
    browser.set_read_timeout(2);
    require(
        controller.input.connected() && !controller.output.connected() &&
            !controller.assembled.connected() && !controller.batch.connected(),
        "passive publication leaves existing and absent connections intact");
    require(json(browser.Get(ports + "input/latest")).at("value") == 7.0,
            "input reads component-acquired value");
    require(RTT::internal::PortDataAccess::publish(local, 9.0) ==
                RTT::WriteSuccess,
            "stage newer producer value");
    for (int repeat = 0; repeat != 3; ++repeat)
      require(
          json(browser.Get(ports + "input/latest")).at("value") == 7.0,
          "HTTP neither consumes newer local sample nor changes current image");
    require(!server.enableInputWrite(controller, "assembled.y", &error) &&
                !controller.assembled.connected(),
            "running owner rejects ingress setup");
    require(json(browser.Get(ports + "output/latest")).at("hasSample") == false,
            "output waits for first committed sample");
    status(browser.Post(ports + "input/samples", "{\"value\":10}",
                        "application/json"),
           404);
    status(browser.Post(ports + "output/samples", "{\"value\":10}",
                        "application/json"),
           404);
    controller.release();
    require(controller.stop(), "stop held component");
    require(RTT::internal::PortDataAccess::refresh(controller.input) ==
                    RTT::NewData &&
                json(browser.Get(ports + "input/latest")).at("value") == 9.0,
            "next acquisition exposes producer update");
    RTT::TaskContext unpublished("unpublished");
    require(!server.enableInputWrite(unpublished, "input", &error),
            "unpublished ingress is rejected");
    require(!server.enableInputWrite(controller, "output", &error),
            "output ingress is rejected");
    require(!server.enableInputWrite(controller, "input", &error),
            "existing producer rejects competing HTTP writer");
    require(!server.enableInputWrite(controller, "assembled.missing", &error),
            "unknown field rejected");
    require(!server.enableInputWrite(controller, "assembled::y", &error),
            "legacy selector rejected");
    require(!server.enableInputWrite(controller, "batch.axes[3]", &error),
            "out-of-range field rejected");

    RTT::OutputPort<double> localField("local_field");
    require(RTT::connectMembers(localField, "", controller.assembled, "x"),
            "connect disjoint local x source");
    require(server.enableInputWrite(controller, "assembled.y", &error), error);
    require(server.enableInputWrite(controller, "assembled.y", &error),
            "same source enable is idempotent");
    require(!server.enableInputWrite(controller, "assembled", &error),
            "whole source overlaps local/member input");
    require(!server.enableInputWrite(controller, "assembled.x", &error),
            "same field cannot have two writers");
    const auto member = ports + "assembled/members/y";
    const auto metadata = json(browser.Get(member));
    require(metadata.at("rttType") == "Float64" &&
                metadata.at("writable") == true &&
                metadata.at("samplesHref") ==
                    boost::json::value(member + "/samples") &&
                metadata.at("types").as_object().contains("Float64"),
            "selected route carries scalar type and writable metadata");
    require(
        json(browser.Get(ports + "assembled")).at("samplesHref").is_null() &&
            json(browser.Get(root)).at("ports").as_array().size() == 5,
        "member source does not enable whole writes or alter port discovery");
    status(browser.Post(ports + "assembled/samples",
                        "{\"value\":{\"x\":4,\"y\":5}}", "application/json"),
           404);
    status(browser.Post(member + "/samples", "{\"value\":{\"y\":20}}",
                        "application/json"),
           422);
    status(browser.Post(member + "/samples", "{\"value\":true}",
                        "application/json"),
           422);
    status(
        browser.Post(member + "/samples", "{\"value\":20}", "application/json"),
        204);
    require(json(browser.Get(member + "/latest")).at("value") == 2.0 &&
                controller.assembled.data().y == 2,
            "accepted member update remains staged");
    status(
        browser.Post(member + "/samples", "{\"value\":21}", "application/json"),
        204);
    localField.data() = 11;
    require(RTT::internal::PortDataAccess::commit(localField) ==
                RTT::WriteSuccess,
            "stage local member");
    controller.cycle();
    require(controller.assembled.data().x == 11 &&
                controller.assembled.data().y == 21,
            "one acquisition assembles disjoint local and latest HTTP members");
    const auto whole =
        json(browser.Get(ports + "assembled/latest")).at("value");
    require(whole.at("x") == 11.0 && whole.at("y") == 21.0 &&
                json(browser.Get(member + "/latest")).at("value") == 21.0,
            "whole and member observations agree with acquired image");
    require(server.disableInputWrite(controller, "assembled.y", &error), error);
    status(
        browser.Post(member + "/samples", "{\"value\":22}", "application/json"),
        404);
    require(json(browser.Get(member)).at("writable") == false &&
                localField.connected() && controller.assembled.data().y == 21,
            "disable releases only HTTP region and retains acquired value");
    require(server.enableInputWrite(controller, "assembled.y", &error),
            "released region can be re-enabled");

    require(server.enableInputWrite(controller, "batch.axes[2]", &error),
            error);
    const auto index = ports + "batch/members/axes%5B2%5D";
    require(json(browser.Get(index)).at("rttType") == "Float64",
            "fixed index selects exact scalar type");
    status(
        browser.Post(index + "/samples", "{\"value\":50}", "application/json"),
        204);
    require(json(browser.Get(index + "/latest")).at("value") == 5.0,
            "fixed index update waits for acquisition");
    controller.cycle();
    require(controller.batch.data().axes[0] == 3 &&
                controller.batch.data().axes[1] == 4 &&
                controller.batch.data().axes[2] == 50,
            "fixed member write preserves disjoint fields");
    status(browser.Get(ports + "batch/members/axes%5B3%5D/latest"), 404);
    status(browser.Get(ports + "batch/members/axes%5B-1%5D/latest"), 404);
    require(!server.enableInputWrite(controller, "batch.axes", &error),
            "array source overlaps enabled element");
    require(json(browser.Get(ports + "batch/latest")).at("value").at("axes") ==
                boost::json::array{3.0, 4.0, 50.0},
            "whole struct exposes its fixed array");
    require(server.disableInputWrite(controller, "batch.axes[2]", &error),
            error);
    require(server.enableInputWrite(controller, "batch.axes", &error), error);
    const auto fixed = ports + "batch/members/axes";
    status(browser.Post(fixed + "/samples", "{\"value\":[6,7]}",
                        "application/json"),
           422);
    status(browser.Post(fixed + "/samples", "{\"value\":[6,7,8]}",
                        "application/json"),
           204);
    require(json(browser.Get(fixed + "/latest")).at("value") ==
                boost::json::array{3.0, 4.0, 50.0},
            "complete selected fixed-array sample remains staged");
    controller.cycle();
    require(json(browser.Get(fixed + "/latest")).at("value") ==
                boost::json::array{6.0, 7.0, 8.0},
            "selected fixed-array codec observes its complete acquired sample");
    require(server.enableInputWrite(controller, "vector", &error), error);
    status(browser.Post(ports + "vector/samples", "{\"value\":4}",
                        "application/json"),
           422);
    status(browser.Post(ports + "vector/samples", "{\"value\":[4,true,6]}",
                        "application/json"),
           422);
    status(browser.Post(ports + "vector/samples", "{\"value\":[4,5,6]}",
                        "application/json"),
           204);
    require(json(browser.Get(ports + "vector/latest")).at("value") ==
                boost::json::array{1.0, 2.0, 3.0},
            "whole array write remains staged");
    controller.cycle();
    require(json(browser.Get(ports + "vector/latest")).at("value") ==
                boost::json::array{4.0, 5.0, 6.0},
            "whole array read observes acquired exact shape");
    require(server.enableInputWrite(controller, "io.input", &error), error);
    const auto nested = root + "/services/io/ports/input";
    status(
        browser.Post(nested + "/samples", "{\"value\":15}", "application/json"),
        204);
    require(json(browser.Get(nested + "/latest")).at("value") == 0.0,
            "nested service input observes acquisition boundary");
    controller.cycle();
    require(json(browser.Get(nested + "/latest")).at("value") == 15.0,
            "nested service ingress reaches exact port");

    controller.output.data() = Pair{100, 200};
    require(json(browser.Get(ports + "output/members/y/latest")).at("value") ==
                2.0,
            "member read cannot expose uncommitted output");
    RTT::internal::PortDataAccess::publish(controller.output, Pair{8, 9});
    require(
        json(browser.Get(ports + "output/latest")).at("value").at("y") == 9.0 &&
            json(browser.Get(ports + "output/members/y/latest")).at("value") ==
                9.0,
        "reflected output observation needs no codec-specific retained reader");
    status(browser.Put(ports + "output/members/y/latest", "{\"value\":1}",
                       "application/json"),
           405);
    std::vector<std::future<bool>> readers;
    for (int i = 0; i < 6; ++i)
      readers.push_back(std::async(std::launch::async, [&] {
        httplib::Client client("127.0.0.1", number);
        return json(client.Get(ports + "output/members/y/latest"))
                       .at("value") == 9.0 &&
               json(client.Get(ports + "output/latest")).at("value").at("y") ==
                   9.0;
      }));
    for (auto &reader : readers)
      require(reader.get(), "independent parallel observation");
    require(controller.start(), "start configured target");
    require(!server.disableInputWrite(controller, "assembled.y", &error),
            "running graph rejects source removal");
    server.finishShutdown();
    require(!controller.batch.connected() && !controller.vector.connected() &&
                !controller.nested.connected() && localField.connected() &&
                local.connected(),
            "shutdown removes HTTP sources and preserves local connections");
    require(!server.enableInputWrite(controller, "assembled.y", &error),
            "final shutdown closes configuration");
    std::cout
        << "Passive port observation, acquisition boundaries, exact member "
           "ingress, overlaps, shape validation and shutdown passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
