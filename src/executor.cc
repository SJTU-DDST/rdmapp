#include "rdmapp/executor.h"

#include <asio/post.hpp>
#include <memory>
#include <spdlog/spdlog.h>

namespace rdmapp {
executor::executor(std::shared_ptr<asio::io_context> io_context)
    : io_ctx_(std::move(io_context)) {}

void executor::process_wc(struct ibv_wc const &wc) {
  auto fn = [wc] {
    spdlog::trace("process_wc: {:#x}", wc.wr_id);
    auto cb = reinterpret_cast<callback_ptr>(wc.wr_id);
    (*cb)(wc);
    spdlog::trace("process_wc done: {:#x}", wc.wr_id);
    destroy_callback(cb);
    spdlog::trace("process_wc callback destroyed");
  };

  asio::post(*io_ctx_, fn);
  spdlog::trace("submit process_wc task: {:#x}", wc.wr_id);
}

void executor::shutdown() {}

void executor::destroy_callback(callback_ptr cb) { delete cb; }

executor::~executor() { shutdown(); }

} // namespace rdmapp
