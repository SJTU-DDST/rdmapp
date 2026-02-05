#include <spdlog/spdlog.h>

#include "qp_connector.h"

#include "qp_transmission.h"
#include "rdmapp/scheduler.h"
#include <cppcoro/net/ipv4_address.hpp>
#include <cppcoro/net/ipv4_endpoint.hpp>
#include <cppcoro/net/socket.hpp>
#include <stdexcept>
#include <utility>

namespace rdmapp {

template <typename cq_poller_t>
basic_qp_connector<cq_poller_t>::basic_qp_connector(
    cppcoro::io_service &io_service,
    std::shared_ptr<rdmapp::scheduler> scheduler, std::shared_ptr<pd> pd,
    std::shared_ptr<srq> srq, ConnConfig config)
    : pd_(pd), srq_(srq), io_service_(io_service), config_(std::move(config)),
      scheduler_(scheduler) {}

template <typename cq_poller_t>
std::shared_ptr<rdmapp::cq> basic_qp_connector<cq_poller_t>::alloc_cq() {
  auto cq =
      std::make_shared<rdmapp::cq>(this->pd_->device_ptr(), config_.cq_size);
  pollers_.emplace_back(cq, scheduler_);
  return cq;
}

template <typename cq_poller_t>
auto basic_qp_connector<cq_poller_t>::from_socket(
    cppcoro::net::socket &socket, std::span<const std::byte> userdata)
    -> cppcoro::task<std::shared_ptr<rdmapp::qp>> {
  auto cq1 = alloc_cq();
  auto cq2 = alloc_cq();
  auto qp_ptr = std::make_shared<rdmapp::qp>(this->pd_, cq1, cq2, srq_,
                                             config_.queue_pair_config);
  qp_ptr->user_data().assign(userdata.begin(), userdata.end());
  co_await send_qp(*qp_ptr, socket);

  auto remote_qp = co_await recv_qp(socket);

  qp_ptr->rtr(remote_qp.header.lid, remote_qp.header.qp_num,
              remote_qp.header.sq_psn, remote_qp.header.gid);
  spdlog::trace("qp: rtr");
  qp_ptr->user_data() = std::move(remote_qp.user_data);
  spdlog::trace("qp: user_data: size={}", qp_ptr->user_data().size());
  qp_ptr->rts();
  spdlog::trace("qp: rts");
  co_return qp_ptr;
}

template <typename cq_poller_t>
auto basic_qp_connector<cq_poller_t>::connect(
    std::string_view hostname, uint16_t port,
    std::span<const std::byte> userdata)
    -> cppcoro::task<std::shared_ptr<rdmapp::qp>> {
  auto addr = cppcoro::net::ipv4_address::from_string(hostname);
  if (!addr) {
    throw std::runtime_error("failed to parse hostname as ipv4 address");
  }

  cppcoro::net::socket socket = cppcoro::net::socket::create_tcpv4(io_service_);
  co_await socket.connect(cppcoro::net::ipv4_endpoint(*addr, port));

  spdlog::info("connector: tcp connected to: {}:{}", hostname, port);
  auto qp = co_await from_socket(socket, userdata);
  spdlog::info("connector: created qp from tcp connection");
  co_return qp;
}

template class basic_qp_connector<rdmapp::native_cq_poller>;

} // namespace rdmapp
