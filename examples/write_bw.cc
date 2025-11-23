#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/thread_pool.hpp>
#include <cassert>
#include <iostream>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>

#include <rdmapp/rdmapp.h>

using namespace std::literals::chrono_literals;

std::atomic<size_t> gSendCount = 0;

constexpr size_t kBufferSizeBytes = 8;
constexpr size_t kSendCount = 1024 * 1024 * 1024;

asio::awaitable<void> client_worker(std::shared_ptr<rdmapp::qp> qp) {
  std::vector<uint8_t> buffer;
  buffer.resize(kBufferSizeBytes);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(&buffer[0], buffer.size()));
  char remote_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  auto remote_mr_serialized_data =
      std::as_writable_bytes(std::span(remote_mr_serialized));
  co_await qp->recv(remote_mr_serialized_data);
  auto remote_mr = rdmapp::remote_mr::deserialize(remote_mr_serialized);
  std::cout << "Received mr addr=" << remote_mr.addr()
            << " length=" << remote_mr.length() << " rkey=" << remote_mr.rkey()
            << " from server" << std::endl;
  for (size_t i = 0; i < kSendCount; ++i) {
    co_await qp->write(remote_mr, local_mr);
    gSendCount.fetch_add(1);
  }
  co_await qp->write_with_imm(remote_mr, local_mr, 0xDEADBEEF);
  co_return;
}

asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor) {
  auto qp = co_await acceptor->accept();
  std::vector<uint8_t> buffer;
  buffer.resize(kBufferSizeBytes);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(&buffer[0], sizeof(buffer)));
  auto local_mr_serialized = local_mr->serialize();
  auto local_mr_serialized_data = std::as_bytes(std::span(local_mr_serialized));
  co_await qp->send(local_mr_serialized_data);
  spdlog::info("sent mr addr={} length={} rkey={} to client", local_mr->addr(),
               local_mr->length(), local_mr->rkey());
  auto imm = (co_await qp->recv(local_mr)).second;
  if (!imm.has_value()) {
    throw std::runtime_error("No imm received");
  }
  if (imm.value() != 0xDEADBEEF) {
    throw std::runtime_error("Wrong imm received");
  }
  co_return;
}

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto qp = co_await connector->connect();
  co_await client_worker(qp);
  co_return;
}

int main(int argc, char *argv[]) {
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(4);
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  auto reporter = std::jthread([&]() {
    while (true) {
      std::this_thread::sleep_for(1s);
      std::cout << "IOPS: " << gSendCount.exchange(0) << std::endl;
    }
  });

  std::jthread _([io_ctx]() {
    auto work_guard = asio::make_work_guard(*io_ctx);
    io_ctx->run();
  });

  asio::thread_pool pool(4);
  if (argc == 2) {
    auto work_guard = asio::make_work_guard(*io_ctx);
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = std::make_shared<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
    asio::co_spawn(*io_ctx, server(acceptor), asio::detached);
    io_ctx->run();
  } else if (argc == 3) {
    auto connector = std::make_shared<rdmapp::qp_connector>(
        argv[1], std::stoi(argv[2]), pd, cq);
    // NOTE: thread pool perform actually better
    asio::co_spawn(pool, client(connector), asio::detached);
    pool.join();
    io_ctx->stop();
    spdlog::info("client exit after communicated with {}:{}", argv[1], argv[2]);
  } else {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
  }

  return 0;
}
