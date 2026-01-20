#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/when_all.hpp>
#include <cstring>
#include <iostream>
#include <spdlog/spdlog.h>

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

constexpr int kConcurrency = 4;
constexpr int kRecvDepth = 32;
constexpr std::size_t kMsgSize = 2 * 1024 * 1024;

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

  // 统一的大块内存池，避免频繁注册 MR
  std::vector<std::byte> buffer_pool_;
  rdmapp::local_mr mr_;

public:
  Server(std::shared_ptr<rdmapp::qp> qp)
      : qp_(qp), buffer_pool_(kRecvDepth * kMsgSize),
        mr_(qp->pd_ptr()->reg_mr(buffer_pool_.data(), buffer_pool_.size())) {
    spdlog::info("server: initialized with {} concurrent workers", kRecvDepth);
  }

  cppcoro::task<void> run() {
    std::vector<cppcoro::task<void>> workers;
    workers.reserve(kRecvDepth);

    for (size_t i = 0; i < kRecvDepth; ++i) {
      workers.emplace_back(server_worker(i));
    }
    std::jthread reporter(reporter_loop, "server");
    co_await cppcoro::when_all(std::move(workers));
  }

private:
  // 单个 Worker：负责一个 Slot 的 接收 -> 处理 -> 发送 循环
  cppcoro::task<void> server_worker(size_t idx) {
    size_t offset = idx * kMsgSize;
    spdlog::info("server_worker {} spawn", idx);
    while (true) {
      auto recv_view = rdmapp::mr_view(mr_, offset, kMsgSize);
      co_await qp_->recv(recv_view, rdmapp::use_native_awaitable);
      g_stats.total_completed += 1;
      g_stats.total_bytes += kMsgSize;
    }
  }
};

cppcoro::task<void> send_worker(int idx, std::shared_ptr<rdmapp::qp> qp) {
  spdlog::info("send_worker {} spawn", idx);

  // 构造一些测试数据
  std::vector<std::byte> req_vec(kMsgSize, std::byte{0x01});
  std::span<std::byte> req_span(req_vec);
  auto local_mr = qp->pd_ptr()->reg_mr(req_span.data(), req_span.size());

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;

  // 发送循环
  for (int i = 0; i < kSendCount; i++) {
    co_await qp->send(local_mr, rdmapp::use_native_awaitable);

    if (i && (i % kBatchSize == 0)) {
      auto now = std::chrono::high_resolution_clock::now();
      auto batch_duration =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_batch_time);
      spdlog::info("[send:{:>2}] {:7d} op: avg latency {:.3f}us", idx, i,
                   static_cast<double>(batch_duration.count()) / kBatchSize);
      last_batch_time = now;
    }
  }
  auto end_time = std::chrono::high_resolution_clock::now();
  auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);
  spdlog::info("[send:{:>2}] total operations: {}", idx, kSendCount);
  spdlog::info("[send:{:>2}] total time: {}us", idx, total_duration.count());
  spdlog::info("[send:{:>2}] average time per operation: {:.3f}us", idx,
               static_cast<double>(total_duration.count()) / kSendCount);
}

cppcoro::task<void> run_client(std::shared_ptr<rdmapp::qp> qp) {
  std::vector<cppcoro::task<void>> tasks;
  for (int i = 0; i < kConcurrency; i++) {
    tasks.emplace_back(send_worker(i, qp));
  }

  co_await cppcoro::when_all(std::move(tasks));

  co_return;
}

void client(rdmapp::native_qp_connector &connector) {
  asio::io_context io_ctx(1);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  auto qp_fut = asio::co_spawn(io_ctx, connector.connect(), asio::use_future);
  io_ctx.run();
  cppcoro::sync_wait(run_client(qp_fut.get()));
}

void server(asio::io_context &io_ctx, rdmapp::native_qp_acceptor &acceptor) {
  spdlog::info("server waiting for connection...");

  auto guard = asio::make_work_guard(io_ctx);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  auto qp = asio::co_spawn(io_ctx, acceptor.accept(), asio::use_future).get();
  guard.reset();

  Server server(qp);
  cppcoro::sync_wait(server.run());
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
  spdlog::set_level(spdlog::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  // NOTE: to use first card, use (0,1) but not (1,1) here
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

  case 3: {
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
