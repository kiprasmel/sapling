/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2.
 */

#pragma once

#include <fmt/format.h>
#include <folly/portability/Windows.h>
#include "eden/common/utils/WinError.h"

#ifdef _WIN32

namespace facebook::eden {

class Guid {
 public:
  static Guid generate();

  explicit Guid(const std::string& s);

  Guid() = default;
  Guid(const GUID& guid) noexcept : guid_{guid} {}

  Guid(const Guid& other) noexcept : guid_{other.guid_} {}

  Guid& operator=(const Guid& other) noexcept {
    guid_ = other.guid_;
    return *this;
  }

  const GUID& getGuid() const noexcept {
    return guid_;
  }

  operator const GUID&() const noexcept {
    return guid_;
  }

  operator const GUID*() const noexcept {
    return &guid_;
  }

  bool operator==(const Guid& other) const noexcept {
    return guid_ == other.guid_;
  }

  bool operator!=(const Guid& other) const noexcept {
    return !(*this == other);
  }

  std::string toString() const noexcept {
    return fmt::format(
        FMT_STRING(
            "{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}"),
        guid_.Data1,
        guid_.Data2,
        guid_.Data3,
        guid_.Data4[0],
        guid_.Data4[1],
        guid_.Data4[2],
        guid_.Data4[3],
        guid_.Data4[4],
        guid_.Data4[5],
        guid_.Data4[6],
        guid_.Data4[7]);
  }

 private:
  GUID guid_{};
};

} // namespace facebook::eden

namespace std {
template <>
struct hash<facebook::eden::Guid> {
  size_t operator()(const facebook::eden::Guid& guid) const {
    return folly::hash::SpookyHashV2::Hash64(
        reinterpret_cast<const void*>(&guid), sizeof(guid), 0);
  }
};
} // namespace std

template <>
struct fmt::formatter<facebook::eden::Guid> : formatter<string_view> {
  template <typename Context>
  auto format(const facebook::eden::Guid& guid, Context& ctx) const {
    return formatter<string_view>::format(guid.toString(), ctx);
  }
};

#endif
