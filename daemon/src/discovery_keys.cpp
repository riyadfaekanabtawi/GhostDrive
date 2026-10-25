#include "ghostdrive/daemon/discovery_keys.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstring>
#include <tuple>

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;

namespace {

constexpr std::array<char, 4> kMagic{'G', 'D', 'K', '1'};

}  // namespace

DiscoveryKeys::DiscoveryKeys(const DriveId& drive, const core::PublicKey& self,
                             const core::DriveSecret& base)
    : drive_(drive), self_(self), base_(base) {
    // A drive starts with nobody removed.
    if (auto d = digest({})) removed_digest_ = d.value();
}

DiscoveryKeys::~DiscoveryKeys() {
    wipe();
}

DiscoveryKeys::DiscoveryKeys(DiscoveryKeys&& other) noexcept
    : drive_(other.drive_),
      self_(other.self_),
      base_(other.base_),
      by_author_(std::move(other.by_author_)),
      earlier_(std::move(other.earlier_)),
      removed_digest_(other.removed_digest_),
      version_(other.version_) {
    other.wipe();
}

DiscoveryKeys& DiscoveryKeys::operator=(DiscoveryKeys&& other) noexcept {
    if (this != &other) {
        wipe();
        drive_ = other.drive_;
        self_ = other.self_;
        base_ = other.base_;
        by_author_ = std::move(other.by_author_);
        earlier_ = std::move(other.earlier_);
        removed_digest_ = other.removed_digest_;
        version_ = other.version_;
        other.wipe();
    }
    return *this;
}

void DiscoveryKeys::wipe() noexcept {
    core::secure_zero(base_);
    for (auto& [author, k] : by_author_) core::secure_zero(k.key);
    by_author_.clear();
    for (auto& k : earlier_) core::secure_zero(k.key);
    earlier_.clear();
}

core::Result<DiscoveryKeys> DiscoveryKeys::parse(std::span<const std::byte> data,
                                                 const DriveId& drive, const core::PublicKey& self,
                                                 const core::DriveSecret& base) {
    auto corrupt = [] { return Error{Errc::corrupt, "the discovery keys file is damaged"}; };
    // Layout: magic | digest of the removed set | u32 length | keys payload | earlier keys payload.
    DiscoveryKeys out(drive, self, base);
    if (data.size() < kMagic.size() + out.removed_digest_.size() ||
        std::memcmp(data.data(), kMagic.data(), kMagic.size()) != 0) {
        return corrupt();
    }
    std::size_t pos = kMagic.size();
    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(pos), out.removed_digest_.size(),
                out.removed_digest_.begin());
    pos += out.removed_digest_.size();
    if (data.size() < pos + 4) return corrupt();
    std::size_t length = 0;
    for (std::size_t i = 0; i < 4; ++i)
        length |= std::to_integer<std::size_t>(data[pos + i]) << (8 * i);
    pos += 4;
    if (data.size() - pos < length) return corrupt();
    auto keys = decode_discovery_keys(data.subspan(pos, length));
    auto earlier = decode_discovery_keys(data.subspan(pos + length));
    bool valid = keys && earlier && earlier.value().size() <= kMaxEarlierKeys;
    auto signed_ok = [&](const DiscoveryKey& k) {
        return core::verify_signature(k.author, discovery_key_signed_bytes(drive, k), k.signature);
    };
    if (keys) {
        for (auto& k : keys.value()) {
            if (valid && !out.by_author_.contains(k.author) && signed_ok(k)) {
                out.by_author_[k.author] = k;
            } else {
                valid = false;
            }
            core::secure_zero(k.key);
        }
    }
    if (earlier) {
        for (auto& k : earlier.value()) {
            if (valid && signed_ok(k)) {
                out.earlier_.push_back(k);
            } else {
                valid = false;
            }
            core::secure_zero(k.key);
        }
    }
    if (!valid) return corrupt();
    return out;
}

core::Bytes DiscoveryKeys::serialize() const {
    auto all = keys();
    auto payload = encode_discovery_keys_payload(all);
    for (auto& k : all) core::secure_zero(k.key);
    std::vector<DiscoveryKey> before(earlier_.begin(), earlier_.end());
    auto earlier = encode_discovery_keys_payload(before);
    for (auto& k : before) core::secure_zero(k.key);
    core::Bytes out;
    out.reserve(kMagic.size() + removed_digest_.size() + 4 + payload.size() + earlier.size());
    for (char c : kMagic) out.push_back(static_cast<std::byte>(c));
    out.insert(out.end(), removed_digest_.begin(), removed_digest_.end());
    auto length = static_cast<std::uint32_t>(payload.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>(length >> (8 * i)));
    out.insert(out.end(), payload.begin(), payload.end());
    out.insert(out.end(), earlier.begin(), earlier.end());
    core::secure_zero(payload);
    core::secure_zero(earlier);
    return out;
}

const DiscoveryKey* DiscoveryKeys::best() const {
    const DiscoveryKey* out = nullptr;
    for (const auto& [author, k] : by_author_) {
        if (!out || std::tie(k.generation, k.author) > std::tie(out->generation, out->author)) {
            out = &k;
        }
    }
    return out;
}

const core::DriveSecret& DiscoveryKeys::current() const noexcept {
    const auto* k = best();
    return k ? k->key : base_;
}

std::pair<std::uint64_t, core::PublicKey> DiscoveryKeys::current_id() const noexcept {
    const auto* k = best();
    return k ? std::pair{k->generation, k->author} : std::pair{std::uint64_t{0}, core::PublicKey{}};
}

std::vector<DiscoveryKeys::Held> DiscoveryKeys::all() const {
    std::vector<Held> out;
    const auto* first = best();
    if (first) out.push_back({first->generation, &first->key});
    for (const auto& [author, k] : by_author_) {
        if (&k != first) out.push_back({k.generation, &k.key});
    }
    for (const auto& k : earlier_) out.push_back({k.generation, &k.key});
    out.push_back({0, &base_});
    return out;
}

std::vector<const core::DriveSecret*> DiscoveryKeys::earlier() const {
    std::vector<const core::DriveSecret*> out;
    const auto& now = current();
    for (const auto& k : earlier_) {
        if (k.key != now) out.push_back(&k.key);
    }
    if (best()) out.push_back(&base_);
    return out;
}

std::optional<DiscoveryKey> DiscoveryKeys::best_copy() const {
    const auto* k = best();
    return k ? std::optional<DiscoveryKey>(*k) : std::nullopt;
}

void DiscoveryKeys::remember(const std::optional<DiscoveryKey>& before) {
    if (!before) return;
    const auto* now = best();
    if (now && *now == *before) return;
    if (std::any_of(earlier_.begin(), earlier_.end(),
                    [&](const DiscoveryKey& k) { return k == *before; })) {
        return;
    }
    earlier_.push_front(*before);
    while (earlier_.size() > kMaxEarlierKeys) {
        core::secure_zero(earlier_.back().key);
        earlier_.pop_back();
    }
}

std::vector<DiscoveryKey> DiscoveryKeys::keys() const {
    std::vector<DiscoveryKey> out;
    out.reserve(by_author_.size());
    for (const auto& [author, k] : by_author_) out.push_back(k);
    return out;
}

core::Result<DiscoveryKeys::Digest> DiscoveryKeys::digest(
    const std::set<core::PublicKey>& removed) {
    core::Bytes data;
    data.reserve(removed.size() * core::kPublicKeySize);
    for (const auto& key : removed) data.insert(data.end(), key.begin(), key.end());
    Digest out{};
    std::size_t len = 0;
    if (EVP_Q_digest(nullptr, "SHA256", nullptr, data.data(), data.size(),
                     reinterpret_cast<unsigned char*>(out.data()), &len) != 1 ||
        len != out.size()) {
        return Error{Errc::crypto, "could not hash the removed devices"};
    }
    return out;
}

bool DiscoveryKeys::prune(const std::set<core::PublicKey>& members) {
    bool changed = false;
    for (auto it = by_author_.begin(); it != by_author_.end();) {
        if (it->first != self_ && !members.contains(it->first)) {
            core::secure_zero(it->second.key);
            it = by_author_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    return changed;
}

core::Result<bool> DiscoveryKeys::update(const core::DeviceKey& key,
                                         const std::set<core::PublicKey>& members,
                                         const std::set<core::PublicKey>& removed) {
    auto before = best_copy();
    bool changed = prune(members);
    auto d = digest(removed);
    if (!d) return d.error();
    if (d.value() != removed_digest_) {
        std::uint64_t top = 0;
        for (const auto& [author, k] : by_author_) top = std::max(top, k.generation);
        if (top + 1 >= kMaxDiscoveryGeneration) {
            return Error{Errc::invalid_argument, "discovery key generations are used up"};
        }
        DiscoveryKey made;
        made.generation = top + 1;
        made.author = self_;
        if (auto st = core::random_bytes(made.key); !st) return st.error();
        auto message = discovery_key_signed_bytes(drive_, made);
        auto signature = key.sign(message);
        core::secure_zero(message);
        if (!signature) {
            core::secure_zero(made.key);
            return signature.error();
        }
        made.signature = signature.value();
        auto& slot = by_author_[self_];
        core::secure_zero(slot.key);
        slot = made;
        core::secure_zero(made.key);
        removed_digest_ = d.value();
        changed = true;
    }
    remember(before);
    if (before) core::secure_zero(before->key);
    if (changed) ++version_;
    return changed;
}

bool DiscoveryKeys::merge(std::span<const DiscoveryKey> keys,
                          const std::set<core::PublicKey>& members) {
    auto before = best_copy();
    bool changed = prune(members);
    // Counted from before the frame, and one key per author per frame, so one frame climbs at
    // most one step.
    std::uint64_t top = 0;
    for (const auto& [author, held] : by_author_) top = std::max(top, held.generation);
    std::set<core::PublicKey> taken;
    for (const auto& k : keys) {
        if (k.author == self_ || !members.contains(k.author) || k.generation == 0 ||
            k.generation >= kMaxDiscoveryGeneration ||
            k.generation > top + kMaxDiscoveryGenerationStep || !taken.insert(k.author).second) {
            continue;
        }
        auto it = by_author_.find(k.author);
        if (it != by_author_.end() && it->second.generation >= k.generation) continue;
        auto message = discovery_key_signed_bytes(drive_, k);
        bool valid = core::verify_signature(k.author, message, k.signature);
        core::secure_zero(message);
        if (!valid) continue;
        auto& slot = by_author_[k.author];
        core::secure_zero(slot.key);
        slot = k;
        changed = true;
    }
    remember(before);
    if (before) core::secure_zero(before->key);
    if (changed) ++version_;
    return changed;
}

void DiscoveryKeys::set_removed(const std::set<core::PublicKey>& removed) {
    if (auto d = digest(removed)) removed_digest_ = d.value();
}

}  // namespace ghostdrive::daemon
