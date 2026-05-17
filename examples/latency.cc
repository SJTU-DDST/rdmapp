
#include <spdlog/spdlog.h>

#include "payload_size.h"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <chrono>
#include <cppcoro/async_scope.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>
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
#include <vector>

constexpr std::size_t kDefaultPayloadSize = 4096;
constexpr int kBatchSize = 100'000;
#ifdef RDMAPP_BUILD_DEBUG
constexpr int kSendCount = 100;
#else
constexpr int kSendCount = 1000'000;
#endif

cppcoro::task<void> client_worker(std::shared_ptr<rdmapp::qp> qp,
                                  std::size_t payload_size,
                                  std::size_t count) {
  std::vector<std::byte> buffer(payload_size);
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());

  auto remote_mr_serialized =
      rdmapp::mr_view(local_mr, 0, rdmapp::remote_mr::kSerializedSize);
  auto [nbytes, _] = co_await qp->recv(remote_mr_serialized);

  assert(nbytes == rdmapp::remote_mr::kSerializedSize);
  (void)nbytes;

  rdmapp::remote_mr remote_mr =
      rdmapp::remote_mr::deserialize(remote_mr_serialized.span().data());

  spdlog::info("client: remote buffer recv, size={}", remote_mr.length());

  std::fill(buffer.begin(), buffer.end(), std::byte(0xdd));

  for (std::size_t i = 0; i < count; i++) {
    std::size_t nbytes [[maybe_unused]] =
        co_await qp->write_with_imm(remote_mr, local_mr, i);
  }

  for (std::size_t i = 0; i < count; i++) {
    std::size_t nbytes [[maybe_unused]] = co_await qp->send(local_mr);
  }

  auto [ack_nbytes, ack_imm] = co_await qp->recv(remote_mr_serialized);
  assert(ack_nbytes == rdmapp::remote_mr::kSerializedSize);
  assert(!ack_imm);
  (void)ack_nbytes;
  (void)ack_imm;

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;
  for (std::size_t i = 0; i < count; i++) {
    std::size_t nbytes [[maybe_unused]] = co_await qp->read(remote_mr, local_mr);

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[read] batch {:7}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[read] total operations: {}", count);
  spdlog::info("[read] total time: {}us", total_duration.count());
  spdlog::info("[read] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / count);

  co_await qp->send(local_mr);
}

cppcoro::task<void> qp_handler(std::shared_ptr<rdmapp::qp> qp,
                               std::size_t payload_size, std::size_t count) {
  std::vector<std::byte> buffer(payload_size);
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());
  auto local_mr_serialized = local_mr.serialize();
  co_await qp->send(local_mr_serialized);

  spdlog::info("server: local buffer sent");

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;
  for (std::size_t i = 0; i < count; i++) {
    co_await qp->recv();

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[write_with_imm/recv] batch {:7}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[write_with_imm/recv] total operations: {}", count);
  spdlog::info("[write_with_imm/recv] total time: {}us",
               total_duration.count());
  spdlog::info("[write_with_imm/recv] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / count);

  start_time = std::chrono::high_resolution_clock::now();
  last_batch_time = start_time;

  for (std::size_t i = 0; i < count; i++) {
    co_await qp->recv(local_mr);

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[send/recv] batch {:7}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  end_time = std::chrono::high_resolution_clock::now();

  total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[send/recv] total operations: {}", count);
  spdlog::info("[send/recv] total time: {}us", total_duration.count());
  spdlog::info("[send/recv] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / count);

  co_await qp->send(local_mr_serialized);
  co_await qp->recv(local_mr);
}

cppcoro::task<void> server(rdmapp::native_qp_acceptor &acceptor,
                           std::size_t payload_size, std::size_t count) {
  cppcoro::async_scope scope;
  while (true) {
    auto qp = co_await acceptor.accept();
    scope.spawn(qp_handler(qp, payload_size, count));
  }
  co_await scope.join();
}

cppcoro::task<void> client(rdmapp::native_qp_connector &connector,
                           std::string_view host, uint16_t port,
                           std::size_t payload_size, std::size_t count,
                           std::size_t threads) {
  std::vector<cppcoro::task<void>> tasks;
  tasks.reserve(threads);

  for (std::size_t i = 0; i < threads; ++i) {
    auto qp = co_await connector.connect(host, port);
    tasks.emplace_back(client_worker(qp, payload_size, count));
  }

  co_await cppcoro::when_all(std::move(tasks));
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  auto device = std::make_shared<rdmapp::device>(rdmapp::auto_select);
  auto pd = std::make_shared<rdmapp::pd>(device);

  auto io_service = cppcoro::io_service(1);
  auto scheduler = std::make_shared<rdmapp::basic_scheduler>();

  examples::payload_size_args args;
  try {
    args = examples::parse_payload_size_args(argc, argv, kDefaultPayloadSize,
                                             kSendCount);
  } catch (std::exception const &e) {
    std::cerr << e.what() << std::endl;
    return 1;
  }

  std::jthread w([&]() { io_service.process_events(); });
  std::vector<std::jthread> scheduler_threads;
  scheduler_threads.reserve(args.scheduler_threads);
  for (std::size_t i = 0; i < args.scheduler_threads; ++i) {
    scheduler_threads.emplace_back([=]() { scheduler->run(); });
  }

  spdlog::info(
      "payload size: {} bytes, count: {}, threads: {}, scheduler threads: {}",
      args.payload_size, args.count, args.threads, args.scheduler_threads);

  switch (args.positional.size()) {
  case 1: {

    uint16_t port = (uint16_t)std::stoi(std::string(args.positional[0]));
    auto acceptor = rdmapp::qp_acceptor(io_service, scheduler, port, pd);
    cppcoro::sync_wait(server(acceptor, args.payload_size, args.count));
    break;
  }

  case 2: {
    uint16_t port = (uint16_t)std::stoi(std::string(args.positional[1]));
    std::string_view hostname = args.positional[0];
    auto connector = rdmapp::qp_connector(io_service, scheduler, pd);
    cppcoro::sync_wait(
        client(connector, hostname, port, args.payload_size, args.count,
               args.threads));
    spdlog::info("client exit after communicated with {}:{}", hostname, port);
    break;
  }

  default: {
    std::cout << "Usage: " << argv[0] << examples::payload_size_usage()
              << " [port] for server and " << argv[0]
              << examples::payload_size_usage()
              << " [server_ip] [port] for client" << std::endl;
  }
  }

  io_service.stop();
  scheduler->stop();
  return 0;
}
