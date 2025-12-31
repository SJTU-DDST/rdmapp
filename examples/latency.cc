#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/signal_set.hpp>
#include <asio/use_future.hpp>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

#include <rdmapp/log.h>
#include <rdmapp/mr.h>
#include <rdmapp/rdmapp.h>

constexpr std::size_t kMessageSize = 4096;
#ifdef RDMAPP_BUILD_DEBUG
constexpr int kSendCount = 100;
#else
constexpr int kSendCount = 1000'000;
#endif

asio::awaitable<void> handler(std::shared_ptr<rdmapp::qp> qp) {
  std::array<std::byte, kMessageSize> buffer;
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());
  auto local_mr_serialized = local_mr.serialize();
  co_await qp->send(local_mr_serialized);

  spdlog::info("server: local buffer sent");

  auto start_time = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < kSendCount; i++) {
    co_await qp->recv();
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);
  spdlog::info("[write_with_imm/recv] total operations: {}", kSendCount);
  spdlog::info("[write_with_imm/recv] total time: {}ms",
               total_duration.count());
  spdlog::info("[write_with_imm/recv] average time per operation: {:.3f}ms",
               static_cast<double>(total_duration.count()) / kSendCount);

  start_time = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < kSendCount; i++) {
    co_await qp->recv(local_mr);
  }
  end_time = std::chrono::high_resolution_clock::now();

  total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);
  spdlog::info("[send/recv] total operations: {}", kSendCount);
  spdlog::info("[send/recv] total time: {}ms", total_duration.count());
  spdlog::info("[send/recv] average time per operation: {:.3f}ms",
               static_cast<double>(total_duration.count()) / kSendCount);
};

asio::awaitable<void> client_worker(std::shared_ptr<rdmapp::qp> qp) {
  std::array<std::byte, kMessageSize> buffer;
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());

  auto remote_mr_serialized =
      rdmapp::mr_view(local_mr, 0, rdmapp::remote_mr::kSerializedSize);
  auto [nbytes, _] = co_await qp->recv(remote_mr_serialized);

  assert(nbytes == rdmapp::remote_mr::kSerializedSize);
  void(nbytes), void(_);

  rdmapp::remote_mr remote_mr =
      rdmapp::remote_mr::deserialize(remote_mr_serialized.span().data());

  spdlog::info("client: remote buffer recv");

  std::fill(buffer.begin(), buffer.end(), std::byte(0xdd));

  for (int i = 0; i < kSendCount; i++) {
    std::size_t nbytes [[maybe_unused]] =
        co_await qp->write_with_imm(remote_mr, local_mr, i);
    assert(nbytes == kMessageSize);
  }

  for (int i = 0; i < kSendCount; i++) {
    std::size_t nbytes [[maybe_unused]] = co_await qp->send(local_mr);
    assert(nbytes == kMessageSize);
  }
}

// NOTE: for server use basic_qp<AtPoller> will perform better in latency
// we can use io_context driven client + poller with consumer driven server
// for lower transport latency; by default, this example use io_context driven
// for both server and client
asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor) {
  auto executor = co_await asio::this_coro::executor;
  while (true) {
    auto qp = co_await acceptor->accept();
    asio::co_spawn(executor, handler(std::move(qp)), asio::detached);
  }
}

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto executor = co_await asio::this_coro::executor;
  auto qp = co_await connector->connect();
  asio::co_spawn(executor, client_worker(std::move(qp)), asio::detached);
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
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
    while (!io_ctx->stopped()) {
      io_ctx->poll();
    }
    break;
  }

  case 3: {
    auto connector = std::make_shared<rdmapp::qp_connector>(
        argv[1], std::stoi(argv[2]), pd, cq);
    asio::co_spawn(*io_ctx, client(connector), asio::detached);
    while (!io_ctx->stopped()) {
      io_ctx->poll();
    }
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
