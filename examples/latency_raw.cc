#include "asio/executor_work_guard.hpp"
#include "asio/io_context.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include "task.hpp"
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

#include "rdmapp/qp.h"
#include <rdmapp/log.h>
#include <rdmapp/mr.h>
#include <rdmapp/rdmapp.h>

constexpr std::size_t kMessageSize = 4096;
#ifdef RDMAPP_BUILD_DEBUG
constexpr int kSendCount = 100;
#else
constexpr int kSendCount = 1000'000;
#endif

task<void> client_worker(std::shared_ptr<rdmapp::qp> qp) {
  std::array<std::byte, kMessageSize> buffer;
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());

  auto remote_mr_serialized =
      rdmapp::mr_view(local_mr, 0, rdmapp::remote_mr::kSerializedSize);
  auto [nbytes, _] =
      co_await qp->recv(remote_mr_serialized, rdmapp::use_native_awaitable);

  assert(nbytes == rdmapp::remote_mr::kSerializedSize);
  void(nbytes), void(_);

  rdmapp::remote_mr remote_mr =
      rdmapp::remote_mr::deserialize(remote_mr_serialized.span().data());

  spdlog::info("client: remote buffer recv, size={}", remote_mr.length());

  std::fill(buffer.begin(), buffer.end(), std::byte(0xdd));

  for (int i = 0; i < kSendCount; i++) {
    std::size_t nbytes [[maybe_unused]] = co_await qp->write_with_imm(
        remote_mr, local_mr, i, rdmapp::use_native_awaitable);
  }

  for (int i = 0; i < kSendCount; i++) {
    std::size_t nbytes [[maybe_unused]] =
        co_await qp->send(local_mr, rdmapp::use_native_awaitable);
  }
}

task<void> qp_handler(std::shared_ptr<rdmapp::qp> qp) {
  std::array<std::byte, kMessageSize> buffer;
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());
  auto local_mr_serialized = local_mr.serialize();
  co_await qp->send(local_mr_serialized, rdmapp::use_native_awaitable);

  spdlog::info("server: local buffer sent");

  auto start_time = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < kSendCount; i++) {
    co_await qp->recv(rdmapp::use_native_awaitable);
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[write_with_imm/recv] total operations: {}", kSendCount);
  spdlog::info("[write_with_imm/recv] total time: {}us",
               total_duration.count());
  spdlog::info("[write_with_imm/recv] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / kSendCount);

  start_time = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < kSendCount; i++) {
    co_await qp->recv(local_mr, rdmapp::use_native_awaitable);
  }
  end_time = std::chrono::high_resolution_clock::now();

  total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[send/recv] total operations: {}", kSendCount);
  spdlog::info("[send/recv] total time: {}us", total_duration.count());
  spdlog::info("[send/recv] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / kSendCount);
}

task<void> server(asio::io_context &io_ctx, rdmapp::qp_acceptor &acceptor) {
  auto guard = asio::make_work_guard(io_ctx);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  while (true) {
    auto qp = asio::co_spawn(io_ctx, acceptor.accept(), asio::use_future).get();
    co_await qp_handler(qp);
  }
  guard.reset();
  co_return;
}

task<void> client(rdmapp::qp_connector &connector) {
  asio::io_context io_ctx(1);
  auto guard = asio::make_work_guard(io_ctx);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  auto qp = asio::co_spawn(io_ctx, connector.connect(), asio::use_future).get();
  guard.reset();
  co_await client_worker(qp);
  co_return;
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
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  switch (argc) {
  case 2: {
    auto io_ctx = std::make_shared<asio::io_context>(1);
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = rdmapp::qp_acceptor(io_ctx, port, pd, cq);
    server(*io_ctx, acceptor);
    break;
  }

  case 3: {
    auto connector = rdmapp::qp_connector(argv[1], std::stoi(argv[2]), pd, cq);
    client(connector);
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
