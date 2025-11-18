#include "helloworld_handler.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/detail/socket_ops.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_future.hpp>
#include <cassert>
#include <cstdint>
#include <memory>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <spdlog/spdlog.h>
#include <string>

#include "rdmapp/mr.h"
#include <rdmapp/executor.h>
#include <rdmapp/rdmapp.h>

namespace py = pybind11;

class RDMAApp {
public:
  RDMAApp() {
    spdlog::set_level(spdlog::level::debug);
    device_ = std::make_shared<rdmapp::device>(0, 1);
    pd_ = std::make_shared<rdmapp::pd>(device_);
    cq_ = std::make_shared<rdmapp::cq>(device_);
    io_ctx_ = std::make_shared<asio::io_context>(1);
    executor_ = std::make_shared<rdmapp::executor>(io_ctx_);
    cq_poller_ = std::make_unique<rdmapp::cq_poller>(cq_, executor_);
  }

  void run_server(uint16_t port) {
    auto work_guard = asio::make_work_guard(*io_ctx_);
    auto acceptor =
        std::make_shared<rdmapp::qp_acceptor>(io_ctx_, port, pd_, cq_);
    asio::co_spawn(*io_ctx_, ::server(acceptor), asio::detached);
    io_ctx_->run();
  }

  void run_client(const std::string &server_ip, uint16_t port) {
    auto connector =
        std::make_shared<rdmapp::qp_connector>(server_ip, port, pd_, cq_);
    auto fut = asio::co_spawn(*io_ctx_, ::client(connector), asio::use_future);
    io_ctx_->run();
    fut.get();
    spdlog::info("client exit after communicated with {}:{}", server_ip, port);
  }

private:
  std::shared_ptr<rdmapp::device> device_;
  std::shared_ptr<rdmapp::pd> pd_;
  std::shared_ptr<rdmapp::cq> cq_;
  std::shared_ptr<asio::io_context> io_ctx_;
  std::shared_ptr<rdmapp::executor> executor_;
  std::unique_ptr<rdmapp::cq_poller> cq_poller_;
};

PYBIND11_MODULE(rdmapp_py, m) {
  m.doc() = "RDMA Python bindings";

  py::class_<RDMAApp>(m, "RDMAApp")
      .def(py::init<>())
      .def("run_server", &RDMAApp::run_server, "Run RDMA server",
           py::arg("port"))
      .def("run_client", &RDMAApp::run_client, "Run RDMA client",
           py::arg("server_ip"), py::arg("port"));
}
