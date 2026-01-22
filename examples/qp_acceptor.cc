#include "qp_acceptor.h"

#include "qp_transmission.h"
#include <asio/use_awaitable.hpp>
#include <cstdint>
#include <exception>
#include <memory>
#include <spdlog/spdlog.h>

#include "rdmapp/cq_poller.h"
#include "rdmapp/qp.h"
#include "rdmapp/srq.h"

namespace rdmapp {

template <typename cq_poller_t>
basic_qp_acceptor<cq_poller_t>::basic_qp_acceptor(
    std::shared_ptr<asio::io_context> io_ctx, uint16_t port,
    std::shared_ptr<pd> pd, std::shared_ptr<srq> srq)
    : pd_(pd), srq_(srq), io_ctx_(io_ctx),
      acceptor_(*io_ctx, {asio::ip::tcp::v4(), port}) {}

template <typename cq_poller_t>
std::shared_ptr<rdmapp::cq> basic_qp_acceptor<cq_poller_t>::alloc_cq() {
  auto cq = std::make_shared<rdmapp::cq>(pd_->device_ptr(), 256);
  pollers_.emplace_back(cq);
  return cq;
}

template <typename cq_poller_t>
asio::awaitable<std::shared_ptr<qp>> basic_qp_acceptor<cq_poller_t>::accept() {
  try {
    asio::ip::tcp::socket socket =
        co_await acceptor_.async_accept(asio::use_awaitable);
    auto remote_qp = co_await recv_qp(socket);
    auto send_cq = alloc_cq();
    auto recv_cq = alloc_cq();
    auto local_qp = std::make_shared<rdmapp::qp>(
        remote_qp.header.lid, remote_qp.header.qp_num, remote_qp.header.sq_psn,
        remote_qp.header.gid, pd_, recv_cq, send_cq, srq_,
        qp_config{.max_send_wr = 2048, .max_recv_wr = 2048});
    local_qp->user_data() = std::move(remote_qp.user_data);
    co_await send_qp(*local_qp, socket);
    co_return local_qp;
  } catch (std::exception const &e) {
    spdlog::error("accept qp failed: {}", e.what());
    co_return nullptr;
  }
}

template class basic_qp_acceptor<rdmapp::cq_poller>;
template class basic_qp_acceptor<rdmapp::native_cq_poller>;

} // namespace rdmapp
