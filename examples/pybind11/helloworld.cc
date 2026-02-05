#include <spdlog/spdlog.h>

#include "helloworld_handler.hpp"
#include "qp_acceptor.h"
#include "qp_connector.h"
#include <cppcoro/io_service.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cstdint>
#include <memory>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <rdmapp/log.h>
#include <rdmapp/rdmapp.h>
#include <rdmapp/scheduler.h>
#include <string>
#include <thread>

namespace py = pybind11;

class RDMAApp {
public:
  RDMAApp()
      : io_service_(1),
        scheduler_(std::make_shared<rdmapp::basic_scheduler>()) {
    rdmapp::log::setup(rdmapp::log::level::debug);
    device_ = std::make_shared<rdmapp::device>(0, 1);
    pd_ = std::make_shared<rdmapp::pd>(device_);
    t_io_ = std::jthread([&]() { io_service_.process_events(); });
    t_sched_ = std::jthread([this]() { scheduler_->run(); });
  }

  void run_server(uint16_t port) {
    auto acceptor = rdmapp::qp_acceptor(io_service_, scheduler_, port, pd_);
    cppcoro::sync_wait(::server(acceptor));
  }

  void run_client(const std::string &server_ip, uint16_t port) {
    auto connector = rdmapp::qp_connector(io_service_, scheduler_, pd_);
    cppcoro::sync_wait(::client(connector, server_ip, port));
    spdlog::info("client exit after communicated with {}:{}", server_ip, port);
    io_service_.stop();
    scheduler_->stop();
  }

private:
  cppcoro::io_service io_service_;
  std::shared_ptr<rdmapp::basic_scheduler> scheduler_;
  std::shared_ptr<rdmapp::device> device_;
  std::shared_ptr<rdmapp::pd> pd_;
  std::jthread t_io_;
  std::jthread t_sched_;
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
