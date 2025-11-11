#pragma once

#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <rdmapp/qp.h>

namespace rdmapp {

asio::awaitable<void> send_qp(rdmapp::qp const &qp,
                              asio::ip::tcp::socket &socket);

asio::awaitable<rdmapp::deserialized_qp> recv_qp(asio::ip::tcp::socket &socket);

} // namespace rdmapp
