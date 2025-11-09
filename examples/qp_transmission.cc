#include "qp_transmission.h"

#include <array>
#include <asio/awaitable.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <cstdint>
#include <spdlog/spdlog.h>

#include <rdmapp/detail/debug.h>
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

task<void> send_qp(qp const &qp, socket::tcp_connection &connection) {
  auto local_qp_data = qp.serialize();
  assert(local_qp_data.size() != 0);
  size_t local_qp_sent = 0;
  while (local_qp_sent < local_qp_data.size()) {
    int n = co_await connection.send(&local_qp_data[local_qp_sent],
                                     local_qp_data.size() - local_qp_sent);
    if (n == 0) {
      throw_with("remote closed unexpectedly while sending qp");
    }
    check_errno(n, "failed to send qp");
    local_qp_sent += n;
  }
  co_return;
}

task<deserialized_qp> recv_qp(socket::tcp_connection &connection) {
  size_t header_read = 0;
  uint8_t header[deserialized_qp::qp_header::kSerializedSize];
  while (header_read < deserialized_qp::qp_header::kSerializedSize) {
    int n = co_await connection.recv(
        &header[header_read],
        deserialized_qp::qp_header::kSerializedSize - header_read);
    if (n == 0) {
      throw_with("remote closed unexpectedly while receiving qp header");
    }
    header_read += n;
  }

  auto remote_qp = deserialized_qp::deserialize(header);
  auto const remote_gid_str = device::gid_hex_string(remote_qp.header.gid);
  RDMAPP_LOG_TRACE(
      "received header gid=%s lid=%u qpn=%u psn=%u user_data_size=%u",
      remote_gid_str.c_str(), remote_qp.header.lid, remote_qp.header.qp_num,
      remote_qp.header.sq_psn, remote_qp.header.user_data_size);
  remote_qp.user_data.resize(remote_qp.header.user_data_size);

  if (remote_qp.header.user_data_size > 0) {
    size_t user_data_read = 0;
    remote_qp.user_data.resize(remote_qp.header.user_data_size, 0);
    while (user_data_read < remote_qp.header.user_data_size) {
      int n = co_await connection.recv(&remote_qp.user_data[user_data_read],
                                       remote_qp.header.user_data_size -
                                           user_data_read);
      if (n == 0) {
        throw_with("remote closed unexpectedly while receiving user data");
      }
      check_errno(n, "failed to receive user data");
      user_data_read += n;
    }
  }
  RDMAPP_LOG_TRACE("received user data");
  co_return remote_qp;
}

} // namespace rdmapp
