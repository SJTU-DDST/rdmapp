#include "helloworld_handler.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_future.hpp>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

#include <rdmapp/log.h>
#include <rdmapp/mr.h>
#include <rdmapp/rdmapp.h>

int main(int argc, char *argv[]) {
  rdmapp::log::setup(rdmapp::log::level::debug);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(1);
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  switch (argc) {
  case 2: {
    asio::signal_set signals(*io_ctx, SIGINT, SIGTERM);
    signals.async_wait([io_ctx](auto, auto) {
      io_ctx->stop();
      spdlog::info("gracefully shutdown");
    });
    auto work_guard = asio::make_work_guard(*io_ctx);
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = std::make_shared<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
    asio::co_spawn(*io_ctx, server(acceptor), asio::detached);
    io_ctx->run();
    break;
  }

  case 3: {
    auto connector = std::make_shared<rdmapp::qp_connector>(
        argv[1], std::stoi(argv[2]), pd, cq);
    auto fut = asio::co_spawn(*io_ctx, client(connector), asio::use_future);
    io_ctx->run();
    fut.get();
    spdlog::info("client exit after communicated with {}:{}", argv[1], argv[2]);
    break;
  }

  default: {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
    return -1;
  }
  }

  return 0;
}
