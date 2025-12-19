#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/bind_cancellation_slot.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/cancellation_type.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_future.hpp>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

#include <rdmapp/log.h>
#include <rdmapp/mr.h>
#include <rdmapp/rdmapp.h>

constexpr std::string_view msg = "hello";

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto qp = co_await connector->connect();

  /* Send/Recv */
  std::string buffer = std::string(msg);
  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  spdlog::info("sent to server: {}", buffer);
  co_return;
}

static asio::awaitable<void> handle_qp(std::shared_ptr<rdmapp::qp> qp) {
  spdlog::info("handling qp");
  std::string buffer;
  buffer.resize(msg.size());
  auto recv_buffer = std::as_writable_bytes(std::span(buffer));
  spdlog::info("waiting for recv from client: nbytes={}", buffer.size());

  auto [n, _] = co_await qp->recv(recv_buffer);
  spdlog::info("received {} bytes from client: {}", n, buffer);

  spdlog::info("waiting for another recv which will never come", buffer.size());
  co_await qp->recv(recv_buffer);
  spdlog::critical("should not reach here");
  co_return;
}

asio::awaitable<void> server(std::unique_ptr<rdmapp::qp_acceptor> acceptor) {
  while (true) {
    auto qp = co_await acceptor->accept();
    if (!qp) {
      spdlog::error("server: failed to accept qp, skipped");
      continue;
    }
    try {
      co_await handle_qp(qp);
    } catch (std::exception &e) {
      spdlog::error("handle_qp: error: msg={}", e.what());
      break;
    }
  }
  co_return;
}

int main(int argc, char *argv[]) {
  rdmapp::log::setup(rdmapp::log::level::trace);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(1);
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  switch (argc) {
  case 2: {
    asio::cancellation_signal cancel_signal_;
    asio::signal_set signals(*io_ctx, SIGINT, SIGTERM);
    auto work_guard = asio::make_work_guard(*io_ctx);

    signals.async_wait([io_ctx, &cancel_signal_, &work_guard](auto, auto) {
      cancel_signal_.emit(asio::cancellation_type::terminal);
      spdlog::info("server: sent terminal signal");
      work_guard.reset();
      spdlog::info("server: gracefully shutting down...");
    });

    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = std::make_unique<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
    asio::co_spawn(
        *io_ctx, server(std::move(acceptor)),
        asio::bind_cancellation_slot(cancel_signal_.slot(), asio::detached));

    io_ctx->run();
    spdlog::info("server exited");
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
    ::exit(-1);
  }
  }

  return 0;
}
