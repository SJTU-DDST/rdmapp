#pragma once

#include <cppcoro/net/socket.hpp>
#include <cppcoro/task.hpp>
#include <rdmapp/qp.h>

namespace rdmapp {
struct ConnConfig {
  uint32_t cq_size = 256;
  qp_config queue_pair_config = default_qp_config();
};

auto send_qp(rdmapp::qp const &qp, cppcoro::net::socket &socket)
    -> cppcoro::task<void>;

auto recv_qp(cppcoro::net::socket &socket)
    -> cppcoro::task<rdmapp::deserialized_qp>;

} // namespace rdmapp
