#include "qp_transmission.h"

#include <array>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <cstdint>
#include <spdlog/spdlog.h>

#include <rdmapp/qp.h>

namespace rdmapp {

asio::awaitable<void> send_qp(rdmapp::qp const &qp,
                              asio::ip::tcp::socket &socket) {
  auto local_qp_data = qp.serialize();
  assert(local_qp_data.size() != 0);
  auto buffer = asio::buffer(local_qp_data);
  auto nbytes = co_await asio::async_write(socket, buffer);
  spdlog::debug("send qp: bytes={}", nbytes);
}

asio::awaitable<rdmapp::deserialized_qp>
recv_qp(asio::ip::tcp::socket &socket) {
  std::array<uint8_t, rdmapp::deserialized_qp::qp_header::kSerializedSize>
      header;
  auto buffer = asio::buffer(header);

  auto nbytes = co_await asio::async_read(socket, buffer);
  spdlog::debug("read qp header: bytes={} expected={}", nbytes, buffer.size());

  auto remote_qp = rdmapp::deserialized_qp::deserialize(header.data());
  auto const remote_gid_str =
      rdmapp::device::gid_hex_string(remote_qp.header.gid);
  spdlog::debug("received header gid={} lid={} qpn={} psn={} user_data_size={}",
                remote_gid_str.c_str(), remote_qp.header.lid,
                remote_qp.header.qp_num, remote_qp.header.sq_psn,
                remote_qp.header.user_data_size);
  remote_qp.user_data.resize(remote_qp.header.user_data_size);

  if (remote_qp.header.user_data_size > 0) {
    std::vector<uint8_t> user_data(remote_qp.header.user_data_size);
    auto buffer = asio::buffer(user_data);

    auto nbytes = co_await asio::async_read(socket, buffer);
    spdlog::debug("read userdata: bytes={} expected={}", nbytes, buffer.size());
  }
  spdlog::debug("received user data: bytes={}",
                remote_qp.header.user_data_size);
  co_return remote_qp;
}

} // namespace rdmapp
