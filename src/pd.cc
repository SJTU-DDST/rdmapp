#include "rdmapp/pd.h"

#include <cstring>
#include <memory>
#include <spdlog/spdlog.h>

#include <infiniband/verbs.h>

#include "rdmapp/device.h"
#include "rdmapp/error.h"

namespace rdmapp {

pd::pd(std::shared_ptr<rdmapp::device> device) : device_(device) {
  pd_ = ::ibv_alloc_pd(device->ctx_);
  check_ptr(pd_, "failed to alloc pd");
  spdlog::trace("alloc pd {}", fmt::ptr(pd_));
}

std::shared_ptr<device> pd::device_ptr() const { return device_; }

local_mr pd::reg_mr(void *buffer, size_t length, int flags) {
  auto mr = ::ibv_reg_mr(pd_, buffer, length, flags);
  check_ptr(mr, "failed to reg mr");
  return rdmapp::local_mr(this->shared_from_this(), mr);
}

pd::~pd() {
  if (pd_ == nullptr) [[unlikely]] {
    return;
  }
  if (auto rc = ::ibv_dealloc_pd(pd_); rc != 0) [[unlikely]] {
    spdlog::error("failed to dealloc pd {}: {}", fmt::ptr(pd_),
                  strerror(errno));
  } else {
    spdlog::trace("dealloc pd {}", fmt::ptr(pd_));
  }
}

} // namespace rdmapp
