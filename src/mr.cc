#include "rdmapp/mr.h"

#include <cstdint>
#include <spdlog/spdlog.h>
#include <utility>
#include <vector>

#include <infiniband/verbs.h>

#include "rdmapp/detail/serdes.h"

namespace rdmapp {

local_mr::mr(std::shared_ptr<pd> pd, struct ibv_mr *mr) : mr_(mr), pd_(pd) {}

local_mr::mr(local_mr &&other)
    : mr_(std::exchange(other.mr_, nullptr)), pd_(std::move(other.pd_)) {}

local_mr &local_mr::operator=(local_mr &&other) {
  mr_ = other.mr_;
  pd_ = std::move(other.pd_);
  other.mr_ = nullptr;
  return *this;
}

local_mr::~mr() {
  if (mr_ == nullptr) [[unlikely]] {
    // This mr is moved.
    return;
  }
  auto addr = mr_->addr;
  if (auto rc = ::ibv_dereg_mr(mr_); rc != 0) [[unlikely]] {
    spdlog::error("failed to dereg mr {} addr={}", fmt::ptr(mr_),
                  fmt::ptr(addr));
  } else {
    spdlog::trace("dereg mr {} addr={}", fmt::ptr(mr_), fmt::ptr(addr));
  }
}

std::vector<uint8_t> local_mr::serialize() const {
  std::vector<uint8_t> buffer;
  auto it = std::back_inserter(buffer);
  detail::serialize(reinterpret_cast<uint64_t>(mr_->addr), it);
  detail::serialize(mr_->length, it);
  detail::serialize(mr_->rkey, it);
  return buffer;
}

void *local_mr::addr() const { return mr_->addr; }

size_t local_mr::length() const { return mr_->length; }

uint32_t local_mr::rkey() const { return mr_->rkey; }

uint32_t local_mr::lkey() const { return mr_->lkey; }

std::span<std::byte const> local_mr::span() const {
  return std::span<std::byte>(static_cast<std::byte *>(mr_->addr), mr_->length);
}

std::span<std::byte> local_mr::span() {
  return std::span<std::byte>(static_cast<std::byte *>(mr_->addr), mr_->length);
}

remote_mr::mr(void *addr, uint32_t length, uint32_t rkey)
    : addr_(addr), length_(length), rkey_(rkey) {}

void *remote_mr::addr() { return addr_; }

uint32_t remote_mr::length() { return length_; }

uint32_t remote_mr::rkey() { return rkey_; }

std::span<std::byte const> remote_mr::span() const {
  return std::span<std::byte>(static_cast<std::byte *>(addr_), length_);
}

std::span<std::byte> remote_mr::span() {
  return std::span<std::byte>(static_cast<std::byte *>(addr_), length_);
}

mr_view::mr(local_mr const &local, std::size_t offset, std::size_t length)
    : addr_(static_cast<std::byte *>(local.addr()) + offset),
      length_(std::min(length, local.length() - offset)), lkey_(local.lkey()) {}

void *mr_view::addr() const { return addr_; }

size_t mr_view::length() const { return length_; }

uint32_t mr_view::lkey() const { return lkey_; }

} // namespace rdmapp
