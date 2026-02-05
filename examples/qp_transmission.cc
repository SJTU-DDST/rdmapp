#include <spdlog/spdlog.h>

#include "qp_transmission.h"

#include <array>
#include <cassert>
#include <cppcoro/net/socket.hpp>
#include <cppcoro/task.hpp>
#include <span>

namespace rdmapp {

namespace {

auto write_exactly(cppcoro::net::socket &socket,
                   std::span<const std::byte> buffer) -> cppcoro::task<void> {
  std::size_t bytes_written = 0;
  while (bytes_written < buffer.size()) {
    bytes_written += co_await socket.send(buffer.data() + bytes_written,
                                          buffer.size() - bytes_written);
  }
}

auto read_exactly(cppcoro::net::socket &socket, std::span<std::byte> buffer)
    -> cppcoro::task<void> {
  std::size_t bytes_read = 0;
  while (bytes_read < buffer.size()) {
    auto n = co_await socket.recv(buffer.data() + bytes_read,
                                  buffer.size() - bytes_read);
    if (n == 0) {
      throw std::runtime_error("socket closed");
    }
    bytes_read += n;
  }
}

} // namespace

auto send_qp(rdmapp::qp const &qp, cppcoro::net::socket &socket)
    -> cppcoro::task<void> {
  auto local_qp_data = qp.serialize();
  assert(!local_qp_data.empty());
  co_await write_exactly(socket, std::as_bytes(std::span{local_qp_data}));
  spdlog::debug("send qp: bytes={}", local_qp_data.size());
}

auto recv_qp(cppcoro::net::socket &socket)
    -> cppcoro::task<rdmapp::deserialized_qp> {
  std::array<std::byte, rdmapp::deserialized_qp::qp_header::kSerializedSize>
      header_buffer;
  co_await read_exactly(socket, header_buffer);

  auto remote_qp = rdmapp::deserialized_qp::deserialize(header_buffer.data());
  auto const remote_gid_str =
      rdmapp::device::gid_hex_string(remote_qp.header.gid);
  spdlog::debug("received header gid={} lid={} qpn={} psn={} user_data_size={}",
                remote_gid_str.c_str(), remote_qp.header.lid,
                remote_qp.header.qp_num, remote_qp.header.sq_psn,
                remote_qp.header.user_data_size);

  remote_qp.user_data.resize(remote_qp.header.user_data_size);
  if (remote_qp.header.user_data_size > 0) {
    co_await read_exactly(
        socket, std::as_writable_bytes(std::span{remote_qp.user_data}));
  }
  spdlog::debug("received user data: bytes={}",
                remote_qp.header.user_data_size);
  co_return remote_qp;
}

} // namespace rdmapp
