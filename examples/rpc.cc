#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/detail/socket_ops.hpp>
#include <asio/signal_set.hpp>
#include <asio/this_coro.hpp>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <ranges>
#include <span>
#include <spdlog/spdlog.h>
#include <string>

#include <rdmapp/mr.h>
#include <rdmapp/rdmapp.h>

using namespace std::literals::chrono_literals;

constexpr std::string_view msg = "this is an rdmapp based rpc simple example";

using rpc_function = std::function<void()>;

constexpr int kRpcCnt = 20;

static auto range =
    std::views::iota(0, kRpcCnt) | std::views::transform([](int i) {
      return std::make_pair(i,
                            rpc_function{[i]() { spdlog::info("rpc_{}", i); }});
    });

std::unordered_map<int, rpc_function> registered_rpc_fn(range.begin(),
                                                        range.end());

asio::awaitable<void> handle_qp(std::shared_ptr<rdmapp::qp> qp) {
  auto buffer = std::string(msg);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size()));
  auto local_mr_serialized = local_mr->serialize();
  auto local_mr_serialized_data = std::as_bytes(std::span(local_mr_serialized));
  co_await qp->send(local_mr_serialized_data);
  spdlog::info("sent mr: addr={} length={} rkey={} to client", local_mr->addr(),
               local_mr->length(), local_mr->rkey());

  for (int i : std::views::iota(0, kRpcCnt)) {
    /* recv the imm which needs ibv_post_send without local mr */
    auto [_, imm] = co_await qp->recv();
    assert(imm.has_value());
    spdlog::info("[{}] written by client: imm={}", i, imm.value());
    registered_rpc_fn[imm.value()]();
  }
}

asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor) {
  while (true) {
    auto qp = co_await acceptor->accept();
    asio::co_spawn(co_await asio::this_coro::executor, handle_qp(qp),
                   asio::detached);
  }
}

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto qp = co_await connector->connect();
  char remote_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  auto remote_mr_serialized_data =
      std::as_writable_bytes(std::span(remote_mr_serialized));
  co_await qp->recv(remote_mr_serialized_data);
  auto remote_mr = rdmapp::remote_mr::deserialize(remote_mr_serialized);
  spdlog::info("received mr addr={} length={} rkey={} from server",
               remote_mr.addr(), remote_mr.length(), remote_mr.rkey());
  auto buffer = std::string(msg);
  auto send_buffer = std::as_bytes(std::span(buffer));
  for (int i : std::views::iota(0, kRpcCnt)) {
    co_await qp->write_with_imm(remote_mr, send_buffer, i);
  }
}

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::debug);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(4);
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  switch (argc) {
  case 2: {
    asio::signal_set signals(*io_ctx, SIGINT, SIGTERM);
    signals.async_wait([io_ctx](auto, auto) {
      io_ctx->stop();
      spdlog::info("gracefully shutdown");
    });
    auto work_guard = asio::make_work_guard(*io_ctx);
    uint16_t port = (uint16_t)std::stoi(argv[1]);
    auto acceptor = std::make_shared<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
    asio::co_spawn(*io_ctx, server(acceptor), asio::detached);
    io_ctx->run();
    break;
  }

  case 3: {
    auto connector = std::make_shared<rdmapp::qp_connector>(
        argv[1], std::stoi(argv[2]), pd, cq);
    asio::co_spawn(*io_ctx, client(connector), asio::detached);
    io_ctx->run();
    spdlog::info("client exit after communicated with {}:{}", argv[1], argv[2]);
    break;
  }

  default: {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
    ::exit(-1);
  }
  }

  return 0;
}
