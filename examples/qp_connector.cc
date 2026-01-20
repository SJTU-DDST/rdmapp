#include "qp_connector.h"

#include "qp_transmission.h"
#include <asio/connect.hpp>
#include <asio/use_awaitable.hpp>
#include <exception>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>

namespace rdmapp {

template <typename cq_poller_t>
basic_qp_connector<cq_poller_t>::basic_qp_connector(std::string const &hostname,
                                                    uint16_t port,
                                                    std::shared_ptr<pd> pd,
                                                    std::shared_ptr<srq> srq)
    : pd_(pd), srq_(srq), hostname_(hostname), port_(port) {}

template <typename cq_poller_t>
std::shared_ptr<rdmapp::cq> basic_qp_connector<cq_poller_t>::alloc_cq() {
  auto cq = std::make_shared<rdmapp::cq>(pd_->device_ptr(), 512);
  pollers_.emplace_back(cq);
  pollers_.emplace_back(cq);
  return cq;
}

template <typename cq_poller_t>
asio::awaitable<std::shared_ptr<rdmapp::qp>>
basic_qp_connector<cq_poller_t>::from_socket(asio::ip::tcp::socket socket) {
  auto send_cq = alloc_cq();
  auto recv_cq = alloc_cq();
  auto qp_ptr = std::make_shared<rdmapp::qp>(this->pd_, recv_cq, send_cq);
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
asio::awaitable<std::shared_ptr<qp>>
basic_qp_connector<cq_poller_t>::connect() {
  auto executor = co_await asio::this_coro::executor;

  asio::ip::tcp::resolver resolver(executor);
  auto endpoints = co_await resolver.async_resolve(
      hostname_, std::to_string(port_), asio::use_awaitable);
  if (endpoints.empty()) {
    spdlog::error("connector: failed to resolve: hostname={}", hostname_);
    std::terminate();
  }
  spdlog::info("connector: resolved hostname: {}",
               endpoints.begin()->host_name(), port_);

  asio::ip::tcp::socket socket(executor);
  auto endpoint =
      co_await asio::async_connect(socket, endpoints, asio::use_awaitable);
  spdlog::info("connector: tcp connected to: {}:{}",
               endpoint.address().to_string(), port_);
  auto qp = co_await from_socket(std::move(socket));
  spdlog::info("connector: created qp from tcp connection");
  co_return qp;
}

template class basic_qp_connector<cq_poller>;
template class basic_qp_connector<native_cq_poller>;

} // namespace rdmapp
