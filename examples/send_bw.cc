#include <spdlog/spdlog.h>

#include "payload_size.h"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <cppcoro/async_scope.hpp>
#include <cppcoro/io_service.hpp>
#include <cppcoro/net/socket.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/when_all.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <rdmapp/completion_token.h>
#include <rdmapp/cq_poller.h>
#include <rdmapp/log.h>
#include <rdmapp/qp.h>

#ifdef RDMAPP_BUILD_DEBUG
constexpr int kSendCount = 10000;
constexpr int kBatchSize = 500;
#else
constexpr int kSendCount = 1024 * 1024 * 1024;
constexpr int kBatchSize = 10000;
#endif

constexpr int kRecvDepth = 32;
constexpr std::size_t kDefaultPayloadSize = 2 * 1024 * 1024;

struct Stats {
  std::atomic<size_t> total_completed{0};
  std::atomic<size_t> total_bytes{0};
} g_stats;

// 报告线程函数
void reporter_loop(std::string_view role) {
  auto last_time = std::chrono::steady_clock::now();

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    auto now = std::chrono::steady_clock::now();
    double duration = std::chrono::duration<double>(now - last_time).count();

    size_t diff_count = g_stats.total_completed.exchange(0);
    size_t diff_bytes = g_stats.total_bytes.exchange(0);

    double iops = diff_count / duration;
    double throughput_gbps =
        (diff_bytes * 8.0 / (1000.0 * 1000 * 1000)) / duration;

    spdlog::info("[{}] IOPS: {:10.2f} ops/s | BW: {:10.2f} Gbps", role, iops,
                 throughput_gbps);
    last_time = now;
  }
}

class Server {
  std::shared_ptr<rdmapp::qp> qp_;
  std::size_t payload_size_;
  std::size_t recv_depth_;

  // 统一的大块内存池，避免频繁注册 MR
  std::vector<std::byte> buffer_pool_;
  rdmapp::local_mr mr_;

public:
  Server(std::shared_ptr<rdmapp::qp> qp, std::size_t payload_size,
         std::size_t recv_depth)
      : qp_(qp), payload_size_(payload_size), recv_depth_(recv_depth),
        buffer_pool_(recv_depth_ * payload_size_),
        mr_(qp->pd_ptr()->reg_mr(buffer_pool_.data(), buffer_pool_.size())) {
    spdlog::info(
        "server: initialized with {} recv workers, payload {} bytes",
        recv_depth_, payload_size_);
  }

  cppcoro::task<void> run() {
    std::vector<cppcoro::task<void>> workers;
    workers.reserve(recv_depth_);

    for (size_t i = 0; i < recv_depth_; ++i) {
      workers.emplace_back(server_worker(i));
    }
    co_await cppcoro::when_all(std::move(workers));
  }

private:
  // 单个 Worker：负责一个 Slot 的 接收 -> 处理 -> 发送 循环
  cppcoro::task<void> server_worker(size_t idx) {
    size_t offset = idx * payload_size_;
    spdlog::info("server_worker {} spawn", idx);
    while (true) {
      auto recv_view = rdmapp::mr_view(mr_, offset, payload_size_);
      co_await qp_->recv(recv_view);
      g_stats.total_completed += 1;
      g_stats.total_bytes += payload_size_;
    }
  }
};

cppcoro::task<void> send_worker(int idx, std::shared_ptr<rdmapp::qp> qp,
                                std::size_t payload_size,
                                std::size_t count) {
  spdlog::info("send_worker {} spawn", idx);

  // 构造一些测试数据
  std::vector<std::byte> req_vec(payload_size, std::byte{0x01});
  std::span<std::byte> req_span(req_vec);
  auto local_mr = qp->pd_ptr()->reg_mr(req_span.data(), req_span.size());

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;

  // 发送循环
  for (std::size_t i = 0; i < count; i++) {
    co_await qp->send(local_mr);

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[send:{:>2}] {:7} op: avg latency {:.3f}us", idx, i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[send:{:>2}] total operations: {}", idx, count);
  spdlog::info("[send:{:>2}] total time: {}us", idx, total_duration.count());
  spdlog::info("[send:{:>2}] average time per operation: {:.3f}us", idx,
               static_cast<double>(total_duration.count()) / count);
}

cppcoro::task<void> client(auto &connector, std::string_view hostname,
                           uint16_t port, std::size_t payload_size,
                           std::size_t count, std::size_t threads) {
  std::vector<std::shared_ptr<rdmapp::qp>> qps;
  qps.reserve(threads);

  for (std::size_t i = 0; i < threads; i++) {
    qps.emplace_back(co_await connector.connect(hostname, port));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::vector<cppcoro::task<void>> tasks;
  tasks.reserve(threads);

  for (std::size_t i = 0; i < threads; i++) {
    tasks.emplace_back(send_worker(static_cast<int>(i), qps[i], payload_size,
                                   count));
  }

  co_await cppcoro::when_all(std::move(tasks));

  co_return;
}

cppcoro::task<void> server(rdmapp::native_qp_acceptor &acceptor,
                           std::size_t payload_size,
                           std::size_t recv_depth) {
  cppcoro::async_scope scope;
  std::jthread reporter(reporter_loop, "server");
  std::vector<std::shared_ptr<Server>> servers;

  while (true) {
    spdlog::info("server waiting for connection...");
    auto qp = co_await acceptor.accept();
    auto server = std::make_shared<Server>(qp, payload_size, recv_depth);
    servers.push_back(server);
    scope.spawn(server->run());
  }

  co_await scope.join();
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
  spdlog::set_level(spdlog::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  // NOTE: send_bw uses the second local device in this testbed.
  auto device = std::make_shared<rdmapp::device>(1, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);

  auto io_service = cppcoro::io_service(1);
  auto scheduler = std::make_shared<rdmapp::basic_scheduler>();

  examples::payload_size_args args;
  try {
    args = examples::parse_payload_size_args(argc, argv, kDefaultPayloadSize,
                                             kSendCount, 1, 1, kRecvDepth);
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

  spdlog::info("payload size: {} bytes, count: {}, threads: {}, scheduler "
               "threads: {}, recv depth: {}",
               args.payload_size, args.count, args.threads,
               args.scheduler_threads, args.recv_depth);

  switch (args.positional.size()) {
  case 1: {
    uint16_t port = (uint16_t)std::stoi(std::string(args.positional[0]));
    auto acceptor = rdmapp::qp_acceptor(io_service, scheduler, port, pd);
    cppcoro::sync_wait(server(acceptor, args.payload_size, args.recv_depth));
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
