#include "coro_rpc.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include "spdlog/common.h"
#include "spdlog/spdlog.h"
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <cppcoro/async_scope.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>
#include <cstring>
#include <iostream>
#include <memory>

#include <rdmapp/cq_poller.h>
#include <rdmapp/log.h>

#ifdef RDMAPP_BUILD_ASAN
#include <sanitizer/lsan_interface.h>

void signal_handler(int) {
  spdlog::info("SIGINT: ASAN check begin");
  // __lsan_do_recoverable_leak_check();
  __lsan_do_leak_check();
  spdlog::info("SIGINT: ASAN check done");
  exit(-1);
}
#endif

constexpr int kSendCount = 1000 * 1000;
#ifdef RDMAPP_BUILD_DEBUG
constexpr int kBatchSize = 100'000;
#else
constexpr int kBatchSize = 400'000;
#endif

int kConcurrency = 16;
int kQP = 4;

struct statistic {
  long long total_operations = 0;
  long long total_duration_us = 0;
  size_t send_msg_size_bytes = kSendMsgSize;
  size_t recv_msg_size_bytes = kRecvMsgSize;
} g_stat_coro, g_stat_sync;

void print_stats(std::string_view prefix, long long total_operations,
                 long long total_duration_us, size_t send_msg_size_bytes,
                 size_t recv_msg_size_bytes) noexcept {
  if (!total_duration_us)
    exit(-10);

  double total_duration_s = total_duration_us / 1'000'000.0;
  // IOPS
  double iops = static_cast<double>(total_operations) / total_duration_s;

  // Bandwidth
  size_t single_op_total_bytes = send_msg_size_bytes + recv_msg_size_bytes;
  long long total_data_bytes = total_operations * single_op_total_bytes;

  // BW in GB/s (using 1024^3 Bytes for 1GB)
  double bw_gb_per_s = static_cast<double>(total_data_bytes) /
                       total_duration_s / (1024.0 * 1024.0 * 1024.0);

  // BW in Gbps (using 10^9 bits for 1Gbit)
  long long total_data_bits = total_data_bytes * 8;
  double bw_gbps =
      static_cast<double>(total_data_bits) / total_duration_s / 1'000'000'000.0;

  spdlog::info("--- {} Statistics ---", prefix);
  spdlog::info("  Total operations: {}", total_operations);
  spdlog::info("  Overall elapsed time: {} us", total_duration_us);
  spdlog::info("  Achieved IOPS: {:.2f} kIOPS ({:.0f} OPS/s)", iops / 1000.0,
               iops);
  spdlog::info("  Achieved Bandwidth: {:.3f} GB/s", bw_gb_per_s);
  spdlog::info("  Achieved Bandwidth: {:.3f} Gbps", bw_gbps);
}

void print_stats(std::string_view prefix, statistic const &stat) noexcept {
  print_stats(prefix, stat.total_operations, stat.total_duration_us,
              stat.send_msg_size_bytes, stat.recv_msg_size_bytes);
}

void sync_rpc_test_worker(RpcClientMux &client) {
  spdlog::info("sync_rpc_worker started");
  // 构造一些测试数据
  std::vector<std::byte> req_vec(kSendMsgSize, std::byte{0x01});
  std::vector<std::byte> resp_vec(kRecvMsgSize, std::byte{0x00});

  std::span<std::byte> req_span(req_vec);
  std::span<std::byte> resp_span(resp_vec);

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;

  // 发送循环
  for (int i = 0; i < kSendCount; i++) {
    size_t n = cppcoro::sync_wait(client.call(req_span, resp_span));

    // 简单验证
    if (n != resp_span.size()) {
      spdlog::error("resp length mismatch: expect {} got {}", resp_span.size(),
                    n);
    }
    if (resp_span[0] != std::byte{0xEE}) {
      spdlog::error("Content mismatch: expect 0xEE got {}",
                    (uint8_t)resp_span[0]);
    }

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[rpc_call] batch {:7d}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[rpc_call] total operations: {}", kSendCount);
  spdlog::info("[rpc_call] total time: {}us", total_duration.count());
  spdlog::info("[rpc_call] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / kSendCount);
  g_stat_sync.total_duration_us = total_duration.count();
  g_stat_sync.total_operations = kSendCount * kConcurrency;
}

cppcoro::task<void> rpc_test_worker(RpcClientMux &client) {
  // 构造一些测试数据
  std::vector<std::byte> req_vec(kSendMsgSize, std::byte{0x01});
  std::vector<std::byte> resp_vec(kRecvMsgSize, std::byte{0x00});

  std::span<std::byte> req_span(req_vec);
  std::span<std::byte> resp_span(resp_vec);

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;

  // 发送循环
  for (int i = 0; i < kSendCount; i++) {
    size_t n = co_await client.call(req_span, resp_span);

    // 简单验证
    if (n != resp_span.size()) {
      spdlog::error("len mismatch: expect {} got {}", resp_span.size(), n);
    }
    if (resp_span[0] != std::byte{0xEE}) {
      spdlog::error("Content mismatch: expect 0xEE got {}",
                    (uint8_t)resp_span[0]);
    }

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[rpc_call] batch {:7d}: avg latency {:.3f}us", i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[rpc_call] total operations: {}", kSendCount);
  spdlog::info("[rpc_call] total time: {}us", total_duration.count());
  spdlog::info("[rpc_call] average time per operation: {:.3f}us",
               static_cast<double>(total_duration.count()) / kSendCount);
  g_stat_coro.total_duration_us = total_duration.count();
  g_stat_coro.total_operations = kSendCount * kConcurrency;
}

cppcoro::task<void> run_client(std::vector<std::shared_ptr<rdmapp::qp>> qp) {
  RpcClientMux client{std::move(qp), 512};

  std::vector<cppcoro::task<void>> tasks;
  for (int i = 0; i < kConcurrency; i++) {
    tasks.emplace_back(rpc_test_worker(client));
  }

  cppcoro::sync_wait(cppcoro::when_all(std::move(tasks)));
  spdlog::info("run_client: all {} call tasks done", kConcurrency);

  std::vector<std::jthread> workers_;
  for (int i = 0; i < kConcurrency; i++) {
    workers_.emplace_back(sync_rpc_test_worker, std::ref(client));
  }
  workers_.clear();
  spdlog::info("run_client: all {} call threads done", kConcurrency);
  print_stats("coro", g_stat_coro);
  print_stats("sync", g_stat_sync);
  co_return;
}

void client(rdmapp::native_qp_connector &connector) {
  asio::io_context io_ctx(1);
  std::vector<std::shared_ptr<rdmapp::qp>> qp_vec;
  auto guard = asio::make_work_guard(io_ctx);
  std::jthread w([&]() { io_ctx.run(); });
  for (int i = 0; i < kQP; i++) {
    auto qp_fut = asio::co_spawn(io_ctx, connector.connect(), asio::use_future);
    auto qp = qp_fut.get();
    spdlog::info("connected: qp={}", fmt::ptr(qp.get()));
    qp_vec.push_back(qp);
  }
  cppcoro::sync_wait(run_client(std::move(qp_vec)));
}

void server(asio::io_context &io_ctx, rdmapp::native_qp_acceptor &acceptor) {
  spdlog::info("Server waiting for connection...");

  auto guard = asio::make_work_guard(io_ctx);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  cppcoro::async_scope scope;
  std::list<RpcServer> servers_;
  int cnt = 1;
  while (true) {
    auto qp = asio::co_spawn(io_ctx, acceptor.accept(), asio::use_future).get();
    auto &it = servers_.emplace_back(qp, 2048);
    scope.spawn(it.run());
    spdlog::info("qp served: nr = {}", cnt++);
  }
  guard.reset();
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_ASAN
  signal(SIGINT, &signal_handler);
#endif

#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
  spdlog::set_level(spdlog::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  auto device = std::make_shared<rdmapp::device>(1, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);

  switch (argc) {
  case 2: {
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto io_ctx = std::make_shared<asio::io_context>(1);
    auto acceptor = rdmapp::native_qp_acceptor(io_ctx, port, pd);
    server(*io_ctx, acceptor);
    break;
  }

  case 4: {
    kConcurrency = std::stoi(argv[3]);
    auto connector =
        rdmapp::native_qp_connector(argv[1], std::stoi(argv[2]), pd);
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
