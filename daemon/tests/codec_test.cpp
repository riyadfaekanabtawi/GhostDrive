#include "ghostdrive/daemon/codec.hpp"

#include <gtest/gtest.h>

#include "ghostdrive/transport/frame.hpp"

namespace ghostdrive::daemon {
namespace {

namespace wire = transport::wire;

std::span<const std::byte> payload_of(const core::Bytes& frame, wire::FrameType expected) {
    auto header = wire::decode_header(
        std::span<const std::byte, wire::kHeaderSize>(frame.data(), wire::kHeaderSize));
    EXPECT_TRUE(header);
    EXPECT_EQ(header.value().type, expected);
    EXPECT_EQ(header.value().length, frame.size() - wire::kHeaderSize);
    return std::span(frame).subspan(wire::kHeaderSize);
}

core::FileRecord sample_record() {
    core::FileRecord r;
    r.path = "photos/2026/beach.jpg";
    r.size = 300'000;
    r.mode = 0644;
    r.mtime_ns = -5;  // negative times survive the trip
    r.hlc = 0x1234'5678'9ABC;
    r.version.bump(7);
    r.version.bump(7);
    r.version.bump(99);
    r.modified_by = 99;
    r.block_size = 128 * 1024;
    r.sequence = 42;
    for (std::uint8_t i = 0; i < 3; ++i) {
        core::ChunkId id;
        id.digest.fill(std::byte{i});
        r.chunks.push_back(id);
    }
    core::Signature signature{};
    for (std::size_t i = 0; i < signature.size(); ++i) signature[i] = static_cast<std::byte>(i);
    r.signature = signature;
    r.origin_path = "photos/2026/beach-original.jpg";
    return r;
}

TEST(Codec, HelloAndRequestRoundTrip) {
    auto frame = encode_hello(Hello{0xAABB, 17});
    auto hello = decode_hello(payload_of(frame, wire::FrameType::hello));
    ASSERT_TRUE(hello);
    EXPECT_EQ(hello.value().index_id, 0xAABBu);
    EXPECT_EQ(hello.value().max_sequence, 17u);

    auto req = encode_index_request(IndexRequest{99});
    auto decoded = decode_index_request(payload_of(req, wire::FrameType::index_request));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().after, 99u);
}

TEST(Codec, IndexBatchRoundTrip) {
    auto deleted = sample_record();
    deleted.path = "old.txt";
    deleted.origin_path.clear();
    deleted.deleted = true;
    deleted.chunks.clear();
    deleted.size = 0;
    std::vector<IndexEntry> entries{{sample_record(), true}, {deleted, false}};
    auto frame = encode_index_batch(entries);
    auto decoded = decode_index_batch(payload_of(frame, wire::FrameType::index_batch2));
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_EQ(decoded.value().size(), 2u);
    const auto& a = decoded.value()[0].record;
    auto expected = sample_record();
    EXPECT_TRUE(decoded.value()[0].sender_holds);
    EXPECT_EQ(a.path, expected.path);
    EXPECT_EQ(a.size, expected.size);
    EXPECT_EQ(a.mode, expected.mode);
    EXPECT_EQ(a.mtime_ns, expected.mtime_ns);
    EXPECT_EQ(a.hlc, expected.hlc);
    EXPECT_EQ(a.version, expected.version);
    EXPECT_EQ(a.modified_by, expected.modified_by);
    EXPECT_EQ(a.block_size, expected.block_size);
    EXPECT_EQ(a.sequence, expected.sequence);
    EXPECT_EQ(a.chunks, expected.chunks);
    EXPECT_EQ(a.signature, expected.signature);
    EXPECT_EQ(a.origin_path, expected.origin_path);
    EXPECT_TRUE(decoded.value()[1].record.origin_path.empty());
    EXPECT_FALSE(decoded.value()[1].sender_holds);
    EXPECT_TRUE(decoded.value()[1].record.deleted);
}

TEST(Codec, TruncatedOrPaddedPayloadsAreRejected) {
    std::vector<IndexEntry> entries{{sample_record(), true}};
    auto frame = encode_index_batch(entries);
    auto payload = payload_of(frame, wire::FrameType::index_batch2);
    for (std::size_t cut = 0; cut < payload.size(); cut += 7) {
        EXPECT_FALSE(decode_index_batch(payload.first(cut))) << "cut at " << cut;
    }
    // Cut inside the signature, which ends 2 + origin_path bytes before the end.
    const std::size_t signature_end = payload.size() - 2 - sample_record().origin_path.size();
    for (std::size_t cut = signature_end - core::kSignatureSize; cut < signature_end; ++cut) {
        EXPECT_FALSE(decode_index_batch(payload.first(cut))) << "cut at " << cut;
    }
    core::Bytes padded(payload.begin(), payload.end());
    padded.push_back(std::byte{0});
    EXPECT_FALSE(decode_index_batch(padded));
}

TEST(Codec, OversizedCountsAreRejectedBeforeAllocating) {
    // A batch claiming more entries than the limit.
    core::Bytes huge{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0x7F}};
    auto r = decode_index_batch(huge);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, core::Errc::protocol);
}

TEST(Codec, JoinAndDriveInfoRoundTrip) {
    Join join;
    join.token.fill(std::byte{0x5A});
    join.name = "laptop";
    join.listen_port = 7373;
    auto jf = encode_join(join);
    auto j = decode_join(payload_of(jf, wire::FrameType::join));
    ASSERT_TRUE(j);
    EXPECT_EQ(j.value().token, join.token);
    EXPECT_EQ(j.value().name, "laptop");
    EXPECT_EQ(j.value().listen_port, 7373);

    DriveInfo info;
    info.drive_id.fill(std::byte{1});
    info.secret.fill(std::byte{2});
    Member m;
    m.key.fill(std::byte{3});
    m.name = "desktop";
    m.address = "192.168.1.20:7373";
    info.members.push_back(m);
    auto df = encode_drive_info(info);
    auto d = decode_drive_info(payload_of(df, wire::FrameType::drive_info));
    ASSERT_TRUE(d);
    EXPECT_EQ(d.value().drive_id, info.drive_id);
    EXPECT_EQ(d.value().secret, info.secret);
    ASSERT_EQ(d.value().members.size(), 1u);
    EXPECT_EQ(d.value().members[0].key, m.key);
    EXPECT_EQ(d.value().members[0].address, m.address);
    EXPECT_TRUE(d.value().membership.empty());

    MembershipEntry e;
    e.kind = MembershipEntry::Kind::add;
    e.invited = true;
    e.author.fill(std::byte{4});
    e.subject.fill(std::byte{5});
    e.time_ms = 1'700'000'000'000;
    e.name = "phone";
    e.signature.fill(std::byte{6});
    MembershipEntry r = e;
    r.kind = MembershipEntry::Kind::remove;
    r.invited = false;
    r.name.clear();
    info.membership = {e, r};
    auto with = encode_drive_info(info);
    auto dm = decode_drive_info(payload_of(with, wire::FrameType::drive_info));
    ASSERT_TRUE(dm);
    EXPECT_EQ(dm.value().membership, info.membership);
    auto batch = encode_membership(info.membership);
    auto b = decode_membership(payload_of(batch, wire::FrameType::membership));
    ASSERT_TRUE(b);
    EXPECT_EQ(b.value(), info.membership);
}

TEST(Codec, DiscoveryKeysRoundTripAndBadOnesAreRejected) {
    DiscoveryKey k;
    k.generation = 3;
    k.author.fill(std::byte{1});
    k.key.fill(std::byte{2});
    k.signature.fill(std::byte{3});
    auto frame = encode_discovery_keys(std::span(&k, 1));
    auto decoded = decode_discovery_keys(payload_of(frame, wire::FrameType::discovery_keys));
    ASSERT_TRUE(decoded);
    ASSERT_EQ(decoded.value().size(), 1u);
    EXPECT_EQ(decoded.value()[0], k);
    auto payload = payload_of(frame, wire::FrameType::discovery_keys);
    EXPECT_FALSE(decode_discovery_keys(payload.first(payload.size() - 1)));

    DriveInfo info;
    info.discovery_keys = {k};
    auto df = encode_drive_info(info);
    auto d = decode_drive_info(payload_of(df, wire::FrameType::drive_info));
    ASSERT_TRUE(d);
    EXPECT_EQ(d.value().discovery_keys, info.discovery_keys);

    // Generation 0 is the drive secret, never sent; generations near the top are refused so
    // they cannot run out.
    for (std::uint64_t generation : {std::uint64_t{0}, kMaxDiscoveryGeneration}) {
        auto bad = k;
        bad.generation = generation;
        auto bf = encode_discovery_keys(std::span(&bad, 1));
        EXPECT_FALSE(decode_discovery_keys(payload_of(bf, wire::FrameType::discovery_keys)));
    }
    std::vector<DiscoveryKey> many(kMaxDiscoveryKeys + 1, k);
    auto mf = encode_discovery_keys(many);
    EXPECT_FALSE(decode_discovery_keys(payload_of(mf, wire::FrameType::discovery_keys)));

    // The signature covers the drive, the generation, the author and the key.
    DriveId drive{};
    auto base = discovery_key_signed_bytes(drive, k);
    auto other = k;
    other.generation = 4;
    EXPECT_NE(discovery_key_signed_bytes(drive, other), base);
    other = k;
    other.key[0] = std::byte{9};
    EXPECT_NE(discovery_key_signed_bytes(drive, other), base);
    drive[0] = std::byte{1};
    EXPECT_NE(discovery_key_signed_bytes(drive, k), base);
}

TEST(Codec, PrintableNameKeepsTextAndReplacesControls) {
    EXPECT_EQ(printable_name("Riyad's MacBook"), "Riyad's MacBook");
    EXPECT_EQ(printable_name("caf\xC3\xA9"), "caf\xC3\xA9");  // UTF-8 kept
    EXPECT_EQ(printable_name("a\x1b[2Jb"), "a?[2Jb");
    EXPECT_EQ(printable_name("a\x9b" "1mb"), "a?1mb");  // raw 8-bit CSI
    EXPECT_EQ(printable_name("a\xC2\x9B" "b"), "a?b");  // C1 control as UTF-8
    EXPECT_EQ(printable_name("a\xE2\x80\xAE" "b"), "a?b");  // right-to-left override
    EXPECT_EQ(printable_name("a\xC0\xAF" "b"), "a??b");  // overlong encoding
    EXPECT_EQ(printable_name("a\xE2\x80"), "a??");  // truncated sequence
}

TEST(Codec, MalformedMembershipEntriesAreRejected) {
    MembershipEntry e;
    e.author.fill(std::byte{4});
    e.subject.fill(std::byte{5});
    e.name = "phone";
    auto self_add = e;
    self_add.subject = self_add.author;
    auto named_remove = e;
    named_remove.kind = MembershipEntry::Kind::remove;
    auto unknown_kind = e;
    unknown_kind.kind = static_cast<MembershipEntry::Kind>(9);
    for (const auto& bad : {self_add, named_remove, unknown_kind}) {
        auto frame = encode_membership(std::span(&bad, 1));
        EXPECT_FALSE(decode_membership(payload_of(frame, wire::FrameType::membership)));
    }
    auto good = encode_membership(std::span(&e, 1));
    auto payload = payload_of(good, wire::FrameType::membership);
    EXPECT_TRUE(decode_membership(payload));
    EXPECT_FALSE(decode_membership(payload.first(payload.size() - 1)));
}


TEST(Codec, DeviceNamesLoseControlBytes) {
    Join join;
    join.name = "evil\x1b[2Jname\n";
    auto frame = encode_join(join);
    auto decoded = decode_join(payload_of(frame, wire::FrameType::join));
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded.value().name, "evil?[2Jname?");
}

}  // namespace
}  // namespace ghostdrive::daemon
