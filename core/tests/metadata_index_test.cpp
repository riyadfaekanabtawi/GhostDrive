#include "ghostdrive/core/metadata_index.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include "ghostdrive/core/chunking.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::core {
namespace {

using Kind = ApplyOutcome::Kind;

DeviceKey seeded_key(std::uint8_t fill) {
    std::array<std::byte, kSeedSize> seed{};
    seed.fill(std::byte{fill});
    return DeviceKey::from_seed(seed).value();
}

PublicKey key_with(std::uint8_t fill) {
    PublicKey key{};
    key.fill(std::byte{fill});
    return key;
}

ChunkId chunk_with(std::uint8_t fill) {
    ChunkId id;
    id.digest.fill(std::byte{fill});
    return id;
}

FileRecord file(std::string path, std::int64_t mtime_s, std::uint8_t chunk_fill) {
    FileRecord r;
    r.path = std::move(path);
    r.size = 100;
    r.mode = 0644;
    r.mtime_ns = mtime_s * 1'000'000'000LL;
    r.block_size = kMinBlockSize;
    r.chunks = {chunk_with(chunk_fill)};
    return r;
}

class MetadataIndexTest : public ::testing::Test {
protected:
    MetadataIndex open(std::string_view name, const DeviceKey& key) {
        auto index = MetadataIndex::open(dir_.path() / name, drive_, key);
        EXPECT_TRUE(index.ok()) << (index.ok() ? "" : index.error().message);
        return std::move(index).value();
    }

    // An index that has pinned the other test devices, A, B and C.
    MetadataIndex open_peer(std::string_view name, const DeviceKey& key) {
        MetadataIndex index = open(name, key);
        for (const DeviceKey* other : {&key_a_, &key_b_, &key_c_}) {
            if (other != &key) EXPECT_TRUE(index.add_device(other->public_key(), "peer"));
        }
        return index;
    }

    // A record as `key` would sign it.
    FileRecord signed_by(const DeviceKey& key, FileRecord record) const {
        record.modified_by = short_id(key.public_key());
        record.signature = key.sign(record_signed_bytes(drive_, record)).value();
        return record;
    }

    // A record as `key` would countersign it when adopting it.
    FileRecord countersigned_by(const DeviceKey& key, FileRecord record) const {
        record.countersigner = short_id(key.public_key());
        record.countersignature = key.sign(countersign_bytes(drive_, record)).value();
        return record;
    }

    // Sends every record `to` has not yet seen from `from`, as an index exchange would.
    static std::vector<ApplyOutcome> sync(const MetadataIndex& from, MetadataIndex& to,
                                          std::uint64_t& cursor, HybridClock& clock) {
        std::vector<ApplyOutcome> outcomes;
        auto records = from.records_since(cursor, 1000, true);
        EXPECT_TRUE(records.ok());
        for (const FileRecord& r : records.value()) {
            auto outcome = to.apply_remote(r, clock);
            EXPECT_TRUE(outcome.ok()) << outcome.error().message;
            outcomes.push_back(outcome.value());
            cursor = r.sequence;
        }
        return outcomes;
    }

    static void expect_same(const MetadataIndex& a, const MetadataIndex& b,
                            const std::string& path) {
        auto ra = a.get(path);
        auto rb = b.get(path);
        ASSERT_TRUE(ra.ok() && rb.ok());
        ASSERT_TRUE(ra.value().has_value()) << path;
        ASSERT_TRUE(rb.value().has_value()) << path;
        EXPECT_EQ(ra.value()->version, rb.value()->version) << path;
        EXPECT_EQ(ra.value()->chunks, rb.value()->chunks) << path;
        EXPECT_EQ(ra.value()->deleted, rb.value()->deleted) << path;
        EXPECT_EQ(ra.value()->modified_by, rb.value()->modified_by) << path;
    }

    test::TempDir dir_;
    const DriveId drive_ = [] {
        DriveId id{};
        id.fill(std::byte{0x5A});
        return id;
    }();
    const DeviceKey key_a_ = seeded_key(1);
    const DeviceKey key_b_ = seeded_key(2);
    const DeviceKey key_c_ = seeded_key(3);
    const std::uint64_t id_a_ = short_id(key_a_.public_key());
    const std::uint64_t id_b_ = short_id(key_b_.public_key());
    const std::uint64_t id_c_ = short_id(key_c_.public_key());
    std::uint64_t pt_ = 1'000'000;
    HybridClock clock_a_{[this] { return pt_; }};
    HybridClock clock_b_{[this] { return pt_; }};
};

TEST_F(MetadataIndexTest, CommitLocalAssignsVersionAndSequence) {
    MetadataIndex index = open("a.db", key_a_);
    EXPECT_EQ(index.max_sequence().value(), 0u);

    auto first = index.commit_local(file("docs/a.txt", 10, 1), clock_a_);
    ASSERT_TRUE(first.ok()) << first.error().message;
    EXPECT_EQ(first.value().sequence, 1u);
    EXPECT_EQ(first.value().version.get(id_a_), 1u);
    EXPECT_EQ(first.value().modified_by, id_a_);

    auto second = index.commit_local(file("docs/a.txt", 11, 2), clock_a_);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().sequence, 2u);
    EXPECT_EQ(second.value().version.get(id_a_), 2u);
    EXPECT_GT(second.value().hlc, first.value().hlc);

    auto stored = index.get("docs/a.txt");
    ASSERT_TRUE(stored.ok() && stored.value().has_value());
    EXPECT_EQ(stored.value()->chunks, std::vector<ChunkId>{chunk_with(2)});
    EXPECT_EQ(stored.value()->mode, 0644u);
    EXPECT_FALSE(index.get("missing").value().has_value());
}

TEST_F(MetadataIndexTest, RejectsInvalidRecords) {
    MetadataIndex index = open("a.db", key_a_);
    EXPECT_EQ(index.commit_local(file("../escape", 1, 1), clock_a_).error().code,
              Errc::invalid_argument);
    FileRecord bad = file("ok", 1, 1);
    bad.chunks.clear();
    EXPECT_EQ(index.apply_remote(bad, clock_a_).error().code, Errc::invalid_argument);
}

TEST_F(MetadataIndexTest, DeleteLeavesTombstone) {
    MetadataIndex index = open("a.db", key_a_);
    ASSERT_TRUE(index.commit_local(file("f", 1, 1), clock_a_).ok());
    auto tomb = index.delete_local("f", clock_a_);
    ASSERT_TRUE(tomb.ok());
    EXPECT_TRUE(tomb.value().deleted);
    EXPECT_TRUE(tomb.value().chunks.empty());
    EXPECT_EQ(tomb.value().version.get(id_a_), 2u);
    EXPECT_EQ(index.delete_local("f", clock_a_).error().code, Errc::not_found);

    auto since = index.records_since(0, 10);
    ASSERT_TRUE(since.ok());
    ASSERT_EQ(since.value().size(), 1u);
    EXPECT_TRUE(since.value()[0].deleted);
}

TEST_F(MetadataIndexTest, ApplyRemoteNewerAndOlder) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    std::uint64_t a_to_b = 0;

    ASSERT_TRUE(a.commit_local(file("f", 1, 1), clock_a_).ok());
    auto out = sync(a, b, a_to_b, clock_b_);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, Kind::applied);
    expect_same(a, b, "f");

    // Re-sending the same version is a no-op.
    auto again = a.get("f").value();
    EXPECT_EQ(b.apply_remote(*again, clock_b_).value().kind, Kind::ignored);

    // B edits on top of A's version; A takes it. A's old version is then ignored by B.
    ASSERT_TRUE(b.commit_local(file("f", 2, 2), clock_b_).ok());
    std::uint64_t b_to_a = 0;
    sync(b, a, b_to_a, clock_a_);
    expect_same(a, b, "f");
    EXPECT_EQ(b.apply_remote(*again, clock_b_).value().kind, Kind::ignored);
}

TEST_F(MetadataIndexTest, ConcurrentEditsConvergeWithConflictCopy) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    std::uint64_t a_to_b = 0;

    ASSERT_TRUE(a.commit_local(file("doc.txt", 1, 1), clock_a_).ok());
    sync(a, b, a_to_b, clock_b_);
    std::uint64_t b_to_a = b.max_sequence().value();

    // Both edit while partitioned; B's edit is later and should keep the name.
    ASSERT_TRUE(a.commit_local(file("doc.txt", 100, 2), clock_a_).ok());
    pt_ += 5000;
    ASSERT_TRUE(b.commit_local(file("doc.txt", 105, 3), clock_b_).ok());

    // Simultaneous exchange: each side sends its changes before seeing the other's.
    const auto from_a = a.records_since(a_to_b, 1000).value();
    const auto from_b = b.records_since(b_to_a, 1000).value();
    ASSERT_EQ(from_a.size(), 1u);
    ASSERT_EQ(from_b.size(), 1u);
    auto at_b = b.apply_remote(from_a[0], clock_b_);
    auto at_a = a.apply_remote(from_b[0], clock_a_);
    ASSERT_TRUE(at_a.ok() && at_b.ok());
    EXPECT_EQ(at_b.value().kind, Kind::conflict);
    EXPECT_EQ(at_a.value().kind, Kind::conflict);
    const std::string copy = at_a.value().conflict_path;
    EXPECT_EQ(copy, at_b.value().conflict_path);
    EXPECT_NE(copy.find("doc.sync-conflict-"), std::string::npos);

    // Further exchanges must settle without new conflicts.
    for (int round = 0; round < 2; ++round) {
        for (const auto& o : sync(a, b, a_to_b, clock_b_)) EXPECT_NE(o.kind, Kind::conflict);
        for (const auto& o : sync(b, a, b_to_a, clock_a_)) EXPECT_NE(o.kind, Kind::conflict);
    }

    expect_same(a, b, "doc.txt");
    expect_same(a, b, copy);
    EXPECT_EQ(a.get("doc.txt").value()->chunks, std::vector<ChunkId>{chunk_with(3)});
    EXPECT_EQ(a.get(copy).value()->chunks, std::vector<ChunkId>{chunk_with(2)});
}

TEST_F(MetadataIndexTest, ConcurrentEditBeatsDelete) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    std::uint64_t a_to_b = 0;

    ASSERT_TRUE(a.commit_local(file("f", 1, 1), clock_a_).ok());
    sync(a, b, a_to_b, clock_b_);
    std::uint64_t b_to_a = b.max_sequence().value();

    ASSERT_TRUE(a.commit_local(file("f", 2, 9), clock_a_).ok());
    pt_ += 5000;  // the delete is later, but must still lose
    ASSERT_TRUE(b.delete_local("f", clock_b_).ok());

    const auto from_a = a.records_since(a_to_b, 1000).value();
    const auto from_b = b.records_since(b_to_a, 1000).value();
    ASSERT_EQ(from_a.size(), 1u);
    ASSERT_EQ(from_b.size(), 1u);
    EXPECT_EQ(b.apply_remote(from_a[0], clock_b_).value().kind, Kind::applied);
    EXPECT_EQ(a.apply_remote(from_b[0], clock_a_).value().kind, Kind::kept_local);
    for (const auto& o : sync(a, b, a_to_b, clock_b_)) EXPECT_NE(o.kind, Kind::conflict);
    for (const auto& o : sync(b, a, b_to_a, clock_a_)) EXPECT_NE(o.kind, Kind::conflict);

    expect_same(a, b, "f");
    EXPECT_FALSE(b.get("f").value()->deleted);
    EXPECT_EQ(b.get("f").value()->chunks, std::vector<ChunkId>{chunk_with(9)});
}

TEST_F(MetadataIndexTest, RecordsSinceIsOrderedAndLimited) {
    MetadataIndex index = open("a.db", key_a_);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(index.commit_local(file("f" + std::to_string(i), i, 1), clock_a_).ok());
    }
    auto page = index.records_since(1, 2);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().size(), 2u);
    EXPECT_EQ(page.value()[0].path, "f1");
    EXPECT_EQ(page.value()[1].path, "f2");
    EXPECT_EQ(index.records_since(5, 10).value().size(), 0u);
}

TEST_F(MetadataIndexTest, ReopenPreservesStateAndChecksDevice) {
    std::uint64_t index_id = 0;
    {
        MetadataIndex index = open("a.db", key_a_);
        index_id = index.index_id();
        ASSERT_TRUE(index.commit_local(file("f", 1, 1), clock_a_).ok());
    }
    MetadataIndex index = open("a.db", key_a_);
    EXPECT_EQ(index.index_id(), index_id);
    EXPECT_EQ(index.max_sequence().value(), 1u);
    EXPECT_TRUE(index.get("f").value().has_value());

    auto wrong = MetadataIndex::open(dir_.path() / "a.db", drive_, key_b_);
    ASSERT_FALSE(wrong.ok());
    EXPECT_EQ(wrong.error().code, Errc::invalid_argument);
}

TEST_F(MetadataIndexTest, PinnedDevices) {
    MetadataIndex index = open("a.db", key_a_);
    const PublicKey peer = key_with(0x11);
    EXPECT_FALSE(index.is_pinned(peer).value());
    ASSERT_TRUE(index.add_device(peer, "laptop").ok());
    EXPECT_TRUE(index.is_pinned(peer).value());
    EXPECT_EQ(index.add_device(peer, "again").error().code, Errc::already_exists);

    ASSERT_TRUE(index.set_peer_progress(peer, 77, 12).ok());
    auto devices = index.devices();
    ASSERT_TRUE(devices.ok());
    ASSERT_EQ(devices.value().size(), 1u);
    EXPECT_EQ(devices.value()[0].name, "laptop");
    EXPECT_EQ(devices.value()[0].short_id, short_id(peer));
    EXPECT_EQ(devices.value()[0].peer_index_id, 77u);
    EXPECT_EQ(devices.value()[0].peer_max_sequence, 12u);

    ASSERT_TRUE(index.remove_device(peer).ok());
    EXPECT_FALSE(index.is_pinned(peer).value());
    EXPECT_EQ(index.remove_device(peer).error().code, Errc::not_found);
    EXPECT_EQ(index.set_peer_progress(peer, 1, 1).error().code, Errc::not_found);
}

TEST_F(MetadataIndexTest, ChunkHolders) {
    MetadataIndex index = open("a.db", key_a_);
    const PublicKey p1 = key_with(0x11);
    const PublicKey p2 = key_with(0x22);
    ASSERT_TRUE(index.add_device(p1, "one").ok());
    ASSERT_TRUE(index.add_device(p2, "two").ok());
    const ChunkId c = chunk_with(5);

    ASSERT_TRUE(index.set_holds(p2, c, true).ok());
    ASSERT_TRUE(index.set_holds(p1, c, true).ok());
    ASSERT_TRUE(index.set_holds(p1, c, true).ok());
    EXPECT_EQ(index.holders(c).value(), (std::vector<PublicKey>{p1, p2}));

    ASSERT_TRUE(index.set_holds(p1, c, false).ok());
    EXPECT_EQ(index.holders(c).value(), std::vector<PublicKey>{p2});

    EXPECT_EQ(index.set_holds(key_with(0x33), c, true).error().code, Errc::not_found);

    // Unpinning a device drops its holdings.
    ASSERT_TRUE(index.remove_device(p2).ok());
    EXPECT_TRUE(index.holders(c).value().empty());
}

TEST_F(MetadataIndexTest, RemoteRecordsCannotWrapClocksOrCounters) {
    auto index = open_peer("a.db", key_a_);
    HybridClock clock;  // real time, so "far future" is measured against now

    auto huge = file("counter.txt", 1, 1);
    for (int i = 0; i < 3; ++i) huge.version.bump(id_b_);
    huge.version = VersionVector::parse(huge.version.serialize()).value();
    // Build a counter at the edge directly through the wire format.
    Bytes raw = huge.version.serialize();
    for (std::size_t i = 12; i < 20; ++i) raw[i] = std::byte{0xFF};  // value of the only counter
    huge.version = VersionVector::parse(raw).value();
    huge.hlc = clock.now();
    EXPECT_EQ(index.apply_remote(huge, clock).error().code, Errc::invalid_argument);

    auto future = file("future.txt", 1, 2);
    future.version.bump(id_b_);
    future.hlc = (HybridClock::physical_ms(clock.now()) + 7ull * 24 * 3600 * 1000) << 16;
    EXPECT_EQ(index.apply_remote(future, clock).error().code, Errc::invalid_argument);

    auto fine = file("fine.txt", 1, 3);
    fine.version.bump(id_b_);
    fine.hlc = clock.now();
    EXPECT_TRUE(index.apply_remote(signed_by(key_b_, fine), clock));
}

TEST_F(MetadataIndexTest, LocalChangesAreSignedByThisDevice) {
    MetadataIndex a = open("a.db", key_a_);
    auto committed = a.commit_local(file("f", 1, 1), clock_a_);
    ASSERT_TRUE(committed);
    ASSERT_TRUE(committed.value().signature);
    EXPECT_TRUE(verify_signature(key_a_.public_key(),
                                 record_signed_bytes(drive_, committed.value()),
                                 *committed.value().signature));
    auto tomb = a.delete_local("f", clock_a_);
    ASSERT_TRUE(tomb && tomb.value().signature);
    EXPECT_TRUE(verify_signature(key_a_.public_key(), record_signed_bytes(drive_, tomb.value()),
                                 *tomb.value().signature));
}

TEST_F(MetadataIndexTest, AForgedModifiedByIsRejected) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    auto genuine = a.commit_local(file("f", 1, 1), clock_a_).value();

    // C relays A's record but claims it as its own.
    auto claimed = genuine;
    claimed.modified_by = id_c_;
    EXPECT_EQ(b.apply_remote(claimed, clock_b_).error().code, Errc::invalid_argument);
    // C makes up a change and attributes it to A, signing with its own key.
    auto made_up = signed_by(key_c_, file("g", 2, 2));
    made_up.modified_by = id_a_;
    made_up.version.bump(id_a_);
    EXPECT_EQ(b.apply_remote(made_up, clock_b_).error().code, Errc::invalid_argument);
    // C alters A's record while relaying it.
    auto altered = genuine;
    altered.chunks = {chunk_with(9)};
    EXPECT_EQ(b.apply_remote(altered, clock_b_).error().code, Errc::invalid_argument);
    auto unsigned_record = genuine;
    unsigned_record.signature.reset();
    EXPECT_EQ(b.apply_remote(unsigned_record, clock_b_).error().code, Errc::invalid_argument);
    EXPECT_FALSE(b.get("f").value().has_value());
    EXPECT_FALSE(b.get("g").value().has_value());

    // A record signed for another drive does not verify here either.
    DriveId other{};
    auto foreign = genuine;
    foreign.signature = key_a_.sign(record_signed_bytes(other, genuine)).value();
    EXPECT_EQ(b.apply_remote(foreign, clock_b_).error().code, Errc::invalid_argument);

    EXPECT_EQ(b.apply_remote(genuine, clock_b_).value().kind, Kind::applied);
}

TEST_F(MetadataIndexTest, ARelayedRecordVerifiesAgainstItsAuthor) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    MetadataIndex c = open_peer("c.db", key_c_);
    ASSERT_TRUE(a.commit_local(file("f", 1, 1), clock_a_));
    std::uint64_t a_to_b = 0;
    std::uint64_t b_to_c = 0;
    sync(a, b, a_to_b, clock_b_);
    auto out = sync(b, c, b_to_c, clock_a_);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].kind, Kind::applied);
    EXPECT_EQ(c.get("f").value()->modified_by, id_a_);
    expect_same(a, c, "f");
}

TEST_F(MetadataIndexTest, AnUnknownSignerIsRejectedUntilPinned) {
    MetadataIndex a = open("a.db", key_a_);
    auto from_b = file("f", 1, 1);
    from_b.version.bump(id_b_);
    from_b = signed_by(key_b_, from_b);
    EXPECT_FALSE(a.is_active_signer(id_b_));
    // not_found: it may become valid, so the exchange fetches it again later.
    EXPECT_EQ(a.apply_remote(from_b, clock_a_).error().code, Errc::not_found);
    EXPECT_FALSE(a.get("f").value().has_value());

    ASSERT_TRUE(a.add_device(key_b_.public_key(), "b"));
    EXPECT_TRUE(a.is_active_signer(id_b_));
    EXPECT_EQ(a.apply_remote(from_b, clock_a_).value().kind, Kind::applied);
}

TEST_F(MetadataIndexTest, AShortIdStaysBoundToItsFirstKey) {
    MetadataIndex a = open("a.db", key_a_);
    ASSERT_TRUE(a.add_device(key_b_.public_key(), "b"));
    ASSERT_TRUE(a.remove_device(key_b_.public_key()));
    // Another key with B's short ID could otherwise sign records in B's name.
    PublicKey twin = key_b_.public_key();
    twin[31] ^= std::byte{1};
    EXPECT_EQ(a.add_device(twin, "twin").error().code, Errc::already_exists);
}

TEST_F(MetadataIndexTest, ARetiredSignerIsRejectedButItsRecordsStay) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    ASSERT_TRUE(b.commit_local(file("kept", 1, 1), clock_b_));
    ASSERT_TRUE(b.commit_local(file("gone", 1, 2), clock_b_));
    ASSERT_TRUE(b.delete_local("gone", clock_b_));
    std::uint64_t b_to_a = 0;
    sync(b, a, b_to_a, clock_a_);
    auto late = b.commit_local(file("late", 2, 3), clock_b_).value();

    // B is removed from the drive.
    ASSERT_TRUE(a.remove_device(key_b_.public_key()));
    auto adopted = a.retire_signer(key_b_.public_key());
    ASSERT_TRUE(adopted) << adopted.error().message;
    EXPECT_EQ(adopted.value(), 2u);  // the tombstone too
    EXPECT_FALSE(a.is_active_signer(id_b_));
    EXPECT_EQ(a.apply_remote(late, clock_a_).error().code, Errc::invalid_argument);
    EXPECT_FALSE(a.get("late").value().has_value());
    EXPECT_EQ(a.retire_signer(key_b_.public_key()).value(), 0u);  // done once

    // Its records are still readable, unchanged but for the signer.
    auto kept = a.get("kept").value();
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->chunks, std::vector<ChunkId>{chunk_with(1)});
    EXPECT_EQ(kept->modified_by, id_b_);
    EXPECT_EQ(kept->countersigner, id_a_);
    EXPECT_EQ(kept->signature, b.get("kept").value()->signature);
    EXPECT_EQ(kept->version, b.get("kept").value()->version);
    EXPECT_TRUE(a.get("gone").value()->deleted);

    // A device that joins later and never pinned B accepts them once it knows B was removed.
    MetadataIndex c = open("c.db", key_c_);
    ASSERT_TRUE(c.add_device(key_a_.public_key(), "a"));
    auto from_a = a.records_since(0, 100, true).value();
    ASSERT_EQ(from_a.size(), 2u);
    EXPECT_EQ(c.apply_remote(from_a[0], clock_a_).error().code, Errc::not_found);
    ASSERT_TRUE(c.retire_signer(key_b_.public_key()));
    std::uint64_t a_to_c = 0;
    for (const auto& o : sync(a, c, a_to_c, clock_a_)) EXPECT_EQ(o.kind, Kind::applied);
    expect_same(a, c, "kept");

    // Removal is permanent: pinned again, B still cannot sign.
    ASSERT_TRUE(a.add_device(key_b_.public_key(), "b"));
    EXPECT_EQ(a.apply_remote(late, clock_a_).error().code, Errc::invalid_argument);
}

TEST_F(MetadataIndexTest, AnAdoptedRecordMustBeOneTheRemovedDeviceSigned) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    auto genuine = b.commit_local(file("f", 1, 1), clock_b_).value();
    auto other = b.commit_local(file("g", 1, 2), clock_b_).value();
    ASSERT_TRUE(a.retire_signer(key_b_.public_key()));

    // B's own signature alone no longer counts.
    EXPECT_EQ(a.apply_remote(genuine, clock_a_).error().code, Errc::invalid_argument);
    // C makes up content in B's name at B's real version and countersigns it.
    auto made_up = genuine;
    made_up.chunks = {chunk_with(9)};
    made_up.signature = key_c_.sign(record_signed_bytes(drive_, made_up)).value();
    EXPECT_EQ(a.apply_remote(countersigned_by(key_c_, made_up), clock_a_).error().code,
              Errc::invalid_argument);
    // A countersignature over other bytes does not count either.
    auto moved = countersigned_by(key_c_, genuine);
    moved.countersignature = countersigned_by(key_c_, other).countersignature;
    EXPECT_EQ(a.apply_remote(moved, clock_a_).error().code, Errc::invalid_argument);
    // B's record countersigned by a pinned member is accepted, unchanged.
    EXPECT_EQ(a.apply_remote(countersigned_by(key_c_, genuine), clock_a_).value().kind,
              Kind::applied);
    EXPECT_EQ(a.get("f").value()->chunks, std::vector<ChunkId>{chunk_with(1)});
    // Not when the adopter is not pinned here; that may change, so it can be retried.
    ASSERT_TRUE(a.remove_device(key_c_.public_key()));
    EXPECT_EQ(a.apply_remote(countersigned_by(key_c_, other), clock_a_).error().code,
              Errc::not_found);
}

TEST_F(MetadataIndexTest, ADeviceThatIsNotPinnedCannotSign) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    auto record = b.commit_local(file("f", 1, 1), clock_b_).value();
    // B is out (say every device that vouched for it is out) but not removed for good.
    ASSERT_TRUE(a.remove_device(key_b_.public_key()));
    EXPECT_EQ(a.apply_remote(record, clock_a_).error().code, Errc::not_found);
    // A countersignature does not stand in for B being pinned.
    EXPECT_EQ(a.apply_remote(countersigned_by(key_c_, record), clock_a_).error().code,
              Errc::not_found);
    EXPECT_FALSE(a.get("f").value().has_value());
    // Pinned again, it signs again.
    ASSERT_TRUE(a.add_device(key_b_.public_key(), "b"));
    EXPECT_EQ(a.apply_remote(record, clock_a_).value().kind, Kind::applied);
}

TEST_F(MetadataIndexTest, RetiringAnAdopterAdoptsWhatItAdopted) {
    const DeviceKey key_d = seeded_key(4);
    MetadataIndex b = open_peer("b.db", key_b_);
    MetadataIndex c = open_peer("c.db", key_c_);
    auto record = b.commit_local(file("f", 1, 1), clock_b_).value();
    ASSERT_TRUE(c.apply_remote(record, clock_a_));
    // B is removed; C adopts its record and A gets C's adoption.
    ASSERT_EQ(c.retire_signer(key_b_.public_key()).value(), 1u);
    MetadataIndex a = open("a.db", key_a_);
    ASSERT_TRUE(a.add_device(key_c_.public_key(), "c"));
    ASSERT_TRUE(a.retire_signer(key_b_.public_key()));
    std::uint64_t c_to_a = 0;
    for (const auto& o : sync(c, a, c_to_a, clock_a_)) EXPECT_EQ(o.kind, Kind::applied);
    EXPECT_EQ(a.get("f").value()->countersigner, id_c_);

    // Then C is removed too: A countersigns the record in its place.
    ASSERT_TRUE(a.remove_device(key_c_.public_key()));
    EXPECT_EQ(a.retire_signer(key_c_.public_key()).value(), 1u);
    EXPECT_EQ(a.get("f").value()->countersigner, id_a_);

    // D joins later, knowing both were removed, and still gets B's file from A.
    MetadataIndex d = open("d.db", key_d);
    ASSERT_TRUE(d.add_device(key_a_.public_key(), "a"));
    ASSERT_TRUE(d.retire_signer(key_b_.public_key()));
    ASSERT_TRUE(d.retire_signer(key_c_.public_key()));
    std::uint64_t a_to_d = 0;
    for (const auto& o : sync(a, d, a_to_d, clock_a_)) EXPECT_EQ(o.kind, Kind::applied);
    EXPECT_EQ(d.get("f").value()->chunks, std::vector<ChunkId>{chunk_with(1)});
}

TEST_F(MetadataIndexTest, AdoptionDoesNotBringBackADeletedFile) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    MetadataIndex c = open_peer("c.db", key_c_);
    // C writes P; A hears of it, then C deletes P and only B hears of that.
    auto written = c.commit_local(file("p", 1, 1), clock_a_).value();
    ASSERT_TRUE(a.apply_remote(written, clock_a_));
    ASSERT_TRUE(b.apply_remote(written, clock_b_));
    auto deleted = c.delete_local("p", clock_a_).value();
    ASSERT_TRUE(b.apply_remote(deleted, clock_b_));
    // C is removed; A and B each adopt what they hold.
    ASSERT_EQ(a.retire_signer(key_c_.public_key()).value(), 1u);
    ASSERT_EQ(b.retire_signer(key_c_.public_key()).value(), 1u);
    EXPECT_EQ(a.apply_remote(deleted, clock_a_).error().code, Errc::invalid_argument);
    std::uint64_t a_to_b = 0;
    std::uint64_t b_to_a = 0;
    for (const auto& o : sync(a, b, a_to_b, clock_b_)) EXPECT_EQ(o.kind, Kind::ignored);
    for (const auto& o : sync(b, a, b_to_a, clock_a_)) EXPECT_EQ(o.kind, Kind::applied);
    EXPECT_TRUE(a.get("p").value()->deleted);
    EXPECT_TRUE(b.get("p").value()->deleted);
    expect_same(a, b, "p");
}

TEST_F(MetadataIndexTest, AdoptionByEveryMemberConvergesWithoutAConflictCopy) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    MetadataIndex c = open_peer("c.db", key_c_);
    ASSERT_TRUE(c.commit_local(file("doc.txt", 1, 1), clock_a_));
    std::uint64_t c_to_a = 0;
    std::uint64_t c_to_b = 0;
    sync(c, a, c_to_a, clock_a_);
    sync(c, b, c_to_b, clock_b_);

    // A and B both retire C and adopt its record at once.
    ASSERT_EQ(a.retire_signer(key_c_.public_key()).value(), 1u);
    pt_ += 10;
    ASSERT_EQ(b.retire_signer(key_c_.public_key()).value(), 1u);
    std::uint64_t a_to_b = a.max_sequence().value() - 1;
    std::uint64_t b_to_a = b.max_sequence().value() - 1;
    for (int round = 0; round < 2; ++round) {
        for (const auto& o : sync(a, b, a_to_b, clock_b_)) EXPECT_EQ(o.kind, Kind::ignored);
        for (const auto& o : sync(b, a, b_to_a, clock_a_)) EXPECT_EQ(o.kind, Kind::ignored);
    }
    expect_same(a, b, "doc.txt");
    EXPECT_EQ(a.get("doc.txt").value()->version, c.get("doc.txt").value()->version);
    const auto all = a.records_since(0, 100).value();
    EXPECT_EQ(all.size(), 1u);
    for (const auto& r : all) {
        EXPECT_EQ(r.path.find(".sync-conflict-"), std::string::npos) << r.path;
    }
}

TEST_F(MetadataIndexTest, AnEditOfAnAdoptedRecordSupersedesIt) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    MetadataIndex c = open_peer("c.db", key_c_);
    ASSERT_TRUE(c.commit_local(file("doc.txt", 1, 1), clock_a_));
    std::uint64_t c_to_a = 0;
    std::uint64_t c_to_b = 0;
    sync(c, a, c_to_a, clock_a_);
    sync(c, b, c_to_b, clock_b_);
    // B edits C's file; A has not seen the edit when it adopts C's version.
    pt_ += 5000;
    auto edit = b.commit_local(file("doc.txt", 2, 2), clock_b_).value();
    ASSERT_EQ(a.retire_signer(key_c_.public_key()).value(), 1u);
    EXPECT_EQ(a.apply_remote(edit, clock_a_).value().kind, Kind::applied);
    EXPECT_EQ(a.get("doc.txt").value()->chunks, std::vector<ChunkId>{chunk_with(2)});
}

TEST_F(MetadataIndexTest, ConcurrentResolutionRelaysTheSignedVersion) {
    MetadataIndex a = open_peer("a.db", key_a_);
    MetadataIndex b = open_peer("b.db", key_b_);
    MetadataIndex c = open_peer("c.db", key_c_);
    std::uint64_t a_to_b = 0;
    ASSERT_TRUE(a.commit_local(file("doc.txt", 1, 1), clock_a_));
    sync(a, b, a_to_b, clock_b_);

    auto from_a = a.commit_local(file("doc.txt", 100, 2), clock_a_).value();
    pt_ += 5000;
    auto from_b = b.commit_local(file("doc.txt", 105, 3), clock_b_).value();
    auto at_a = a.apply_remote(from_b, clock_a_);
    ASSERT_TRUE(at_a);
    ASSERT_EQ(at_a.value().kind, Kind::conflict);

    // A stores B's winning record as B signed it; only the local effective version merges.
    auto stored = a.get("doc.txt").value().value();
    EXPECT_EQ(stored.version, from_b.version);
    EXPECT_EQ(stored.signature, from_b.signature);
    EXPECT_EQ(stored.effective_version, from_a.version.merged(from_b.version));
    // A's own version is now ignored, and a later edit here supersedes both.
    EXPECT_EQ(a.apply_remote(from_a, clock_a_).value().kind, Kind::ignored);

    // What A relays verifies on a third device, the conflict copy included.
    auto copy = a.get(at_a.value().conflict_path).value().value();
    EXPECT_EQ(copy.origin_path, "doc.txt");
    EXPECT_EQ(copy.modified_by, id_a_);
    std::uint64_t a_to_c = 0;
    for (const auto& o : sync(a, c, a_to_c, clock_a_)) EXPECT_NE(o.kind, Kind::conflict);
    expect_same(a, c, "doc.txt");
    expect_same(a, c, copy.path);
    EXPECT_EQ(c.get(copy.path).value()->chunks, std::vector<ChunkId>{chunk_with(2)});

    // A copy is valid only at the path derived from its origin.
    auto moved = copy;
    moved.path = "elsewhere.txt";
    EXPECT_EQ(c.apply_remote(moved, clock_a_).error().code, Errc::invalid_argument);

    auto edited = a.commit_local(file("doc.txt", 200, 4), clock_a_).value();
    EXPECT_EQ(edited.version.compare(stored.effective_version), VersionVector::Order::after);
    EXPECT_EQ(b.apply_remote(edited, clock_b_).value().kind, Kind::applied);
}

// The schema before signing, as of commit 15c3cc3.
constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL) WITHOUT ROWID;
CREATE TABLE files (
    path TEXT PRIMARY KEY, size INTEGER NOT NULL, mode INTEGER NOT NULL,
    mtime_ns INTEGER NOT NULL, hlc INTEGER NOT NULL, version BLOB NOT NULL,
    modified_by INTEGER NOT NULL, block_size INTEGER NOT NULL, chunking INTEGER NOT NULL,
    deleted INTEGER NOT NULL, sequence INTEGER NOT NULL UNIQUE
) WITHOUT ROWID;
CREATE TABLE chunks (
    path TEXT NOT NULL REFERENCES files(path) ON DELETE CASCADE, idx INTEGER NOT NULL,
    chunk BLOB NOT NULL, PRIMARY KEY (path, idx)
) WITHOUT ROWID;
CREATE INDEX chunks_by_id ON chunks(chunk);
CREATE TABLE devices (
    key BLOB PRIMARY KEY, short_id INTEGER NOT NULL UNIQUE, name TEXT NOT NULL,
    peer_index_id INTEGER NOT NULL DEFAULT 0, peer_max_sequence INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE TABLE holdings (
    chunk BLOB NOT NULL, device BLOB NOT NULL REFERENCES devices(key) ON DELETE CASCADE,
    PRIMARY KEY (chunk, device)
) WITHOUT ROWID;
PRAGMA user_version = 1;
)sql";

class V1Database {
public:
    explicit V1Database(const std::filesystem::path& path) {
        sqlite3* raw = nullptr;
        EXPECT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        db_.reset(raw);
        exec(kSchemaV1);
    }

    void exec(const std::string& sql) {
        EXPECT_EQ(sqlite3_exec(db_.get(), sql.c_str(), nullptr, nullptr, nullptr), SQLITE_OK)
            << sqlite3_errmsg(db_.get());
    }

    void insert(const FileRecord& r) {
        const Bytes version = r.version.serialize();
        run("INSERT INTO files VALUES(?, ?, ?, ?, ?, ?, ?, ?, 0, ?, ?)", [&](sqlite3_stmt* s) {
            sqlite3_bind_text(s, 1, r.path.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(r.size));
            sqlite3_bind_int64(s, 3, r.mode);
            sqlite3_bind_int64(s, 4, r.mtime_ns);
            sqlite3_bind_int64(s, 5, static_cast<sqlite3_int64>(r.hlc));
            sqlite3_bind_blob(s, 6, version.data(), static_cast<int>(version.size()),
                              SQLITE_TRANSIENT);
            sqlite3_bind_int64(s, 7, static_cast<sqlite3_int64>(r.modified_by));
            sqlite3_bind_int64(s, 8, r.block_size);
            sqlite3_bind_int64(s, 9, r.deleted ? 1 : 0);
            sqlite3_bind_int64(s, 10, static_cast<sqlite3_int64>(r.sequence));
        });
        for (std::size_t i = 0; i < r.chunks.size(); ++i) {
            const auto id = r.chunks[i].to_bytes();
            run("INSERT INTO chunks VALUES(?, ?, ?)", [&](sqlite3_stmt* s) {
                sqlite3_bind_text(s, 1, r.path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(i));
                sqlite3_bind_blob(s, 3, id.data(), static_cast<int>(id.size()), SQLITE_TRANSIENT);
            });
        }
    }

    void insert_device(const PublicKey& key) {
        run("INSERT INTO devices(key, short_id, name, peer_index_id, peer_max_sequence) "
            "VALUES(?, ?, 'peer', 99, 7)",
            [&](sqlite3_stmt* s) {
                sqlite3_bind_blob(s, 1, key.data(), static_cast<int>(key.size()), SQLITE_TRANSIENT);
                sqlite3_bind_int64(s, 2, static_cast<sqlite3_int64>(short_id(key)));
            });
    }

private:
    template <typename Bind>
    void run(const char* sql, Bind bind) {
        sqlite3_stmt* raw = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db_.get(), sql, -1, &raw, nullptr), SQLITE_OK)
            << sqlite3_errmsg(db_.get());
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, &sqlite3_finalize);
        bind(raw);
        EXPECT_EQ(sqlite3_step(raw), SQLITE_DONE) << sqlite3_errmsg(db_.get());
    }

    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, &sqlite3_close};
};

TEST_F(MetadataIndexTest, AVersionOneIndexMigrates) {
    // B's own index holds the same record for theirs.txt, signed.
    MetadataIndex b = open_peer("b.db", key_b_);
    auto theirs = b.commit_local(file("theirs.txt", 2, 2), clock_b_).value();

    const std::uint64_t removed = 0xDDDD000000000000ull;
    {
        V1Database v1(dir_.path() / "a.db");
        auto mine = file("mine.txt", 1, 1);
        mine.version.bump(id_a_);
        mine.modified_by = id_a_;
        mine.hlc = 5;
        mine.sequence = 1;
        v1.insert(mine);
        auto stored_theirs = theirs;
        stored_theirs.sequence = 2;
        v1.insert(stored_theirs);
        auto gone = file("gone.txt", 3, 3);
        gone.version.bump(removed);  // a device removed before signing
        gone.modified_by = removed;
        gone.hlc = 6;
        gone.sequence = 3;
        v1.insert(gone);
        v1.insert_device(key_b_.public_key());
        v1.exec("INSERT INTO meta VALUES('index_id', 1234), ('sequence', 3), ('local_device', " +
                std::to_string(static_cast<std::int64_t>(id_a_)) + ")");
    }

    MetadataIndex a = open("a.db", key_a_);
    EXPECT_NE(a.index_id(), 1234u);
    // Every file is still there and readable.
    auto all = a.records_since(0, 100).value();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(a.get("theirs.txt").value()->chunks, std::vector<ChunkId>{chunk_with(2)});
    EXPECT_TRUE(a.is_pinned(key_b_.public_key()).value());

    // A's own record is signed as it was; B takes it without a version change.
    auto mine = a.get("mine.txt").value().value();
    ASSERT_TRUE(mine.signature);
    EXPECT_EQ(mine.version.get(id_a_), 1u);
    EXPECT_EQ(b.apply_remote(mine, clock_b_).value().kind, Kind::applied);
    // Nothing vouches for the removed device's record: it stays readable here, unsigned.
    auto gone = a.get("gone.txt").value().value();
    EXPECT_EQ(gone.modified_by, removed);
    EXPECT_FALSE(gone.signature);

    // B's record stays unsigned and is not sent on until B's signature arrives.
    auto unsigned_theirs = a.get("theirs.txt").value().value();
    EXPECT_FALSE(unsigned_theirs.signature);
    auto sendable = a.records_since(0, 100, true).value();
    EXPECT_EQ(sendable.size(), 1u);
    auto forged = theirs;
    forged.signature = key_c_.sign(record_signed_bytes(drive_, theirs)).value();
    EXPECT_EQ(a.apply_remote(forged, clock_a_).error().code, Errc::invalid_argument);
    EXPECT_EQ(a.apply_remote(theirs, clock_a_).value().kind, Kind::ignored);
    EXPECT_EQ(a.get("theirs.txt").value()->signature, theirs.signature);
    EXPECT_EQ(a.records_since(0, 100, true).value().size(), 2u);
    EXPECT_EQ(a.apply_remote(theirs, clock_a_).value().kind, Kind::ignored);

    // Reopening does not migrate again.
    const auto index_id = a.index_id();
    a = open("a.db", key_a_);
    EXPECT_EQ(a.index_id(), index_id);
    EXPECT_EQ(a.records_since(0, 100, true).value().size(), 2u);
}

// A version 1 index of device A holding `record` (unsigned) and B pinned.
void make_v1_index(const std::filesystem::path& path, std::uint64_t local, const PublicKey& b,
                   FileRecord record) {
    V1Database v1(path);
    record.sequence = 1;
    v1.insert(record);
    v1.insert_device(b);
    v1.exec("INSERT INTO meta VALUES('index_id', 1), ('sequence', 1), ('local_device', " +
            std::to_string(static_cast<std::int64_t>(local)) + ")");
}

TEST_F(MetadataIndexTest, UnsignedRowsAreNotAdopted) {
    MetadataIndex b = open_peer("b.db", key_b_);
    auto theirs = b.commit_local(file("theirs.txt", 2, 2), clock_b_).value();
    make_v1_index(dir_.path() / "a.db", id_a_, key_b_.public_key(), theirs);
    MetadataIndex a = open("a.db", key_a_);
    // B is removed before its signature reached A: the row was never verified.
    EXPECT_EQ(a.retire_signer(key_b_.public_key()).value(), 0u);
    EXPECT_FALSE(a.get("theirs.txt").value()->signature);
    EXPECT_TRUE(a.records_since(0, 100, true).value().empty());
}

TEST_F(MetadataIndexTest, AnUnsignedRowIsReplacedOnlyByTheSameRecord) {
    MetadataIndex b = open_peer("b.db", key_b_);
    auto theirs = b.commit_local(file("theirs.txt", 2, 2), clock_b_).value();
    // The stored row claims B's version with other content (planted by some member).
    auto planted = theirs;
    planted.chunks = {chunk_with(7)};
    make_v1_index(dir_.path() / "a.db", id_a_, key_b_.public_key(), planted);
    MetadataIndex a = open("a.db", key_a_);
    auto outcome = a.apply_remote(theirs, clock_a_);
    ASSERT_TRUE(outcome);
    // Treated as concurrent: both contents stay, and only the signed one is sent on.
    EXPECT_EQ(outcome.value().kind, Kind::conflict);
    const auto sendable = a.records_since(0, 100, true).value();
    ASSERT_EQ(sendable.size(), 1u);
    EXPECT_EQ(sendable[0].chunks, std::vector<ChunkId>{chunk_with(2)});
    EXPECT_EQ(a.records_since(0, 100).value().size(), 2u);
}

}  // namespace
}  // namespace ghostdrive::core
