#include <spdlog/spdlog.h>

#include "helloworld_handler.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include "rdmapp/scheduler.h"
#include <cppcoro/sync_wait.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <rdmapp/log.h>
#include <rdmapp/mr.h>
#include <rdmapp/rdmapp.h>

int main(int argc, char *argv[]) {
  rdmapp::log::setup(rdmapp::log::level::debug);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);

  auto io_service = cppcoro::io_service(1);
  auto scheduler = std::make_shared<rdmapp::basic_scheduler>();
  std::jthread w([&]() {
    io_service.process_events();
    spdlog::info("io_service exit.");
  });
  std::jthread s([=]() {
    scheduler->run();
    spdlog::info("scheduler exit.");
  });

  switch (argc) {
  case 2: {
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = rdmapp::qp_acceptor(io_service, scheduler, port, pd);
    cppcoro::sync_wait(server(acceptor));
    break;
  }

  case 3: {
    uint16_t port = (uint16_t)std::stoi(argv[2]);
    std::string_view hostname = argv[1];
    auto connector = rdmapp::qp_connector(io_service, scheduler, pd);
    cppcoro::sync_wait(client(connector, hostname, port));
    spdlog::info("client exit after communicated with {}:{}", hostname, port);
    break;
  }

  default: {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
    break;
  }
  }

  io_service.stop();
  scheduler->stop();
  return 0;
}
