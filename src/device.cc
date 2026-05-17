#include "rdmapp/device.h"

#include "rdmapp/detail/logger.h"
#include "rdmapp/error.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <infiniband/verbs.h>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace rdmapp {

namespace {

constexpr uint32_t kUnknownGidType = std::numeric_limits<uint32_t>::max();

struct gid_candidate {
  int index = -1;
  union ibv_gid gid = {};
  uint32_t gid_type = kUnknownGidType;
  uint32_t ifindex = 0;
  int rc = 0;
  int error = 0;
  bool ok = false;
  bool zero = true;
  bool legacy = false;
};

static constexpr bool is_zero_gid(union ibv_gid const &gid) noexcept {
  for (auto byte : gid.raw) {
    if (byte != 0) {
      return false;
    }
  }
  return true;
}

static constexpr bool is_preferred_gid_type(uint8_t link_layer,
                                            uint32_t gid_type,
                                            int priority) noexcept {
  /*
  RoCE / Ethernet
  priority 0: only ROCE_V2
  priority 1: only ROCE_V1
  priority 2: any GID type

  InfiniBand
  priority 0: only IB
  priority 1: any GID type

  Other unknown link layer
  priority 0: any GID type
  */
  if (link_layer == IBV_LINK_LAYER_ETHERNET) {
    return (priority == 0 && gid_type == IBV_GID_TYPE_ROCE_V2) ||
           (priority == 1 && gid_type == IBV_GID_TYPE_ROCE_V1) || priority == 2;
  }
  if (link_layer == IBV_LINK_LAYER_INFINIBAND) {
    return (priority == 0 && gid_type == IBV_GID_TYPE_IB) || priority == 1;
  }
  return priority == 0;
}

std::string device_name(struct ibv_device *device) {
  if (auto name = ::ibv_get_device_name(device); name != nullptr) {
    return name;
  }
  return "unknown";
}

} // namespace

device_list::device_list() : devices_(nullptr), nr_devices_(0) {
  int32_t nr_devices = -1;
  devices_ = ::ibv_get_device_list(&nr_devices);
  if (nr_devices == 0) {
    ::ibv_free_device_list(devices_);
    throw std::runtime_error("no Infiniband devices found");
  }
  check_ptr(devices_, "failed to get Infiniband devices");
  nr_devices_ = nr_devices;
}

device_list::~device_list() {
  if (devices_ != nullptr) {
    ::ibv_free_device_list(devices_);
    log::debug("closed devices list");
  }
}

device_list::iterator::iterator(struct ibv_device **devices, size_t i)
    : i_(i), devices_(devices) {}

struct ibv_device *&device_list::iterator::operator*() { return devices_[i_]; }

bool device_list::iterator::operator==(
    device_list::iterator const &other) const {
  return i_ == other.i_;
}

bool device_list::iterator::operator!=(
    device_list::iterator const &other) const {
  return i_ != other.i_;
}

device_list::iterator &device_list::iterator::operator++() {
  i_++;
  return *this;
}

device_list::iterator &device_list::iterator::operator++(int) {
  i_++;
  return *this;
}

device_list::iterator device_list::begin() { return iterator(devices_, 0); }

device_list::iterator device_list::end() {
  return iterator(devices_, nr_devices_);
}

size_t device_list::size() { return nr_devices_; }

struct ibv_device *device_list::at(size_t i) {
  if (i >= nr_devices_) {
    throw std::out_of_range("out of range");
  }
  return devices_[i];
}

uint32_t device::mtu_bytes(enum ibv_mtu mtu) {
  switch (mtu) {
  case IBV_MTU_256:
    return 256;
  case IBV_MTU_512:
    return 512;
  case IBV_MTU_1024:
    return 1024;
  case IBV_MTU_2048:
    return 2048;
  case IBV_MTU_4096:
    return 4096;
  }
  return 0;
}

std::string device::mtu_string(enum ibv_mtu mtu) {
  auto const bytes = mtu_bytes(mtu);
  if (bytes != 0) {
    return std::to_string(bytes);
  }
  return "unknown(" + std::to_string(static_cast<int>(mtu)) + ")";
}

std::string device::link_layer_string(uint8_t link_layer) {
  switch (link_layer) {
  case IBV_LINK_LAYER_ETHERNET:
    return "ethernet";
  case IBV_LINK_LAYER_INFINIBAND:
    return "infiniband";
  }
  return "unspecified(" + std::to_string(link_layer) + ")";
}

std::string device::gid_type_string(uint32_t gid_type) {
  if (gid_type == kUnknownGidType) {
    return "unknown";
  }
  switch (gid_type) {
  case IBV_GID_TYPE_IB:
    return "ib";
  case IBV_GID_TYPE_ROCE_V1:
    return "roce-v1";
  case IBV_GID_TYPE_ROCE_V2:
    return "roce-v2";
  }
  return "unknown(" + std::to_string(gid_type) + ")";
}

void device::select_gid() {
  std::vector<gid_candidate> candidates;
  candidates.reserve(static_cast<size_t>(std::max(port_attr_.gid_tbl_len, 0)));

  for (int index = 0; index < port_attr_.gid_tbl_len; ++index) {
    gid_candidate candidate;
    candidate.index = index;

    struct ibv_gid_entry entry = {};
    candidate.rc = ::ibv_query_gid_ex(ctx_, port_num_, index, &entry, 0);
    if (candidate.rc == 0) {
      candidate.gid = entry.gid;
      candidate.gid_type = entry.gid_type;
      candidate.ifindex = entry.ndev_ifindex;
      candidate.ok = true;
      candidate.zero = is_zero_gid(candidate.gid);
    } else {
      candidate.error = candidate.rc;
      auto const legacy_rc =
          ::ibv_query_gid(ctx_, port_num_, index, &candidate.gid);
      if (legacy_rc == 0) {
        candidate.rc = 0;
        candidate.gid_type = kUnknownGidType;
        candidate.ok = true;
        candidate.zero = is_zero_gid(candidate.gid);
        candidate.legacy = true;
      } else {
        candidate.rc = legacy_rc;
        candidate.error = legacy_rc;
      }
    }

    if (candidate.ok) {
      log::debug("gid table entry device={} port={} index={} gid={} type={} "
                 "ifindex={} zero={} source={}",
                 device_name(device_), port_num_, index,
                 gid_hex_string(candidate.gid),
                 gid_type_string(candidate.gid_type), candidate.ifindex,
                 candidate.zero ? "true" : "false",
                 candidate.legacy ? "ibv_query_gid" : "ibv_query_gid_ex");
    } else {
      log::warn("failed to query gid table entry device={} port={} index={}: "
                "{} (rc={} errno={})",
                device_name(device_), port_num_, index,
                ::strerror(candidate.error), candidate.rc, candidate.error);
    }
    candidates.push_back(candidate);
  }

  gid_candidate const *selected = nullptr;
  auto select_by_priority = [&](int priority) {
    for (auto const &candidate : candidates) {
      if (!candidate.ok || candidate.zero) {
        continue;
      }
      if (is_preferred_gid_type(port_attr_.link_layer, candidate.gid_type,
                                priority)) {
        selected = &candidate;
        return true;
      }
    }
    return false;
  };

  if (port_attr_.link_layer == IBV_LINK_LAYER_ETHERNET) {
    if (!select_by_priority(0) && !select_by_priority(1)) {
      select_by_priority(2);
    }
  } else if (port_attr_.link_layer == IBV_LINK_LAYER_INFINIBAND) {
    if (!select_by_priority(0)) {
      select_by_priority(1);
    }
  } else {
    select_by_priority(0);
  }

  if (selected != nullptr) {
    gid_index_ = selected->index;
    gid_ = selected->gid;
    gid_type_ = selected->gid_type;
    log::info(
        "selected gid device={} port={} index={} gid={} type={} link_layer={}",
        device_name(device_), port_num_, gid_index_, gid_hex_string(gid_),
        gid_type_string(gid_type_), link_layer_string(port_attr_.link_layer));
    return;
  }

  std::ostringstream summary;
  for (auto const &candidate : candidates) {
    if (candidate.ok) {
      summary << " index=" << candidate.index
              << " gid=" << gid_hex_string(candidate.gid)
              << " type=" << gid_type_string(candidate.gid_type)
              << " zero=" << (candidate.zero ? "true" : "false") << ";";
    } else {
      summary << " index=" << candidate.index
              << " query_failed=" << ::strerror(candidate.error)
              << " rc=" << candidate.rc << " errno=" << candidate.error << ";";
    }
  }

  throw_with("failed to select gid for device=%s port=%u link_layer=%s lid=%u "
             "active_mtu=%s gid_tbl_len=%d entries:%s",
             device_name(device_).c_str(), port_num_,
             link_layer_string(port_attr_.link_layer).c_str(), port_attr_.lid,
             mtu_string(port_attr_.active_mtu).c_str(), port_attr_.gid_tbl_len,
             summary.str().c_str());
}

void device::open_device(struct ibv_device *target, uint16_t port_num) {
  device_ = target;
  port_num_ = port_num;
  ctx_ = ::ibv_open_device(device_);
  check_ptr(ctx_, "failed to open device");

  try {
    check_rc(::ibv_query_port(ctx_, port_num_, &port_attr_),
             "failed to query port");
    struct ibv_query_device_ex_input query = {};
    check_rc(::ibv_query_device_ex(ctx_, &query, &device_attr_ex_),
             "failed to query extended attributes");

    select_gid();
  } catch (...) {
    ::ibv_close_device(ctx_);
    ctx_ = nullptr;
    throw;
  }

  auto const gid_str = gid_hex_string(gid_);
  log::debug("opened Infiniband device name={} port={} gid={} gid_index={} "
             "gid_type={} lid={} link_layer={} active_mtu={} max_mtu={}",
             device_name(device_), port_num_, gid_str, gid_index_,
             gid_type_string(gid_type_), port_attr_.lid,
             link_layer_string(port_attr_.link_layer),
             mtu_string(port_attr_.active_mtu), mtu_string(port_attr_.max_mtu));
}

device::device(struct ibv_device *target, uint16_t port_num) {
  assert(target != nullptr);
  open_device(target, port_num);
}

device::device(std::string const &device_name, uint16_t port_num)
    : device_(nullptr), port_num_(0) {
  auto devices = device_list();
  for (auto target : devices) {
    if (::ibv_get_device_name(target) == device_name) {
      open_device(target, port_num);
      return;
    }
  }
  throw_with("no device named %s found", device_name.c_str());
}

device::device(uint16_t device_num, uint16_t port_num)
    : device_(nullptr), port_num_(0) {
  device_list_ = std::make_unique<device_list>();
  if (device_num >= device_list_->size()) {
    char buffer[kErrorStringBufferSize] = {0};
    ::snprintf(buffer, sizeof(buffer),
               "requested device number %d out of range, %lu devices available",
               device_num, device_list_->size());
    throw std::invalid_argument(buffer);
  }
  open_device(device_list_->at(device_num), port_num);
}

uint16_t device::port_num() const { return port_num_; }

uint16_t device::lid() const { return port_attr_.lid; }

enum ibv_mtu device::active_mtu() const { return port_attr_.active_mtu; }

enum ibv_mtu device::max_mtu() const { return port_attr_.max_mtu; }

uint32_t device::active_mtu_bytes() const { return mtu_bytes(active_mtu()); }

union ibv_gid device::gid() const {
  union ibv_gid gid_copied;
  ::memcpy(&gid_copied, &gid_, sizeof(union ibv_gid));
  return gid_copied;
}

bool device::is_compare_and_swap_supported() const {
  return device_attr_ex_.orig_attr.atomic_cap != IBV_ATOMIC_NONE;
}

bool device::is_fetch_and_add_supported() const {
  return device_attr_ex_.orig_attr.atomic_cap != IBV_ATOMIC_NONE;
}

int device::gid_index() const { return gid_index_; }

uint32_t device::gid_type() const { return gid_type_; }

std::string device::gid_hex_string(union ibv_gid const &gid) {
  std::string gid_str;
  char buf[16] = {0};
  const static size_t kGidLength = 16;
  for (size_t i = 0; i < kGidLength; ++i) {
    ::snprintf(buf, 16, "%02x", gid.raw[i]);
    gid_str += i == 0 ? buf : std::string(":") + buf;
  }

  return gid_str;
}

device::~device() {
  if (device_list_ != nullptr) {
    device_ = nullptr; // avoid dangling pointer
  }
  if (ctx_ == nullptr) [[unlikely]] {
    return;
  }

  auto const gid_str = gid_hex_string(gid_);

  if (auto rc = ::ibv_close_device(ctx_); rc != 0) [[unlikely]] {
    log::error("failed to close device gid={} lid={}: {}", gid_str,
               port_attr_.lid, ::strerror(rc));
  } else {
    log::debug("closed device gid={} lid={}", gid_str, port_attr_.lid);
  }
}

} // namespace rdmapp
