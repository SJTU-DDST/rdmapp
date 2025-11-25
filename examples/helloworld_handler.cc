#include "helloworld_handler.hpp"

#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <spdlog/spdlog.h>
#include <string_view>

#include <rdmapp/qp.h>

constexpr std::string_view msg = "hello";
constexpr std::string_view resp = "world";

static asio::awaitable<void> handle_qp(std::shared_ptr<rdmapp::qp> qp) {

  spdlog::info("handling qp");
  /* Send/Recv */
  std::string buffer = std::string(msg);
  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  spdlog::info("sent to client: {}", buffer);

  buffer.resize(resp.size());
  std::span<std::byte> recv_buffer = std::as_writable_bytes(std::span(buffer));
  co_await qp->recv(recv_buffer);
  spdlog::info("received from client: {}", buffer);

  /* Read/Write */
  buffer = std::string(msg);
  auto local_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(buffer.data(), buffer.size()));
  auto local_mr_serialized = local_mr->serialize();
  auto local_mr_serialized_data = std::as_bytes(std::span(local_mr_serialized));
  co_await qp->send(local_mr_serialized_data);
  spdlog::info("sent mr: addr={} length={} rkey={} to client", local_mr->addr(),
               local_mr->length(), local_mr->rkey());

  /* recv the imm which needs ibv_post_send without local mr */
  auto [_, imm] = co_await qp->recv();
  assert(imm.has_value());
  spdlog::info("written by client: (imm={}): {}", imm.value(), buffer);

  /* Atomic */
  uint64_t counter = 42;
  auto counter_mr = std::make_shared<rdmapp::local_mr>(
      qp->pd_ptr()->reg_mr(&counter, sizeof(counter)));
  auto counter_mr_serialized = counter_mr->serialize();
  auto counter_mr_serialized_data =
      std::as_bytes(std::span(counter_mr_serialized));
  co_await qp->send(counter_mr_serialized_data);

  spdlog::info("sent mr: addr={} length={} rkey={} to client",
               counter_mr->addr(), counter_mr->length(), counter_mr->rkey());
  imm = (co_await qp->recv(local_mr)).second;
  assert(imm.has_value());
  spdlog::info("fetched and added by client: {}", counter);
  imm = (co_await qp->recv(local_mr)).second;
  assert(imm.has_value());
  spdlog::info("compared and swapper by client: {}", counter);

  co_return;
}

asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor) {
  while (true) {
    auto qp = co_await acceptor->accept();
    if (!qp) {
      spdlog::error("server: failed to accept qp, skipped");
      continue;
    }
    asio::co_spawn(co_await asio::this_coro::executor, handle_qp(qp),
                   asio::detached);
  }
}

asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto qp = co_await connector->connect();
  std::string buffer;
  buffer.resize(msg.size());
  auto recv_buffer = std::as_writable_bytes(std::span(buffer));
  /* Send/Recv */
  auto [n, _] = co_await qp->recv(recv_buffer);
  spdlog::info("received {} bytes from server: {}", n, buffer);
  buffer = std::string(resp);
  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  spdlog::info("sent to server: {}", buffer);
  /* Read/Write */
  char remote_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  auto remote_mr_serialized_data =
      std::as_writable_bytes(std::span(remote_mr_serialized));
  co_await qp->recv(remote_mr_serialized_data);
  auto remote_mr = rdmapp::remote_mr::deserialize(remote_mr_serialized);
  spdlog::info("received mr addr={} length={} rkey={} from server",
               remote_mr.addr(), remote_mr.length(), remote_mr.rkey());
  buffer.resize(msg.size());
  recv_buffer = std::as_writable_bytes(std::span(buffer));
  n = co_await qp->read(remote_mr, recv_buffer);
  spdlog::info("read {} bytes from server: {}", n, buffer);
  buffer = std::string(resp);
  send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->write_with_imm(remote_mr, send_buffer, 1);
  /* Atomic Fetch-and-Add (FA)/Compare-and-Swap (CS) */
  char counter_mr_serialized[rdmapp::remote_mr::kSerializedSize];
  recv_buffer = std::as_writable_bytes(std::span(counter_mr_serialized));
  co_await qp->recv(recv_buffer);
  auto counter_mr = rdmapp::remote_mr::deserialize(counter_mr_serialized);
  spdlog::info("received mr addr={} length={} rkey={} from server",
               counter_mr.addr(), counter_mr.length(), counter_mr.rkey());
  uint64_t counter = 0;
  auto cnt_buffer = std::as_writable_bytes(std::span(&counter, 1));
  co_await qp->fetch_and_add(counter_mr, cnt_buffer, 1);
  spdlog::info("fetched and added from server: {}", counter);
  co_await qp->write_with_imm(counter_mr, cnt_buffer, 1);
  spdlog::info("written with imm: buffer={} imm={}", counter, 1);
  co_await qp->compare_and_swap(counter_mr, cnt_buffer, 43, 4422);
  spdlog::info("compared and swapped from server: {}", counter);
  co_await qp->write_with_imm(counter_mr, cnt_buffer, 1);
  co_return;
}
