#include "listener.h"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <algorithm>
#include <asio/awaitable.hpp>
#include <asio/detail/socket_ops.hpp>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>

#include "rdmapp/executor.h"
#include <rdmapp/rdmapp.h>

#include "rdmapp/detail/debug.h"

using namespace std::literals::chrono_literals;

constexpr std::string_view msg = "hello";
constexpr std::string_view resp = "world";

asio::awaitable<void> handle_qp(std::shared_ptr<rdmapp::qp> qp) {
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

void serve(std::shared_ptr<rdmapp::listener> l,
           std::shared_ptr<rdmapp::qp_acceptor> acc) {
  auto f = [acc](asio::ip::tcp::socket socket) -> asio::awaitable<void> {
    auto qp = co_await acc->accept(std::move(socket));
    co_await handle_qp(qp);
  };
  l->bind_listener(std::move(f));
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

  n = co_await qp->read(remote_mr, buffer, sizeof(buffer));
  std::cout << "Read " << n << " bytes from server: " << buffer << std::endl;
  std::copy_n("world", sizeof(buffer), buffer);
  co_await qp->write_with_imm(remote_mr, buffer, sizeof(buffer), 1);

  /* Atomic Fetch-and-Add (FA)/Compare-and-Swap (CS) */
  char counter_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  co_await qp->recv(counter_mr_serialized, sizeof(counter_mr_serialized));
  auto counter_mr = rdmapp::remote_mr::deserialize(counter_mr_serialized);
  std::cout << "Received mr addr=" << counter_mr.addr()
            << " length=" << counter_mr.length()
            << " rkey=" << counter_mr.rkey() << " from server" << std::endl;
  uint64_t counter = 0;
  co_await qp->fetch_and_add(counter_mr, &counter, sizeof(counter), 1);
  std::cout << "Fetched and added from server: " << counter << std::endl;
  co_await qp->write_with_imm(remote_mr, buffer, sizeof(buffer), 1);
  co_await qp->compare_and_swap(counter_mr, &counter, sizeof(counter), 43,
                                4422);
  std::cout << "Compared and swapped from server: " << counter << std::endl;
  co_await qp->write_with_imm(remote_mr, buffer, sizeof(buffer), 1);

  co_return;
}

int main(int argc, char *argv[]) {
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(4);
  auto work_guard = asio::make_work_guard(*io_ctx);
  auto executor = std::make_shared<rdmapp::executor>(io_ctx);
  auto cq_poller = std::make_shared<rdmapp::cq_poller>(cq, executor);

  if (argc == 2) {
    auto l = std::make_shared<rdmapp::listener>(io_ctx);
    auto acc = std::make_shared<rdmapp::qp_acceptor>(pd, cq);
    serve(l, acc);
  } else if (argc == 3) {
    // rdmapp::connector connector(loop, argv[1], std::stoi(argv[2]), pd, cq);
    // client(connector);
  } else {
    std::cout << "Usage: " << argv[0] << " [port] for server and " << argv[0]
              << " [server_ip] [port] for client" << std::endl;
  }
  io_ctx->run();
  return 0;
}
