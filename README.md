# RDMA++

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)

This library encapsulates the details of building IB queue pairs and provides a user-friendly modern C++ interface, featuring coroutines, and integration with asio.

Requires C++ 20 (i.e. gcc 10 or later).

## Quick Example

Initialize the device, create a protection domain and create a completion queue with a corresponding poller, plus an asio::io_context for QP exchanges with asio::sockets:

```cpp
#include <rdmapp/rdmapp.h>
#include <asio/asio.hpp>

int main(int argc, char *argv[]) {
  spdlog::set_level(spdlog::level::debug);
  auto device = std::make_shared<rdmapp::device>(0, 1);
  auto pd = std::make_shared<rdmapp::pd>(device);
  auto cq = std::make_shared<rdmapp::cq>(device);
  auto io_ctx = std::make_shared<asio::io_context>(4);
  auto cq_poller = std::make_unique<rdmapp::cq_poller>(cq);

  auto work_guard = asio::make_work_guard(*io_ctx);
  auto port = static_cast<uint16_t>(std::stoi(argv[1]));
  auto acceptor = std::make_shared<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
  asio::co_spawn(*io_ctx, server(acceptor), asio::detached);
  io_ctx->run();
}
```

On the server side, create an acceptor to accept QPs:

```cpp
asio::awaitable<void> handle_qp(std::shared_ptr<rdmapp::qp> qp) {
  char buffer[6] = "hello";

  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  std::cout << "Sent to client: " << buffer << std::endl;

  auto recv_buffer = std::as_writable_bytes(std::span(buffer));
  co_await qp->recv(recv_buffer);
  std::cout << "Received from client: " << buffer << std::endl;
  co_return;
}

asio::awaitable<void> server(std::shared_ptr<rdmapp::qp_acceptor> acceptor) {
  while (true) {
    auto qp = co_await acceptor->accept();
    asio::co_spawn(co_await asio::this_coro::executor, handle_qp(qp),
                   asio::detached);
  }
}

int main() {
  // ...
  auto work_guard = asio::make_work_guard(*io_ctx);
  // ...
  auto acceptor = std::make_shared<rdmapp::qp_acceptor>(io_ctx, port, pd, cq);
  asio::co_spawn(*io_ctx, server(acceptor), asio::detached);
  io_ctx->run();

}
```

On the client side, connect to server:

```cpp
asio::awaitable<void> client(std::shared_ptr<rdmapp::qp_connector> connector) {
  auto qp = co_await connector->connect();
  char buffer[6];

  auto recv_buffer = std::as_bytes(std::span(buffer));
  co_await qp->recv(recv_buffer);
  std::cout << "Received from server: " << buffer << std::endl;
  std::copy_n("world", sizeof(buffer), buffer);

  auto send_buffer = std::as_bytes(std::span(buffer));
  co_await qp->send(send_buffer);
  std::cout << "Sent to server: " << buffer << std::endl;
  co_return;
}


int main(int argc, char *argv[]) {
  using rdmapp::qp_connector;
  // ...
  auto connector = std::make_shared<qp_connector>("127.0.0.1", 2333, pd, cq);
  asio::co_spawn(*io_ctx, client(connector), asio::detached);
  io_ctx->run();
}
```

Browse [`examples`](/examples) to learn more about this library.

## Building

Requires: C++ compiler with C++20 standard support and `libibverbs` development headers, asio headers installed.

```bash
git clone https://github.com/SJTU-DDST/rdmapp && cd rdmapp
cmake -Bbuild -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR .
# for shared library build, use -DRDMAPP_BUILD_PIC=ON
# for pybind11, see examples, use -DRDMAPP_BUILD_EXAMPLES_PYBIND=ON
# for no RTTI build, use -RDMAPP_BUILD_NORTTI=ON
cmake --build build
```

## Using with cmake

```cmake
include(FetchContent)

set(RDMAPP_BUILD_PIC ON) # for shared library like pybind
set(RDMAPP_BUILD_NORTTI OFF) # pybind11 needs RTTI
set(RDMAPP_BUILD_EXAMPLES OFF) # for not building examples

fetchcontent_declare(
  rdmapp
  GIT_REPOSITORY https://github.com/SJTU-DDST/rdmapp.git
  GIT_TAG asio-coro
  GIT_SHALLOW 1
)

fetchcontent_makeavailable(rdmapp)

target_link_libraries(your_target PUBLIC rdmapp)
```

## Developing

Install `clang-format` and `pre-commit`.

```bash
pip install pre-commit
pre-commit install
```

## Honor
[rdmapp](https://github.com/howardlau1999/rdmapp)
