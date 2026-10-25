#include "ghostdrive/discovery/rendezvous_codec.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <string_view>

#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::discovery {

using core::Errc;
using core::Error;
namespace wire = transport::wire;

namespace {

constexpr std::string_view kGroupLabel = "ghostdrive-rendezvous-group-v1";
constexpr std::string_view kAdmissionLabel = "ghostdrive-rendezvous-admission-v1";

// HMAC-SHA256(key, label). A silent failure would leave every drive with the all-zero ID.
core::Result<GroupId> derive(const core::DriveSecret& key, std::string_view label) {
    GroupId out{};
    std::size_t len = 0;
    if (EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key.data(), key.size(),
                  reinterpret_cast<const unsigned char*>(label.data()), label.size(),
                  reinterpret_cast<unsigned char*>(out.data()), out.size(), &len) == nullptr ||
        len != out.size()) {
        return Error{Errc::crypto, "could not derive a rendezvous ID"};
    }
    return out;
}

void put_groups(core::Bytes& out, const std::vector<GroupId>& groups, std::size_t limit) {
    auto n = std::min(groups.size(), limit);
    out.push_back(static_cast<std::byte>(n));
    for (std::size_t i = 0; i < n; ++i) out.insert(out.end(), groups[i].begin(), groups[i].end());
}

// A count u8 and that many groups, at most `limit`; consumes them from `in`.
bool get_groups(std::span<const std::byte>& in, std::size_t limit, std::vector<GroupId>& out) {
    if (in.empty()) return false;
    auto n = std::to_integer<std::size_t>(in[0]);
    in = in.subspan(1);
    if (n > limit || in.size() < n * kGroupIdSize) return false;
    for (std::size_t i = 0; i < n; ++i) {
        GroupId g{};
        std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(i * kGroupIdSize), kGroupIdSize,
                    g.begin());
        out.push_back(g);
    }
    in = in.subspan(n * kGroupIdSize);
    return true;
}

core::Bytes frame(wire::FrameType type, const core::Bytes& payload) {
    auto header = wire::encode_header(type, static_cast<std::uint32_t>(payload.size()));
    core::Bytes out(header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// family u8 (0 none, 4, 6) | address | port u16 big endian
void put_endpoint(core::Bytes& out, const std::optional<asio::ip::udp::endpoint>& ep) {
    if (!ep) {
        out.push_back(std::byte{0});
        return;
    }
    if (ep->address().is_v4()) {
        out.push_back(std::byte{4});
        for (auto b : ep->address().to_v4().to_bytes()) out.push_back(static_cast<std::byte>(b));
    } else {
        out.push_back(std::byte{6});
        for (auto b : ep->address().to_v6().to_bytes()) out.push_back(static_cast<std::byte>(b));
    }
    out.push_back(static_cast<std::byte>(ep->port() >> 8));
    out.push_back(static_cast<std::byte>(ep->port() & 0xFF));
}

// Returns false on malformed input; sets `ep` to nullopt for "none".
bool get_endpoint(std::span<const std::byte>& in, std::optional<asio::ip::udp::endpoint>& ep) {
    if (in.empty()) return false;
    auto family = std::to_integer<int>(in[0]);
    in = in.subspan(1);
    std::size_t len = family == 4 ? 4 : family == 6 ? 16 : family == 0 ? 0 : 99;
    if (len == 99 || in.size() < len + (family == 0 ? 0 : 2)) return false;
    if (family == 0) {
        ep.reset();
        return true;
    }
    asio::ip::address address;
    if (family == 4) {
        asio::ip::address_v4::bytes_type b{};
        for (std::size_t i = 0; i < 4; ++i) b[i] = std::to_integer<unsigned char>(in[i]);
        address = asio::ip::address_v4(b);
    } else {
        asio::ip::address_v6::bytes_type b{};
        for (std::size_t i = 0; i < 16; ++i) b[i] = std::to_integer<unsigned char>(in[i]);
        address = asio::ip::address_v6(b);
    }
    auto port = static_cast<std::uint16_t>(std::to_integer<unsigned>(in[len]) << 8 |
                                           std::to_integer<unsigned>(in[len + 1]));
    in = in.subspan(len + 2);
    ep = asio::ip::udp::endpoint(address, port);
    return true;
}

Error malformed(const char* what) { return Error{Errc::protocol, std::string("malformed ") + what}; }

}  // namespace

core::Result<GroupId> rendezvous_group(const core::DriveSecret& key) {
    return derive(key, kGroupLabel);
}

core::Result<GroupId> rendezvous_admission(const core::DriveSecret& key) {
    return derive(key, kAdmissionLabel);
}

core::Bytes encode(const Register& m) {
    core::Bytes p(m.group.begin(), m.group.end());
    put_groups(p, m.admissions, kMaxAdmissions);
    put_groups(p, m.hidden, kMaxHiddenGroups);
    return frame(wire::FrameType::rendezvous_register, p);
}

core::Bytes encode_fixed_register(const Register& m) {
    core::Bytes p(m.group.begin(), m.group.end());
    const auto& admission = m.admissions.empty() ? m.group : m.admissions.back();
    p.insert(p.end(), admission.begin(), admission.end());
    put_groups(p, m.hidden, kMaxHiddenGroups);
    return frame(wire::FrameType::rendezvous_register, p);
}

core::Bytes encode(const Registered& m) {
    core::Bytes p;
    put_endpoint(p, m.observed);
    return frame(wire::FrameType::rendezvous_registered, p);
}

core::Bytes encode(const Connect& m) {
    return frame(wire::FrameType::rendezvous_connect, core::Bytes(m.peer.begin(), m.peer.end()));
}

core::Bytes encode(const PeerAt& m) {
    core::Bytes p(m.peer.begin(), m.peer.end());
    put_endpoint(p, m.address);
    return frame(wire::FrameType::rendezvous_peer_at, p);
}

core::Result<Register> decode_register(std::span<const std::byte> payload) {
    Register m;
    if (payload.size() < kGroupIdSize) return malformed("Register");
    std::copy_n(payload.begin(), kGroupIdSize, m.group.begin());
    auto rest = payload.subspan(kGroupIdSize);
    switch (payload.size() % kGroupIdSize) {
        case 0:  // an older client: group only, which admits it
            if (!rest.empty()) return malformed("Register");
            m.admissions.push_back(m.group);
            return m;
        case 1: {  // group | admission | hidden
            if (rest.size() < kGroupIdSize) return malformed("Register");
            GroupId admission{};
            std::copy_n(rest.begin(), kGroupIdSize, admission.begin());
            m.admissions.push_back(admission);
            rest = rest.subspan(kGroupIdSize);
            break;
        }
        case 2:  // group | admissions | hidden
            if (!get_groups(rest, kMaxAdmissions, m.admissions) || m.admissions.empty())
                return malformed("Register");
            break;
        default:
            return malformed("Register");
    }
    if (!get_groups(rest, kMaxHiddenGroups, m.hidden) || !rest.empty())
        return malformed("Register");
    return m;
}

core::Result<Registered> decode_registered(std::span<const std::byte> payload) {
    std::optional<asio::ip::udp::endpoint> ep;
    if (!get_endpoint(payload, ep) || !ep || !payload.empty()) return malformed("Registered");
    return Registered{*ep};
}

core::Result<Connect> decode_connect(std::span<const std::byte> payload) {
    if (payload.size() != core::kPublicKeySize) return malformed("Connect");
    Connect m;
    std::copy(payload.begin(), payload.end(), m.peer.begin());
    return m;
}

core::Result<PeerAt> decode_peer_at(std::span<const std::byte> payload) {
    if (payload.size() < core::kPublicKeySize) return malformed("PeerAt");
    PeerAt m;
    std::copy_n(payload.begin(), core::kPublicKeySize, m.peer.begin());
    payload = payload.subspan(core::kPublicKeySize);
    if (!get_endpoint(payload, m.address) || !payload.empty()) return malformed("PeerAt");
    return m;
}

}  // namespace ghostdrive::discovery
