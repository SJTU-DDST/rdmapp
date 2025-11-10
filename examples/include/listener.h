#pragma once

#include <asio/awaitable.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/socket.h>

#include "rdmapp/detail/noncopyable.h"

namespace rdmapp {
class listener : public noncopyable {

public:
  using handler = std::function<asio::awaitable<void>(asio::ip::tcp::socket)>;

  listener(uint16_t port = 9988);

  void listen_and_serve(asio::io_context &io_ctx, handler f);

private:
  asio::awaitable<void> listener_fn(handler f);
  uint16_t port_;
};

} // namespace rdmapp
