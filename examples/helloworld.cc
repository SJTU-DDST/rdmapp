#include "listener.h"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/detail/socket_ops.hpp>
#include <asio/this_coro.hpp>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <spdlog/spdlog.h>
#include <string>

#include "rdmapp/executor.h"
#include <rdmapp/rdmapp.h>

using namespace std::literals::chrono_literals;

constexpr std::string_view msg = "hello";
constexpr std::string_view resp = "world";

asio::awaitable<void> handle_qp(std::shared_ptr<rdmapp::qp> qp) {
  spdlog::info("handling qp");
  /* Send/Recv */
  std::string buffer = std::string(msg);
  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  std::cout << "Sent to client: " << buffer << std::endl;

  buffer.resize(resp.size());
  std::span<std::byte> recv_buffer = std::as_writable_bytes(std::span(buffer));
  co_await qp->recv(recv_buffer);
  std::cout << "Received from client: " << buffer << std::endl;

  /* Read/Write */
  buffer = std::string(msg);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size()));
  auto local_mr_serialized = local_mr->serialize();
  auto local_mr_serialized_data = std::as_bytes(std::span(local_mr_serialized));
  co_await qp->send(local_mr_serialized_data);
  std::cout << "Sent mr addr=" << local_mr->addr()
            << " length=" << local_mr->length() << " rkey=" << local_mr->rkey()
            << " to client" << std::endl;
  auto [_, imm] = co_await qp->recv(local_mr);
  assert(imm.has_value());
  std::cout << "Written by client (imm=" << imm.value() << "): " << buffer
            << std::endl;

  /* Atomic */
  uint64_t counter = 42;
  auto counter_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(&counter, sizeof(counter)));
  auto counter_mr_serialized = counter_mr->serialize();
  auto counter_mr_serialized_data =
      std::as_bytes(std::span(counter_mr_serialized));
  co_await qp->send(counter_mr_serialized_data);
  std::cout << "Sent mr addr=" << counter_mr->addr()
            << " length=" << counter_mr->length()
            << " rkey=" << counter_mr->rkey() << " to client" << std::endl;
  imm = (co_await qp->recv(local_mr)).second;
  assert(imm.has_value());
  std::cout << "Fetched and added by client: " << counter << std::endl;
  imm = (co_await qp->recv(local_mr)).second;
  assert(imm.has_value());
  std::cout << "Compared and swapped by client: " << counter << std::endl;

  co_return;
}

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto qp = co_await connector->connect();
  std::string buffer;
  buffer.resize(msg.size());
  auto recv_buffer = std::as_writable_bytes(std::span(buffer));

  /* Send/Recv */
  auto [n, _] = co_await qp->recv(recv_buffer);
  std::cout << "Received " << n << " bytes from server: " << buffer
            << std::endl;

  buffer = std::string(resp);
  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  std::cout << "Sent to server: " << buffer << std::endl;

  /* Read/Write */
  char remote_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  auto remote_mr_serialized_data =
      std::as_writable_bytes(std::span(remote_mr_serialized));
  co_await qp->recv(remote_mr_serialized_data);
  auto remote_mr = rdmapp::remote_mr::deserialize(remote_mr_serialized);
  std::cout << "Received mr addr=" << remote_mr.addr()
            << " length=" << remote_mr.length() << " rkey=" << remote_mr.rkey()
            << " from server" << std::endl;

  buffer.resize(msg.size());
  recv_buffer = std::as_writable_bytes(std::span(buffer));

  n = co_await qp->read(remote_mr, recv_buffer);
  std::cout << "Read " << n << " bytes from server: " << buffer << std::endl;
  buffer = std::string(resp);
  send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->write_with_imm(remote_mr, send_buffer, 1);

  /* Atomic Fetch-and-Add (FA)/Compare-and-Swap (CS) */
  char counter_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  recv_buffer = std::as_writable_bytes(std::span(counter_mr_serialized));
  co_await qp->recv(recv_buffer);
  auto counter_mr = rdmapp::remote_mr::deserialize(counter_mr_serialized);
  std::cout << "Received mr addr=" << counter_mr.addr()
            << " length=" << counter_mr.length()
            << " rkey=" << counter_mr.rkey() << " from server" << std::endl;
  uint64_t counter = 0;
  auto cnt_buffer =
      std::as_writable_bytes(std::span(&counter, sizeof(counter)));
  co_await qp->fetch_and_add(counter_mr, cnt_buffer, 1);
  std::cout << "Fetched and added from server: " << counter << std::endl;
  co_await qp->write_with_imm(remote_mr, cnt_buffer, 1);
  co_await qp->compare_and_swap(counter_mr, cnt_buffer, 43, 4422);
  std::cout << "Compared and swapped from server: " << counter << std::endl;
  co_await qp->write_with_imm(remote_mr, cnt_buffer, 1);

  co_return;
}

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::debug);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(4);
  auto executor = std::make_shared<rdmapp::executor>(io_ctx);
  auto cq_poller = std::make_shared<rdmapp::cq_poller>(cq, executor);

  switch (argc) {
  case 2: {
    auto work_guard = asio::make_work_guard(*io_ctx);
    auto l = std::make_shared<rdmapp::listener>(std::stoi(argv[1]));
    auto acc = std::make_shared<rdmapp::qp_acceptor>(pd, cq);
    auto f = [acc](asio::ip::tcp::socket socket) -> asio::awaitable<void> {
      auto qp = co_await acc->accept(std::move(socket));
      co_await handle_qp(qp);
    };
    l->listen_and_serve(*io_ctx, std::move(f));
    io_ctx->run();

    break;
  }

  case 3: {
    auto connector = std::make_shared<rdmapp::qp_connector>(
        argv[1], std::stoi(argv[2]), pd, cq);
    asio::co_spawn(*io_ctx, client(connector), asio::detached);
    io_ctx->run();
    break;
  }

  default: {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
    exit(-1);
  }
  }

  return 0;
}
