#include "rdmapp/executor.h"

#include <asio/post.hpp>
#include <memory>

namespace rdmapp {
executor::executor(std::shared_ptr<asio::io_context> io_context)
    : io_ctx_(std::move(io_context)) {}

void executor::process_wc(struct ibv_wc const &wc) {
  auto fn = [wc] {
    auto cb = reinterpret_cast<callback_ptr>(wc.wr_id);
    (*cb)(wc);
    destroy_callback(cb);
  };

  asio::post(*io_ctx_, fn);
}

void executor::shutdown() {}

void executor::destroy_callback(callback_ptr cb) { delete cb; }

executor::~executor() { shutdown(); }

} // namespace rdmapp
