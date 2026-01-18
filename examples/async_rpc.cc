#include "coro_rpc.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include "spdlog/common.h"
#include "spdlog/spdlog.h"
#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/use_future.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>
#include <cstring>
#include <iostream>

#include <rdmapp/cq_poller.h>
#include <rdmapp/log.h>

constexpr int kSendCount = 2 * 1000 * 1000;
constexpr int kBatchSize = 10000;

constexpr int kConcurrency = 4;

void call_loop() {}

void sync_rpc_test_worker(RpcClient &client) {
  spdlog::info("sync_rpc_worker started");
  // 构造一些测试数据
  std::vector<std::byte> req_vec(kMsgSize - sizeof(RpcHeader), std::byte{0x01});
  std::vector<std::byte> resp_vec(kMsgSize, std::byte{0x00});

  std::span<std::byte> req_span(req_vec);
  std::span<std::byte> resp_span(resp_vec);

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;

  // 发送循环
  for (int i = 0; i < kSendCount; i++) {
    size_t n = cppcoro::sync_wait(client.call(req_span, resp_span));

    // 简单验证
    if (n != req_span.size()) {
      spdlog::error("Len mismatch: expect {} got {}", req_span.size(), n);
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
}

cppcoro::task<void> rpc_test_worker(RpcClient &client) {
  // 构造一些测试数据
  std::vector<std::byte> req_vec(kMsgSize - sizeof(RpcHeader), std::byte{0x01});
  std::vector<std::byte> resp_vec(kMsgSize, std::byte{0x00});

  std::span<std::byte> req_span(req_vec);
  std::span<std::byte> resp_span(resp_vec);

  auto start_time = std::chrono::high_resolution_clock::now();
  auto last_batch_time = start_time;

  // 发送循环
  for (int i = 0; i < kSendCount; i++) {
    size_t n = co_await client.call(req_span, resp_span);

    // 简单验证
    if (n != req_span.size()) {
      spdlog::error("Len mismatch: expect {} got {}", req_span.size(), n);
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
}

cppcoro::task<void> run_client(std::shared_ptr<rdmapp::qp> qp) {
  RpcClient client{qp};

  cppcoro::static_thread_pool tp(4);
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
  co_return;
}

void client(rdmapp::qp_connector &connector) {
  asio::io_context io_ctx(1);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  auto qp_fut = asio::co_spawn(io_ctx, connector.connect(), asio::use_future);
  io_ctx.run();
  cppcoro::sync_wait(run_client(qp_fut.get()));
}

void server(asio::io_context &io_ctx, rdmapp::qp_acceptor &acceptor) {
  spdlog::info("Server waiting for connection...");

  auto guard = asio::make_work_guard(io_ctx);
  std::jthread w([&io_ctx]() { io_ctx.run(); });
  auto qp = asio::co_spawn(io_ctx, acceptor.accept(), asio::use_future).get();
  guard.reset();

  RpcServer server(qp);
  cppcoro::sync_wait(server.run());
}

int main(int argc, char *argv[]) {
#ifdef RDMAPP_BUILD_DEBUG
  rdmapp::log::setup(rdmapp::log::level::debug);
  spdlog::set_level(spdlog::level::debug);
#else
  rdmapp::log::setup(rdmapp::log::level::info);
#endif
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto cq_poller = std::make_unique<rdmapp::native_cq_poller>(cq);
  auto cq2 = std::make_shared<rdmapp::cq>(device);
  auto cq_poller2 = std::make_unique<rdmapp::native_cq_poller>(cq2);

  switch (argc) {
  case 2: {
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto io_ctx = std::make_shared<asio::io_context>(1);
    auto acceptor = rdmapp::qp_acceptor(io_ctx, port, pd, cq, cq2);
    server(*io_ctx, acceptor);
    break;
  }

  case 3: {
    auto connector =
        rdmapp::qp_connector(argv[1], std::stoi(argv[2]), pd, cq, cq2);
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
