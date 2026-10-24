#include "ghostdrive/vfs/tree.hpp"

#include <gtest/gtest.h>

#include "ghostdrive/core/chunking.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::vfs {
namespace {

core::FileRecord file(std::string path, std::uint64_t size = 10, std::uint64_t sequence = 0) {
    core::FileRecord r;
    r.path = std::move(path);
    r.size = size;
    r.mode = 0640;
    r.mtime_ns = 1'000;
    r.block_size = core::kMinBlockSize;
    r.chunks.resize(core::chunk_count(size, r.block_size));
    r.sequence = sequence;
    return r;
}

core::FileRecord tombstone(std::string path) {
    auto r = file(std::move(path));
    r.deleted = true;
    r.chunks.clear();
    return r;
}

std::vector<std::string> names(const Tree& tree, Ino dir) {
    std::vector<std::string> out;
    auto entries = tree.readdir(dir);  // keep the optional alive across the loop
    for (const auto& e : entries.value()) out.push_back(e.name);
    return out;
}

TEST(Tree, RootIsAnEmptyDirectory) {
    Tree tree;
    auto attr = tree.getattr(kRootIno);
    ASSERT_TRUE(attr);
    EXPECT_EQ(attr->type, NodeType::directory);
    EXPECT_TRUE(tree.readdir(kRootIno)->empty());
}

TEST(Tree, FilesImplyTheirDirectories) {
    Tree tree;
    tree.apply(file("a/b/c.txt", 42));
    tree.apply(file("a/d.txt"));

    auto a = tree.lookup(kRootIno, "a");
    ASSERT_TRUE(a);
    EXPECT_EQ(tree.getattr(*a)->type, NodeType::directory);
    EXPECT_EQ(names(tree, *a), (std::vector<std::string>{"b", "d.txt"}));
    EXPECT_EQ(tree.getattr(*a)->nlink, 3u);  // ".", entry in root, and b's ".."

    auto c = tree.resolve("a/b/c.txt");
    ASSERT_TRUE(c);
    auto attr = tree.getattr(*c);
    EXPECT_EQ(attr->type, NodeType::file);
    EXPECT_EQ(attr->size, 42u);
    EXPECT_EQ(attr->perm, 0640u);
    EXPECT_FALSE(tree.lookup(*c, "x"));  // files have no children
}

TEST(Tree, UpdateKeepsTheInode) {
    Tree tree;
    tree.apply(file("f", 10));
    auto before = tree.resolve("f");
    tree.apply(file("f", 20));
    EXPECT_EQ(tree.resolve("f"), before);
    EXPECT_EQ(tree.getattr(*before)->size, 20u);
}

TEST(Tree, DeleteRemovesFileAndEmptyDirectories) {
    Tree tree;
    tree.apply(file("a/b/c"));
    tree.apply(file("a/keep"));
    tree.apply(tombstone("a/b/c"));
    EXPECT_FALSE(tree.resolve("a/b"));
    EXPECT_TRUE(tree.resolve("a/keep"));
    tree.apply(tombstone("a/keep"));
    EXPECT_FALSE(tree.resolve("a"));
    EXPECT_TRUE(tree.readdir(kRootIno)->empty());
}

TEST(Tree, InodesAreNeverReused) {
    Tree tree;
    tree.apply(file("x"));
    auto first = *tree.resolve("x");
    tree.apply(tombstone("x"));
    EXPECT_FALSE(tree.getattr(first));
    tree.apply(file("x"));
    EXPECT_NE(*tree.resolve("x"), first);
}

TEST(Tree, CollisionsBetweenFilesAndDirectoriesAreIgnored) {
    Tree tree;
    tree.apply(file("a/b"));
    tree.apply(file("a"));      // "a" is a directory
    tree.apply(file("a/b/c"));  // "a/b" is a file
    EXPECT_EQ(tree.getattr(*tree.resolve("a"))->type, NodeType::directory);
    EXPECT_EQ(tree.getattr(*tree.resolve("a/b"))->type, NodeType::file);
    EXPECT_EQ(names(tree, *tree.resolve("a")), std::vector<std::string>{"b"});
}

TEST(Tree, InvalidPathsAreIgnored) {
    Tree tree;
    tree.apply(file("../escape"));
    tree.apply(file("/abs"));
    tree.apply(file("a//b"));
    EXPECT_TRUE(tree.readdir(kRootIno)->empty());
}

TEST(Tree, RefreshAppliesOnlyNewIndexRecords) {
    test::TempDir dir;
    auto index = core::MetadataIndex::open(dir.path() / "index.db", 0xA1);
    ASSERT_TRUE(index);
    core::HybridClock clock;
    ASSERT_TRUE(index.value().commit_local(file("one"), clock));
    ASSERT_TRUE(index.value().commit_local(file("dir/two"), clock));

    Tree tree;
    auto applied = tree.refresh(index.value());
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value(), 2u);
    EXPECT_TRUE(tree.resolve("dir/two"));

    applied = tree.refresh(index.value());
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value(), 0u);

    ASSERT_TRUE(index.value().delete_local("one", clock));
    applied = tree.refresh(index.value());
    ASSERT_TRUE(applied);
    EXPECT_EQ(applied.value(), 1u);
    EXPECT_FALSE(tree.resolve("one"));
    EXPECT_EQ(tree.applied_sequence(), index.value().max_sequence().value());
}

}  // namespace
}  // namespace ghostdrive::vfs
