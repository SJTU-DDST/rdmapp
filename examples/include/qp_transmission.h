#pragma once

#include "socket/tcp_connection.h"
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include <rdmapp/qp.h>
#include <rdmapp/task.h>

namespace rdmapp {

asio::awaitable<void> send_qp(rdmapp::qp const &qp,
                              asio::ip::tcp::socket &socket);

asio::awaitable<rdmapp::deserialized_qp> recv_qp(asio::ip::tcp::socket &socket);

task<deserialized_qp> recv_qp(socket::tcp_connection &connection);

task<void> send_qp(qp const &qp, socket::tcp_connection &connection);

} // namespace rdmapp
