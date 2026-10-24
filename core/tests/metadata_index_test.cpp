#include "ghostdrive/core/metadata_index.hpp"

#include <gtest/gtest.h>

#include "ghostdrive/core/chunking.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::core {
namespace {

using Kind = ApplyOutcome::Kind;

constexpr std::uint64_t kDeviceA = 0xAAAA000000000000ull;
constexpr std::uint64_t kDeviceB = 0xBBBB000000000000ull;

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
    MetadataIndex open(std::string_view name, std::uint64_t device) {
        auto index = MetadataIndex::open(dir_.path() / name, device);
        EXPECT_TRUE(index.ok()) << (index.ok() ? "" : index.error().message);
        return std::move(index).value();
    }

    // Sends every record `to` has not yet seen from `from`, as an index exchange would.
    static std::vector<ApplyOutcome> sync(const MetadataIndex& from, MetadataIndex& to,
                                          std::uint64_t& cursor, HybridClock& clock) {
        std::vector<ApplyOutcome> outcomes;
        auto records = from.records_since(cursor, 1000);
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
    std::uint64_t pt_ = 1'000'000;
    HybridClock clock_a_{[this] { return pt_; }};
    HybridClock clock_b_{[this] { return pt_; }};
};

TEST_F(MetadataIndexTest, CommitLocalAssignsVersionAndSequence) {
    MetadataIndex index = open("a.db", kDeviceA);
    EXPECT_EQ(index.max_sequence().value(), 0u);

    auto first = index.commit_local(file("docs/a.txt", 10, 1), clock_a_);
    ASSERT_TRUE(first.ok()) << first.error().message;
    EXPECT_EQ(first.value().sequence, 1u);
    EXPECT_EQ(first.value().version.get(kDeviceA), 1u);
    EXPECT_EQ(first.value().modified_by, kDeviceA);

    auto second = index.commit_local(file("docs/a.txt", 11, 2), clock_a_);
    ASSERT_TRUE(second.ok());
    EXPECT_EQ(second.value().sequence, 2u);
    EXPECT_EQ(second.value().version.get(kDeviceA), 2u);
    EXPECT_GT(second.value().hlc, first.value().hlc);

    auto stored = index.get("docs/a.txt");
    ASSERT_TRUE(stored.ok() && stored.value().has_value());
    EXPECT_EQ(stored.value()->chunks, std::vector<ChunkId>{chunk_with(2)});
    EXPECT_EQ(stored.value()->mode, 0644u);
    EXPECT_FALSE(index.get("missing").value().has_value());
}

TEST_F(MetadataIndexTest, RejectsInvalidRecords) {
    MetadataIndex index = open("a.db", kDeviceA);
    EXPECT_EQ(index.commit_local(file("../escape", 1, 1), clock_a_).error().code,
              Errc::invalid_argument);
    FileRecord bad = file("ok", 1, 1);
    bad.chunks.clear();
    EXPECT_EQ(index.apply_remote(bad, clock_a_).error().code, Errc::invalid_argument);
}

TEST_F(MetadataIndexTest, DeleteLeavesTombstone) {
    MetadataIndex index = open("a.db", kDeviceA);
    ASSERT_TRUE(index.commit_local(file("f", 1, 1), clock_a_).ok());
    auto tomb = index.delete_local("f", clock_a_);
    ASSERT_TRUE(tomb.ok());
    EXPECT_TRUE(tomb.value().deleted);
    EXPECT_TRUE(tomb.value().chunks.empty());
    EXPECT_EQ(tomb.value().version.get(kDeviceA), 2u);
    EXPECT_EQ(index.delete_local("f", clock_a_).error().code, Errc::not_found);

    auto since = index.records_since(0, 10);
    ASSERT_TRUE(since.ok());
    ASSERT_EQ(since.value().size(), 1u);
    EXPECT_TRUE(since.value()[0].deleted);
}

TEST_F(MetadataIndexTest, ApplyRemoteNewerAndOlder) {
    MetadataIndex a = open("a.db", kDeviceA);
    MetadataIndex b = open("b.db", kDeviceB);
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
    MetadataIndex a = open("a.db", kDeviceA);
    MetadataIndex b = open("b.db", kDeviceB);
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
    MetadataIndex a = open("a.db", kDeviceA);
    MetadataIndex b = open("b.db", kDeviceB);
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
    MetadataIndex index = open("a.db", kDeviceA);
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
        MetadataIndex index = open("a.db", kDeviceA);
        index_id = index.index_id();
        ASSERT_TRUE(index.commit_local(file("f", 1, 1), clock_a_).ok());
    }
    MetadataIndex index = open("a.db", kDeviceA);
    EXPECT_EQ(index.index_id(), index_id);
    EXPECT_EQ(index.max_sequence().value(), 1u);
    EXPECT_TRUE(index.get("f").value().has_value());

    auto wrong = MetadataIndex::open(dir_.path() / "a.db", kDeviceB);
    ASSERT_FALSE(wrong.ok());
    EXPECT_EQ(wrong.error().code, Errc::invalid_argument);
}

TEST_F(MetadataIndexTest, PinnedDevices) {
    MetadataIndex index = open("a.db", kDeviceA);
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
    MetadataIndex index = open("a.db", kDeviceA);
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

}  // namespace
}  // namespace ghostdrive::core
