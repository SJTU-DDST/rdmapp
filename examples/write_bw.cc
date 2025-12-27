#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/signal_set.hpp>
#include <asio/thread_pool.hpp>
#include <cassert>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <stop_token>
#include <string>
#include <thread>

#include <rdmapp/rdmapp.h>

using namespace std::literals::chrono_literals;

constexpr size_t kBufferSizeBytes = 2 * 1024 * 1024;
constexpr size_t kSendCount = 1024 * 1024 * 1024;
constexpr size_t kQP = 3;
std::array<std::atomic<size_t>, kQP> gSendCounts;

asio::awaitable<void> client_worker(std::shared_ptr<rdmapp::qp> qp, int id) {
  std::vector<std::byte> buffer;
  buffer.resize(kBufferSizeBytes);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size()));
  std::byte remote_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  auto remote_mr_serialized_data =
      std::as_writable_bytes(std::span(remote_mr_serialized));
  co_await qp->recv(remote_mr_serialized_data);
  auto remote_mr = rdmapp::remote_mr::deserialize(remote_mr_serialized);
  spdlog::info("received mr addr={} length={} rkey={} from server",
               remote_mr.addr(), remote_mr.length(), remote_mr.rkey());
  for (size_t i = 0; i < kSendCount; ++i) {
    co_await qp->write(remote_mr, local_mr);
    gSendCounts[id].fetch_add(1);
  }
  co_await qp->write_with_imm(remote_mr, local_mr, 0xDEADBEEF);
  co_return;
}

asio::awaitable<void> handler(std::shared_ptr<rdmapp::qp> qp) {
  std::vector<std::byte> buffer;
  buffer.resize(kBufferSizeBytes);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size()));
  auto local_mr_serialized = local_mr->serialize();
  auto local_mr_serialized_data = std::as_bytes(std::span(local_mr_serialized));
  co_await qp->send(local_mr_serialized_data);
  spdlog::info("sent mr addr={} length={} rkey={} to client", local_mr->addr(),
               local_mr->length(), local_mr->rkey());
  auto imm = (co_await qp->recv()).second;
  if (!imm.has_value()) {
    throw std::runtime_error("No imm received");
  }
  if (imm.value() != 0xDEADBEEF) {
    throw std::runtime_error("Wrong imm received");
  }
  co_return;
}

asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor) {
  auto executor = co_await asio::this_coro::executor;
  for (size_t i = 0; i < kQP; i++) {
    auto qp = co_await acceptor->accept();
    asio::co_spawn(executor, handler(std::move(qp)), asio::detached);
  }
  co_return;
}

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto executor = co_await asio::this_coro::executor;
  for (size_t i = 0; i < kQP; i++) {
    auto qp = co_await connector->connect();
    asio::co_spawn(executor, client_worker(std::move(qp), i), asio::detached);
  }
  co_return;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    spdlog::info(
        "Usage: {} [port] for server and {} [server_ip] [port] for client",
        argv[0], argv[0]);
    return -1;
  }

  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(4);
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  auto reporter_fn = [&](std::stop_token token) {
    while (!token.stop_requested()) {
      std::this_thread::sleep_for(1s);
      size_t iops = 0;
      for (size_t i = 0; i < kQP; i++) {
        auto t = gSendCounts[i].exchange(0);
        spdlog::info("IOPS({}): {} buffer_size={}B BW={}Gbps", i, t,
                     kBufferSizeBytes,
                     1.0f * kBufferSizeBytes * t * 8 / 1000 / 1000 / 1000);
        iops += t;
      }
      spdlog::info("IOPS: {} buffer_size={}B BW={}Gbps", iops, kBufferSizeBytes,
                   1.0f * kBufferSizeBytes * iops * 8 / 1000 / 1000 / 1000);
    }
  };

  asio::thread_pool pool(4);
  asio::signal_set signals(*io_ctx, SIGINT, SIGTERM);
  signals.async_wait([io_ctx, &pool](auto, auto) {
    io_ctx->stop();
    pool.stop();
    spdlog::info("gracefully shutdown");
  });

  if (argc == 2) {
    auto work_guard = asio::make_work_guard(*io_ctx);
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = std::make_shared<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
    asio::co_spawn(*io_ctx, server(acceptor), asio::detached);
    std::vector<std::jthread> workers_;
    for (size_t i = 0; i < kQP; i++) {
      workers_.emplace_back([io_ctx, i]() {
        io_ctx->run();
        spdlog::info("worker[{}] exited", i);
      });
    }
    return 0;
  }

  if (argc == 3) {
    std::jthread _([io_ctx]() { io_ctx->run(); }); // handle SIGINT/SIGTERM
    auto connector = std::make_shared<rdmapp::qp_connector>(
        argv[1], std::stoi(argv[2]), pd, cq);
    // NOTE: thread pool perform actually better
    asio::co_spawn(pool, client(connector), asio::detached);
    auto reporter = std::jthread(reporter_fn);
    pool.join();
    spdlog::info("client exit after communicated with {}:{}", argv[1], argv[2]);
  }
  return 0;
}
