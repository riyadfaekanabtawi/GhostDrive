#include "ghostdrive/daemon/codec.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

#include "ghostdrive/core/version_vector.hpp"
#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::daemon {

using core::Errc;
using core::Error;
namespace wire = transport::wire;

namespace {

// Zeroes its buffer when done, since some frames carry the drive secret. Encoders reserve
// enough up front that growth does not leave copies behind.
class Writer {
public:
    explicit Writer(std::size_t reserve = 256) { out_.reserve(reserve); }
    ~Writer() { core::secure_zero(out_); }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void u8(std::uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u16(std::uint16_t v) { put(v, 2); }
    void u32(std::uint32_t v) { put(v, 4); }
    void u64(std::uint64_t v) { put(v, 8); }
    void i64(std::int64_t v) { put(static_cast<std::uint64_t>(v), 8); }
    void bytes(std::span<const std::byte> b) { out_.insert(out_.end(), b.begin(), b.end()); }
    void str16(std::string_view s) {
        u16(static_cast<std::uint16_t>(s.size()));
        bytes(core::as_bytes(s));
    }
    void blob32(std::span<const std::byte> b) {
        u32(static_cast<std::uint32_t>(b.size()));
        bytes(b);
    }

    core::Bytes payload() const { return out_; }

    core::Bytes frame(wire::FrameType type) const {
        auto header = wire::encode_header(type, static_cast<std::uint32_t>(out_.size()));
        core::Bytes frame(header.begin(), header.end());
        frame.insert(frame.end(), out_.begin(), out_.end());
        return frame;
    }

private:
    void put(std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out_.push_back(static_cast<std::byte>(v >> (8 * i)));
    }
    core::Bytes out_;
};

// Bounds-checked little-endian reader. Every getter returns nullopt once the input runs out.
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) : in_(in) {}

    std::optional<std::uint8_t> u8() { return get_as<std::uint8_t>(1); }
    std::optional<std::uint16_t> u16() { return get_as<std::uint16_t>(2); }
    std::optional<std::uint32_t> u32() { return get_as<std::uint32_t>(4); }
    std::optional<std::uint64_t> u64() { return get(8); }
    std::optional<std::int64_t> i64() { return get_as<std::int64_t>(8); }
    std::optional<std::span<const std::byte>> take(std::size_t n) {
        if (in_.size() < n) return std::nullopt;
        auto out = in_.first(n);
        in_ = in_.subspan(n);
        return out;
    }
    std::optional<std::string> str16(std::size_t max) {
        auto len = u16();
        if (!len || *len > max) return std::nullopt;
        auto b = take(*len);
        if (!b) return std::nullopt;
        return std::string(reinterpret_cast<const char*>(b->data()), b->size());
    }
    template <std::size_t N>
    bool fixed(std::array<std::byte, N>& out) {
        auto b = take(N);
        if (!b) return false;
        std::copy(b->begin(), b->end(), out.begin());
        return true;
    }
    bool done() const noexcept { return in_.empty(); }

private:
    template <typename T>
    std::optional<T> get_as(int n) {
        auto v = get(n);
        if (!v) return std::nullopt;
        return static_cast<T>(*v);
    }
    std::optional<std::uint64_t> get(int n) {
        if (in_.size() < static_cast<std::size_t>(n)) return std::nullopt;
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            v |= std::to_integer<std::uint64_t>(in_[static_cast<std::size_t>(i)]) << (8 * i);
        }
        in_ = in_.subspan(static_cast<std::size_t>(n));
        return v;
    }
    std::span<const std::byte> in_;
};

Error malformed(const char* what) {
    return Error{Errc::protocol, std::string("malformed ") + what};
}

void put_record(Writer& w, const core::FileRecord& r) {
    w.str16(r.path);
    w.u64(r.size);
    w.u32(r.mode);
    w.i64(r.mtime_ns);
    w.u64(r.hlc);
    w.blob32(r.version.serialize());
    w.u64(r.modified_by);
    w.u32(r.block_size);
    w.u8(static_cast<std::uint8_t>(r.chunking));
    w.u8(r.deleted ? 1 : 0);
    w.u64(r.sequence);
    w.u32(static_cast<std::uint32_t>(r.chunks.size()));
    for (const auto& id : r.chunks) w.bytes(id.to_bytes());
}

std::optional<core::FileRecord> get_record(Reader& r) {
    core::FileRecord rec;
    auto path = r.str16(kMaxPathBytes);
    auto size = r.u64();
    auto mode = r.u32();
    auto mtime = r.i64();
    auto hlc = r.u64();
    auto version_len = r.u32();
    if (!path || !size || !mode || !mtime || !hlc || !version_len) return std::nullopt;
    auto version_bytes = r.take(*version_len);
    if (!version_bytes) return std::nullopt;
    auto version = core::VersionVector::parse(*version_bytes);
    if (!version) return std::nullopt;
    auto modified_by = r.u64();
    auto block_size = r.u32();
    auto chunking = r.u8();
    auto deleted = r.u8();
    auto sequence = r.u64();
    auto count = r.u32();
    if (!modified_by || !block_size || !chunking || !deleted || !sequence || !count) {
        return std::nullopt;
    }
    if (*count > kMaxChunksPerRecord ||
        *chunking != static_cast<std::uint8_t>(core::Chunking::fixed) || *deleted > 1) {
        return std::nullopt;
    }
    rec.path = std::move(*path);
    rec.size = *size;
    rec.mode = *mode;
    rec.mtime_ns = *mtime;
    rec.hlc = *hlc;
    rec.version = std::move(version).value();
    rec.modified_by = *modified_by;
    rec.block_size = *block_size;
    rec.chunking = core::Chunking::fixed;
    rec.deleted = *deleted == 1;
    rec.sequence = *sequence;
    rec.chunks.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        auto b = r.take(core::kChunkIdSize);
        if (!b) return std::nullopt;
        auto id = core::ChunkId::from_bytes(*b);
        if (!id) return std::nullopt;
        rec.chunks.push_back(id.value());
    }
    return rec;
}


constexpr std::string_view kMembershipContext = "ghostdrive-membership-v1";

void put_entry_body(Writer& w, const MembershipEntry& e) {
    w.u8(static_cast<std::uint8_t>(e.kind));
    w.u8(e.invited ? 1 : 0);
    w.bytes(e.author);
    w.bytes(e.subject);
    w.i64(e.time_ms);
    w.str16(e.name);
}

void put_entries(Writer& w, std::span<const MembershipEntry> entries) {
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        put_entry_body(w, e);
        w.bytes(e.signature);
    }
}

std::optional<std::vector<MembershipEntry>> get_entries(Reader& r) {
    auto count = r.u32();
    if (!count || *count > kMaxMembershipEntries) return std::nullopt;
    std::vector<MembershipEntry> out;
    // A claimed count is not trusted for the allocation; the entries must actually be there.
    out.reserve(std::min<std::size_t>(*count, 256));
    for (std::uint32_t i = 0; i < *count; ++i) {
        MembershipEntry e;
        auto kind = r.u8();
        auto invited = r.u8();
        if (!kind || !invited || !r.fixed(e.author) || !r.fixed(e.subject)) return std::nullopt;
        auto time = r.i64();
        auto name = r.str16(kMaxNameBytes);
        if (!time || !name || !r.fixed(e.signature)) return std::nullopt;
        if (*kind == static_cast<std::uint8_t>(MembershipEntry::Kind::add)) {
            if (*invited > 1) return std::nullopt;
        } else if (*kind == static_cast<std::uint8_t>(MembershipEntry::Kind::remove)) {
            if (*invited != 0 || !name->empty()) return std::nullopt;
        } else {
            return std::nullopt;
        }
        if (e.author == e.subject) return std::nullopt;
        e.kind = static_cast<MembershipEntry::Kind>(*kind);
        e.invited = *invited == 1;
        e.time_ms = *time;
        // Not made printable here: that would change the signed bytes. Names are cleaned
        // where they are shown or pinned.
        e.name = std::move(*name);
        out.push_back(std::move(e));
    }
    return out;
}

}  // namespace

std::string printable_name(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    std::size_t i = 0;
    while (i < name.size()) {
        auto lead = static_cast<unsigned char>(name[i]);
        std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                                          : (lead >> 3) == 0x1E ? 4 : 0;
        char32_t cp = len == 1 ? lead : len == 2 ? lead & 0x1F : len == 3 ? lead & 0x0F : lead & 0x07;
        bool valid = len != 0 && i + len <= name.size();
        for (std::size_t k = 1; valid && k < len; ++k) {
            auto c = static_cast<unsigned char>(name[i + k]);
            if ((c & 0xC0) != 0x80) valid = false;
            cp = (cp << 6) | (c & 0x3F);
        }
        // Overlong forms, surrogates and out-of-range values are invalid too.
        static constexpr char32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (valid && (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))) {
            valid = false;
        }
        bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
        bool bidi = cp == 0x061C || cp == 0x200E || cp == 0x200F ||
                    (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069);
        if (!valid || control || bidi) {
            out.push_back('?');
            i += valid ? len : 1;
            continue;
        }
        out.append(name.substr(i, len));
        i += len;
    }
    return out;
}

core::Bytes encode_hello(const Hello& hello) {
    Writer w;
    w.u64(hello.index_id);
    w.u64(hello.max_sequence);
    return w.frame(wire::FrameType::hello);
}

core::Result<Hello> decode_hello(std::span<const std::byte> payload) {
    Reader r(payload);
    auto id = r.u64();
    auto max = r.u64();
    if (!id || !max || !r.done()) return malformed("Hello");
    return Hello{*id, *max};
}

core::Bytes encode_index_request(const IndexRequest& request) {
    Writer w;
    w.u64(request.after);
    return w.frame(wire::FrameType::index_request);
}

core::Result<IndexRequest> decode_index_request(std::span<const std::byte> payload) {
    Reader r(payload);
    auto after = r.u64();
    if (!after || !r.done()) return malformed("IndexRequest");
    return IndexRequest{*after};
}

core::Bytes encode_index_batch(std::span<const IndexEntry> entries) {
    Writer w;
    w.u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        w.u8(e.sender_holds ? 1 : 0);
        put_record(w, e.record);
    }
    return w.frame(wire::FrameType::index_batch);
}

core::Result<std::vector<IndexEntry>> decode_index_batch(std::span<const std::byte> payload) {
    Reader r(payload);
    auto count = r.u32();
    if (!count || *count > kMaxBatchEntries) return malformed("IndexBatch");
    std::vector<IndexEntry> entries;
    entries.reserve(*count);
    for (std::uint32_t i = 0; i < *count; ++i) {
        auto holds = r.u8();
        if (!holds || *holds > 1) return malformed("IndexBatch");
        auto record = get_record(r);
        if (!record) return malformed("IndexBatch record");
        entries.push_back(IndexEntry{std::move(*record), *holds == 1});
    }
    if (!r.done()) return malformed("IndexBatch");
    return entries;
}

core::Bytes encode_join(const Join& join) {
    Writer w;
    w.bytes(join.token);
    w.str16(join.name);
    w.u16(join.listen_port);
    return w.frame(wire::FrameType::join);
}

core::Result<Join> decode_join(std::span<const std::byte> payload) {
    Reader r(payload);
    Join join;
    if (!r.fixed(join.token)) return malformed("Join");
    auto name = r.str16(kMaxNameBytes);
    auto port = r.u16();
    if (!name || !port || !r.done()) return malformed("Join");
    join.name = printable_name(*name);
    join.listen_port = *port;
    return join;
}

core::Bytes encode_drive_info(const DriveInfo& info) {
    Writer w(kDriveIdSize + core::kDriveSecretSize + 2 +
             info.members.size() * (core::kPublicKeySize + 4 + 2 * kMaxNameBytes));
    w.bytes(info.drive_id);
    w.bytes(info.secret);
    w.u16(static_cast<std::uint16_t>(info.members.size()));
    for (const auto& m : info.members) {
        w.bytes(m.key);
        w.str16(m.name);
        w.str16(m.address);
    }
    w.str16(info.rendezvous);
    put_entries(w, info.membership);
    return w.frame(wire::FrameType::drive_info);
}

core::Result<DriveInfo> decode_drive_info(std::span<const std::byte> payload) {
    Reader r(payload);
    DriveInfo info;
    if (!r.fixed(info.drive_id) || !r.fixed(info.secret)) return malformed("DriveInfo");
    auto count = r.u16();
    if (!count || *count > kMaxMembers) return malformed("DriveInfo");
    for (std::uint16_t i = 0; i < *count; ++i) {
        Member m;
        if (!r.fixed(m.key)) return malformed("DriveInfo member");
        auto name = r.str16(kMaxNameBytes);
        auto address = r.str16(kMaxNameBytes);
        if (!name || !address) return malformed("DriveInfo member");
        m.name = printable_name(*name);
        m.address = std::move(*address);
        info.members.push_back(std::move(m));
    }
    auto rendezvous = r.str16(kMaxNameBytes);
    if (!rendezvous) return malformed("DriveInfo");
    info.rendezvous = std::move(*rendezvous);
    auto entries = get_entries(r);
    if (!entries || !r.done()) return malformed("DriveInfo membership");
    info.membership = std::move(*entries);
    return info;
}

core::Bytes membership_signed_bytes(const DriveId& drive, const MembershipEntry& entry) {
    Writer w;
    w.bytes(core::as_bytes(kMembershipContext));
    w.bytes(drive);
    put_entry_body(w, entry);
    return w.payload();
}

core::Bytes encode_membership(std::span<const MembershipEntry> entries) {
    Writer w;
    put_entries(w, entries);
    return w.frame(wire::FrameType::membership);
}

core::Bytes encode_membership_payload(std::span<const MembershipEntry> entries) {
    Writer w;
    put_entries(w, entries);
    return w.payload();
}

core::Result<std::vector<MembershipEntry>> decode_membership(std::span<const std::byte> payload) {
    Reader r(payload);
    auto entries = get_entries(r);
    if (!entries || !r.done()) return malformed("Membership");
    return std::move(*entries);
}

}  // namespace ghostdrive::daemon
