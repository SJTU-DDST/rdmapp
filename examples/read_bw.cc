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

constexpr std::size_t kDefaultPayloadSize = 4 * 1024 * 1024;
#ifdef RDMAPP_BUILD_DEBUG
constexpr int kReadCount = 100;
#else
constexpr int kReadCount = 3000;
#endif

cppcoro::task<void> server_worker(std::shared_ptr<rdmapp::qp> qp,
                                  std::size_t payload_size) {
  std::vector<std::byte> buffer(payload_size, std::byte{0xdd});
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());
  auto local_mr_serialized = local_mr.serialize();
  co_await qp->send(local_mr_serialized);

  std::vector<std::byte> done_buffer(1);
  auto done_mr = qp->pd_ptr()->reg_mr(done_buffer.data(), done_buffer.size());
  co_await qp->recv(done_mr);
}

cppcoro::task<void> server(rdmapp::native_qp_acceptor &acceptor,
                           std::size_t payload_size) {
  cppcoro::async_scope scope;
  while (true) {
    auto qp = co_await acceptor.accept();
    scope.spawn(server_worker(qp, payload_size));
  }
  co_await scope.join();
}

cppcoro::task<double> read_worker(std::size_t idx, std::shared_ptr<rdmapp::qp> qp,
                                  std::size_t payload_size,
                                  std::size_t count) {
  std::vector<std::byte> buffer(payload_size);
  rdmapp::local_mr local_mr =
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size());

  auto remote_mr_serialized =
      rdmapp::mr_view(local_mr, 0, rdmapp::remote_mr::kSerializedSize);
  auto [nbytes, imm] = co_await qp->recv(remote_mr_serialized);
  assert(nbytes == rdmapp::remote_mr::kSerializedSize);
  assert(!imm);
  (void)nbytes;
  (void)imm;

  rdmapp::remote_mr remote_mr =
      rdmapp::remote_mr::deserialize(remote_mr_serialized.span().data());

  auto start_time = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < count; i++) {
    std::size_t read_bytes [[maybe_unused]] =
        co_await qp->read(remote_mr, local_mr);
  }
  auto end_time = std::chrono::steady_clock::now();
  auto duration = std::chrono::duration<double>(end_time - start_time);
  auto total_bytes = static_cast<double>(payload_size) * count;
  auto bandwidth_gbps = total_bytes * 8.0 / duration.count() / 1e9;
  spdlog::info("[read:{:>2}] total operations: {}", idx, count);
  spdlog::info("[read:{:>2}] total time: {:.6f}s", idx, duration.count());
  spdlog::info("[read:{:>2}] bandwidth: {:.3f} Gbps", idx, bandwidth_gbps);

  std::vector<std::byte> done_buffer(1);
  auto done_mr = qp->pd_ptr()->reg_mr(done_buffer.data(), done_buffer.size());
  co_await qp->send(done_mr);

  co_return bandwidth_gbps;
}

cppcoro::task<void> client(rdmapp::native_qp_connector &connector,
                           std::string_view host, uint16_t port,
                           std::size_t payload_size, std::size_t count,
                           std::size_t threads) {
  std::vector<std::shared_ptr<rdmapp::qp>> qps;
  qps.reserve(threads);

  for (std::size_t i = 0; i < threads; ++i) {
    qps.emplace_back(co_await connector.connect(host, port));
  }

  std::vector<cppcoro::task<double>> tasks;
  tasks.reserve(threads);

  for (std::size_t i = 0; i < threads; ++i) {
    tasks.emplace_back(read_worker(i, qps[i], payload_size, count));
  }

  auto bandwidths = co_await cppcoro::when_all(std::move(tasks));
  double total_bandwidth_gbps = 0.0;
  for (auto bandwidth : bandwidths) {
    total_bandwidth_gbps += bandwidth;
  }
  spdlog::info("[read] aggregate bandwidth: {:.3f} Gbps",
               total_bandwidth_gbps);
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  // NOTE: read_bw uses the second local device in this testbed.
  auto device = std::make_shared<rdmapp::device>(1, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);

  auto io_service = cppcoro::io_service(1);
  auto scheduler = std::make_shared<rdmapp::basic_scheduler>();

  examples::payload_size_args args;
  try {
    args = examples::parse_payload_size_args(argc, argv, kDefaultPayloadSize,
                                             kReadCount);
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
    cppcoro::sync_wait(server(acceptor, args.payload_size));
    break;
  }

  case 2: {
    uint16_t port = (uint16_t)std::stoi(std::string(args.positional[1]));
    std::string_view hostname = args.positional[0];
    auto connector = rdmapp::qp_connector(io_service, scheduler, pd);
    cppcoro::sync_wait(client(connector, hostname, port, args.payload_size,
                              args.count, args.threads));
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
