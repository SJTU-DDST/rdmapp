
#include <spdlog/spdlog.h>

#include "qp_acceptor.h"
#include "qp_connector.h"
#include <chrono>
#include <cppcoro/async_scope.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <rdmapp/completion_token.h>
#include <rdmapp/log.h>
#include <rdmapp/mr.h>
#include <rdmapp/qp.h>
#include <rdmapp/rdmapp.h>
#include <string>

constexpr std::size_t kMessageSize = 4096;
constexpr int kBatchSize = 100'000;
#ifdef RDMAPP_BUILD_DEBUG
constexpr int kSendCount = 100;
#else
constexpr int kSendCount = 1000'000;
#endif

cppcoro::task<void> client_worker(std::shared_ptr<rdmapp::qp> qp) {
  std::array<std::byte, kMessageSize> buffer;
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());

  auto remote_mr_serialized =
      rdmapp::mr_view(local_mr, 0, rdmapp::remote_mr::kSerializedSize);
  auto [nbytes, _] =
      co_await qp->recv(remote_mr_serialized, rdmapp::use_native_awaitable);

  assert(nbytes == rdmapp::remote_mr::kSerializedSize);
  (void)nbytes;

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

cppcoro::task<void> qp_handler(std::shared_ptr<rdmapp::qp> qp) {
  std::array<std::byte, kMessageSize> buffer;
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());
  auto local_mr_serialized = local_mr.serialize();
  co_await qp->send(local_mr_serialized, rdmapp::use_native_awaitable);

  spdlog::info("server: local buffer sent");

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;
  for (int i = 0; i < kSendCount; i++) {
    co_await qp->recv(rdmapp::use_native_awaitable);

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[write_with_imm/recv] batch {:7d}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
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
  last_batch_time = start_time;

  for (int i = 0; i < kSendCount; i++) {
    co_await qp->recv(local_mr, rdmapp::use_native_awaitable);

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[send/recv] batch {:7d}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  end_time = std::chrono::high_resolution_clock::now();

  total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[send/recv] total operations: {}", kSendCount);
  spdlog::info("[send/recv] total time: {}us", total_duration.count());
  spdlog::info("[send/recv] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / kSendCount);
}

cppcoro::task<void> server(rdmapp::native_qp_acceptor &acceptor) {
  cppcoro::async_scope scope;
  while (true) {
    auto qp = co_await acceptor.accept();
    scope.spawn(qp_handler(qp));
  }
  co_await scope.join();
}

cppcoro::task<void> client(rdmapp::native_qp_connector &connector,
                           std::string_view host, uint16_t port) {
  auto qp = co_await connector.connect(host, port);
  co_await client_worker(qp);
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);

  auto io_service = cppcoro::io_service(1);
  auto scheduler = std::make_shared<rdmapp::basic_scheduler>();
  std::jthread w([&]() { io_service.process_events(); });
  std::jthread s([=]() { scheduler->run(); });

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
    io_service.stop();
    scheduler->stop();
  }

  default: {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
  }
  }

  io_service.stop();
  scheduler->stop();
  return 0;
}
