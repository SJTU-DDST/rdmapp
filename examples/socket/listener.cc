#include "listener.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/use_awaitable.hpp>
#include <memory>

#include "rdmapp/detail/debug.h"

namespace rdmapp {

listener::listener(std::shared_ptr<asio::io_context> io_ctx, uint16_t port)
    : io_ctx_(io_ctx), port_(port) {}

asio::awaitable<void> listener::listener_fn(handler f) {
  auto executor = co_await asio::this_coro::executor;
  auto endpoint = asio::ip::tcp::endpoint{asio::ip::tcp::v4(), port_};
  asio::ip::tcp::acceptor acceptor(executor, endpoint);

  RDMAPP_LOG_DEBUG("handling connection with given handler");
  while (true) {
    asio::ip::tcp::socket socket =
        co_await acceptor.async_accept(asio::use_awaitable);
    asio::co_spawn(executor, f(std::move(socket)), asio::detached);
  }
}

void listener::bind_listener(handler f) {
  auto fn = [this, f]() { return listener_fn(f); };
  auto io_ctx = io_ctx_.lock();
  asio::co_spawn(*io_ctx, fn, asio::detached);
}

} // namespace rdmapp
