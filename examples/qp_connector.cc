#include "qp_connector.h"

#include "qp_transmission.h"
#include <asio/connect.hpp>
#include <asio/use_awaitable.hpp>
#include <exception>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#include <string>

namespace rdmapp {

qp_connector::qp_connector(std::shared_ptr<asio::io_context> io_ctx,
                           std::string const &hostname, uint16_t port,
                           std::shared_ptr<pd> pd, std::shared_ptr<cq> recv_cq,
                           std::shared_ptr<cq> send_cq,
                           std::shared_ptr<srq> srq)
    : io_ctx_(io_ctx), pd_(pd), recv_cq_(recv_cq), send_cq_(send_cq), srq_(srq),
      hostname_(hostname), port_(port) {}

qp_connector::qp_connector(std::shared_ptr<asio::io_context> io_ctx,
                           std::string const &hostname, uint16_t port,
                           std::shared_ptr<pd> pd, std::shared_ptr<cq> cq,
                           std::shared_ptr<srq> srq)
    : qp_connector(io_ctx, hostname, port, pd, cq, cq, srq) {}

asio::awaitable<std::shared_ptr<rdmapp::qp>>
qp_connector::from_socket(asio::ip::tcp::socket socket) {
  auto qp_ptr =
      std::make_shared<rdmapp::qp>(this->pd_, this->recv_cq_, this->send_cq_);
  co_await send_qp(*qp_ptr, socket);

  auto remote_qp = co_await recv_qp(socket);

  qp_ptr->rtr(remote_qp.header.lid, remote_qp.header.qp_num,
              remote_qp.header.sq_psn, remote_qp.header.gid);
  spdlog::debug("qp: rtr");
  qp_ptr->user_data() = std::move(remote_qp.user_data);
  spdlog::debug("qp: user_data: size={}", qp_ptr->user_data().size());
  qp_ptr->rts();
  spdlog::debug("qp: rts");
  co_return qp_ptr;
}

asio::awaitable<std::shared_ptr<qp>> qp_connector::connect() {
  auto executor = co_await asio::this_coro::executor;
  asio::ip::tcp::resolver resolver(executor);
  auto endpoints = co_await resolver.async_resolve(
      hostname_, std::to_string(port_), asio::use_awaitable);

  if (endpoints.empty()) {
    spdlog::error("connector: failed to resolve: hostname={}", hostname_);
    std::terminate();
  }

  asio::ip::tcp::socket socket(executor);
  co_await asio::async_connect(socket, endpoints, asio::use_awaitable);
  co_await socket.async_connect(*endpoints.begin());

  spdlog::info("connector: tcp connected to: {}:{}", hostname_, port_);
  auto qp = co_await from_socket(std::move(socket));
  spdlog::info("connector: created qp from tcp connection");
  co_return qp;
}

} // namespace rdmapp
