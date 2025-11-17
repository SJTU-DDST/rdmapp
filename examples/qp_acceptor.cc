#include "qp_acceptor.h"

#include "qp_transmission.h"
#include <asio/use_awaitable.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <spdlog/spdlog.h>

namespace rdmapp {

qp_acceptor::qp_acceptor(std::shared_ptr<asio::io_context> io_ctx,
                         uint16_t port, std::shared_ptr<pd> pd,
                         std::shared_ptr<cq> cq, std::shared_ptr<srq> srq)
    : qp_acceptor(io_ctx, port, pd, cq, cq, srq) {}

qp_acceptor::qp_acceptor(std::shared_ptr<asio::io_context> io_ctx,
                         uint16_t port, std::shared_ptr<pd> pd,
                         std::shared_ptr<cq> recv_cq,
                         std::shared_ptr<cq> send_cq, std::shared_ptr<srq> srq)
    : pd_(pd), recv_cq_(recv_cq), send_cq_(send_cq), srq_(srq), io_ctx_(io_ctx),
      acceptor_(*io_ctx, {asio::ip::tcp::v4(), port}) {}

asio::awaitable<std::shared_ptr<qp>> qp_acceptor::accept() {
  try {
    asio::ip::tcp::socket socket =
        co_await acceptor_.async_accept(asio::use_awaitable);
    auto remote_qp = co_await recv_qp(socket);
    auto local_qp = std::make_shared<rdmapp::qp>(
        remote_qp.header.lid, remote_qp.header.qp_num, remote_qp.header.sq_psn,
        remote_qp.header.gid, pd_, recv_cq_, send_cq_);
    local_qp->user_data() = std::move(remote_qp.user_data);
    co_await send_qp(*local_qp, socket);
    co_return local_qp;
  } catch (std::exception const &e) {
    spdlog::error("accept qp failed: {}", e.what());
    co_return nullptr;
  }
}

} // namespace rdmapp
