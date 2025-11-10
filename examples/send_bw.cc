#include "asio/awaitable.hpp"
#include "asio/co_spawn.hpp"
#include "asio/detached.hpp"
#include "asio/thread_pool.hpp"
#include "listener.h"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>

#include <rdmapp/rdmapp.h>

using namespace std::literals::chrono_literals;

constexpr size_t kBufferSizeBytes = 12 * 1024 * 1024; // 2 MB
constexpr size_t kWorkerCount = 8;
constexpr size_t kSendCount = 8 * 1024;
constexpr size_t kPrintInterval = 1024;
constexpr size_t kTotalSizeBytes = kBufferSizeBytes * kSendCount * kWorkerCount;
constexpr size_t kGlobalThread = 16;

template <bool Client = false>
asio::awaitable<void> worker(size_t id, std::shared_ptr<rdmapp::qp> qp) {
  std::vector<uint8_t> buffer;
  buffer.resize(kBufferSizeBytes);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(&buffer[0], buffer.size()));
  spdlog::info("worker: {} started", id);
  for (size_t i = 0; i < kSendCount; ++i) {
    if constexpr (Client) {
      co_await qp->recv(local_mr);
    } else {
      co_await qp->send(local_mr);
    }
    if ((i + 1) % kPrintInterval == 0) {
      spdlog::info("worker {}: {} {} times", id, Client ? "recv" : "sent",
                   i + 1);
    }
  }
  spdlog::info("worker {} exited", id);
  co_return;
}

template <bool Client = false>
asio::awaitable<void> handler(std::shared_ptr<rdmapp::qp> qp) {
  auto executor = co_await asio::this_coro::executor;
  std::vector<std::future<void> *> futures;
  for (size_t i = 0; i < kWorkerCount; ++i) {
    asio::co_spawn(executor, worker<Client>(i, qp), asio::detached);
  }
  co_return;
}

asio::awaitable<void> client(rdmapp::qp_connector &connector) {
  auto qp = co_await connector.connect();
  co_await handler<true>(qp);
  co_return;
}

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::info);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(
      kGlobalThread); // NOTE:  thread for global io context
  auto executor = std::make_shared<rdmapp::executor>(io_ctx);
  auto cq_poller = std::make_shared<rdmapp::cq_poller>(cq, executor);

  asio::thread_pool pool(kGlobalThread);

  if (argc == 2) {
    auto work_guard = asio::make_work_guard(*io_ctx);
    auto l = std::make_shared<rdmapp::listener>(std::stoi(argv[1]));
    auto acc = std::make_shared<rdmapp::qp_acceptor>(pd, cq);
    auto f = [acc](asio::ip::tcp::socket socket) -> asio::awaitable<void> {
      auto qp = co_await acc->accept(std::move(socket));
      co_await handler<false>(qp);
    };
    l->listen_and_serve(*io_ctx, std::move(f));
    for (size_t i = 0; i < kGlobalThread; i++)
      asio::post(pool, [i, io_ctx]() {
        spdlog::info("thread {} start", i);
        io_ctx->run();
      });
    pool.join();

  } else if (argc == 3) {
    auto connector = rdmapp::qp_connector(argv[1], std::stoi(argv[2]), pd, cq);
    asio::co_spawn(*io_ctx, client(connector), asio::detached);

    auto tik = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < kGlobalThread; i++)
      asio::post(pool, [i, io_ctx]() {
        spdlog::info("thread {} start", i);
        io_ctx->run();
      });
    pool.join();
    auto tok = std::chrono::high_resolution_clock::now();
    spdlog::info("client exit after communicated with {}:{}", argv[1], argv[2]);

    std::chrono::duration<double> seconds = tok - tik;
    double mb = static_cast<double>(kTotalSizeBytes) / 1024 / 1024;
    double throughput = mb / seconds.count();
    spdlog::info("total: {} MB, elapsed: {} s, throughput: {}", mb,
                 seconds.count(), throughput);

  } else {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
    ::exit(-1);
  }
  return 0;
}
