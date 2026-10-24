#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ghostdrive/core/chunk_id.hpp"
#include "ghostdrive/core/identity.hpp"

namespace ghostdrive::discovery {

// The identifier a device puts in its LAN advertisement (ARCHITECTURE.md section 3.4):
//   HMAC-SHA256(drive_secret, "ghostdrive-lan-v1" || epoch u64 LE || device public key)
// truncated to 12 bytes and base32 encoded. Members compute it for each device they know and
// so learn which member is advertising; anyone without the drive secret sees a value that
// changes every epoch and cannot be linked to a device or across epochs.
inline constexpr std::chrono::seconds kLanEpoch{3600};
inline constexpr std::size_t kLanIdSize = 12;

std::uint64_t lan_epoch(std::chrono::system_clock::time_point now) noexcept;

std::string lan_id(const core::DriveSecret& secret, const core::PublicKey& device,
                   std::uint64_t epoch);

// The member whose ID this is, checking the epochs either side of `now` to allow for clock
// skew between devices.
std::optional<core::PublicKey> match_lan_id(std::string_view id, const core::DriveSecret& secret,
                                            std::span<const core::PublicKey> members,
                                            std::chrono::system_clock::time_point now);

}  // namespace ghostdrive::discovery
