// Real mounts through WinFsp. The filesystem's executor runs on a background thread while the
// test thread uses ordinary Win32 calls on the mountpoint.

// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include "ghostdrive/vfs/winfsp_mount.hpp"

#include <gtest/gtest.h>

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <set>
#include <thread>

#include "ghostdrive/core/chunking.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::vfs {
namespace {

core::Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    core::Bytes data(size);
    for (auto& b : data) b = static_cast<std::byte>(rng());
    return data;
}

core::Bytes read_whole(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    core::Bytes out(chars.size());
    std::memcpy(out.data(), chars.data(), chars.size());
    return out;
}

void write_whole(const std::filesystem::path& path, const core::Bytes& data) {
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
}

std::set<std::wstring> names_in(const std::filesystem::path& dir) {
    std::set<std::wstring> names;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        names.insert(e.path().filename().wstring());
    }
    return names;
}

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};

class WinFsp : public ::testing::Test {
protected:
    WinFsp()
        : secret_(core::generate_drive_secret().value()),
          store_(core::ChunkStore::open(dir_.path() / "store", core::ChunkHasher(secret_)).value()),
          index_(
              core::MetadataIndex::open(dir_.path() / "index.db", core::DriveId{}, key_).value()),
          cache_(128 * 1024 * 1024) {
        mountpoint_ = dir_.path() / "mnt";  // WinFsp creates it
    }

    ~WinFsp() override {
        mount_.reset();  // unmount before the executor and the filesystem go away
        work_.reset();
        io_.stop();
        if (io_thread_.joinable()) io_thread_.join();
        disk_.join();
    }

    std::optional<core::FileRecord> indexed(const std::string& path) {
        auto r = index_.get(path);
        if (!r || !r.value() || r.value()->deleted) return std::nullopt;
        return *r.value();
    }

    void start(bool writable = false, std::filesystem::path mountpoint = {}) {
        if (!mountpoint.empty()) mountpoint_ = std::move(mountpoint);
        fetcher_ = std::make_unique<Fetcher>(io_.get_executor(), disk_.get_executor(), store_,
                                             index_, cache_, nullptr);
        std::optional<WriteSupport> writes;
        if (writable) {
            writes.emplace(WriteSupport{store_, index_, clock_, disk_.get_executor(),
                                        dir_.path() / "staging"});
        }
        fs_ = std::make_unique<Filesystem>(io_.get_executor(), tree_, *fetcher_,
                                           FilesystemOptions{}, std::move(writes));
        io_thread_ = std::thread([this] { io_.run(); });
        WinFspMountOptions options;
        options.capacity_from = dir_.path();
        auto mounted = WinFspMount::mount(*fs_, mountpoint_, options);
        ASSERT_TRUE(mounted) << mounted.error().message;
        mount_ = std::move(mounted).value();
    }

    core::FileRecord add(const std::string& path, const core::Bytes& data) {
        auto source = dir_.path() / ("src-" + std::to_string(counter_++));
        write_whole(source, data);
        auto imported = store_.import_file(source).value();
        core::FileRecord record;
        record.path = path;
        record.size = imported.size;
        record.mode = 0644;
        record.mtime_ns = 1'700'000'000'000'000'000;
        record.block_size = imported.block_size;
        record.chunks = imported.chunks;
        EXPECT_TRUE(index_.commit_local(record, clock_));
        EXPECT_TRUE(tree_.refresh(index_));
        return record;
    }

    test::TempDir dir_;
    asio::io_context io_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> work_{
        io_.get_executor()};
    asio::thread_pool disk_{2};
    std::thread io_thread_;
    core::DriveSecret secret_;
    core::ChunkStore store_;
    core::DeviceKey key_ = core::DeviceKey::generate().value();
    core::MetadataIndex index_;
    core::HybridClock clock_;
    core::ChunkCache cache_;
    Tree tree_;
    std::unique_ptr<Fetcher> fetcher_;
    std::unique_ptr<Filesystem> fs_;
    std::filesystem::path mountpoint_;
    std::unique_ptr<WinFspMount> mount_;
    int counter_ = 0;
};

TEST_F(WinFsp, ListsStatsAndReadsFiles) {
    auto text = random_bytes(5000, 1);
    auto clip = random_bytes(3 * core::kMinBlockSize + 12345, 2);
    add("notes.txt", text);
    add("media/clip.bin", clip);
    ASSERT_NO_FATAL_FAILURE(start());

    EXPECT_EQ(names_in(mountpoint_), (std::set<std::wstring>{L"media", L"notes.txt"}));
    EXPECT_TRUE(std::filesystem::is_directory(mountpoint_ / "media"));
    EXPECT_EQ(std::filesystem::file_size(mountpoint_ / "media" / "clip.bin"), clip.size());
    EXPECT_TRUE(read_whole(mountpoint_ / "notes.txt") == text);
    EXPECT_TRUE(read_whole(mountpoint_ / "media" / "clip.bin") == clip);
}

TEST_F(WinFsp, RandomAccessReadsReturnTheRightBytes) {
    auto data = random_bytes(6 * core::kMinBlockSize, 3);
    add("f.bin", data);
    ASSERT_NO_FATAL_FAILURE(start());

    Handle file{::CreateFileW((mountpoint_ / "f.bin").c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(file.h, INVALID_HANDLE_VALUE) << ::GetLastError();
    std::mt19937_64 rng(4);
    for (int i = 0; i < 50; ++i) {
        const std::size_t len = 1 + rng() % 70000;
        const std::size_t off = rng() % (data.size() - len);
        core::Bytes buf(len);
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(off);
        DWORD n = 0;
        ASSERT_TRUE(::ReadFile(file.h, buf.data(), static_cast<DWORD>(len), &n, &at))
            << ::GetLastError();
        ASSERT_EQ(n, len) << "offset " << off;
        ASSERT_TRUE(
            std::equal(buf.begin(), buf.end(), data.begin() + static_cast<std::ptrdiff_t>(off)))
            << "mismatch at offset " << off;
    }
}

TEST_F(WinFsp, IsReadOnlyAndReportsMissingFiles) {
    add("f", random_bytes(10, 5));
    ASSERT_NO_FATAL_FAILURE(start());
    Handle write{::CreateFileW((mountpoint_ / "f").c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    EXPECT_EQ(write.h, INVALID_HANDLE_VALUE);
    Handle missing{::CreateFileW((mountpoint_ / "missing").c_str(), GENERIC_READ, 0, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    EXPECT_EQ(missing.h, INVALID_HANDLE_VALUE);
    EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_FILE_NOT_FOUND));
    Handle created{::CreateFileW((mountpoint_ / "new").c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    EXPECT_EQ(created.h, INVALID_HANDLE_VALUE);
}

TEST_F(WinFsp, LooksUpNamesWithoutRegardToCase) {
    auto data = random_bytes(777, 6);
    add("Media/Clip.bin", data);
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_TRUE(read_whole(mountpoint_ / "MEDIA" / "clip.BIN") == data);
    // Listings keep the stored case.
    EXPECT_EQ(names_in(mountpoint_ / "media"), (std::set<std::wstring>{L"Clip.bin"}));
}

TEST_F(WinFsp, LeavesOutNamesWindowsCannotUse) {
    for (const char* name : {"ok.txt", "bad:name", "trailing.", "CON", "nul.txt", "back\\slash"}) {
        add(name, random_bytes(3, 7));
    }
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_EQ(names_in(mountpoint_), (std::set<std::wstring>{L"ok.txt"}));
}

TEST_F(WinFsp, WritesCommitWhenTheLastHandleCloses) {
    ASSERT_NO_FATAL_FAILURE(start(true));
    auto data = random_bytes(2 * core::kMinBlockSize + 5, 8);
    write_whole(mountpoint_ / "new.bin", data);
    auto record = indexed("new.bin");
    ASSERT_TRUE(record);
    EXPECT_EQ(record->size, data.size());
    EXPECT_TRUE(read_whole(mountpoint_ / "new.bin") == data);

    // Appending through a second handle rewrites only the tail.
    {
        std::ofstream out(mountpoint_ / "new.bin", std::ios::binary | std::ios::app);
        out << "tail";
    }
    record = indexed("new.bin");
    ASSERT_TRUE(record);
    EXPECT_EQ(record->size, data.size() + 4);
}

TEST_F(WinFsp, DirectoriesRenamesAndDeletes) {
    ASSERT_NO_FATAL_FAILURE(start(true));
    std::filesystem::create_directories(mountpoint_ / "docs" / "old");
    EXPECT_TRUE(std::filesystem::is_directory(mountpoint_ / "docs" / "old"));
    write_whole(mountpoint_ / "docs" / "a.txt", random_bytes(100, 9));
    ASSERT_TRUE(indexed("docs/a.txt"));

    std::filesystem::rename(mountpoint_ / "docs" / "a.txt", mountpoint_ / "docs" / "b.txt");
    EXPECT_FALSE(indexed("docs/a.txt"));
    EXPECT_TRUE(indexed("docs/b.txt"));

    // A rename that only changes case keeps the file.
    std::filesystem::rename(mountpoint_ / "docs" / "b.txt", mountpoint_ / "docs" / "B.txt");
    EXPECT_FALSE(indexed("docs/b.txt"));
    EXPECT_TRUE(indexed("docs/B.txt"));

    // Without replace, a target that differs only in case counts as existing.
    write_whole(mountpoint_ / "docs" / "c.txt", random_bytes(10, 10));
    EXPECT_FALSE(::MoveFileExW((mountpoint_ / "docs" / "c.txt").c_str(),
                               (mountpoint_ / "docs" / "b.TXT").c_str(), 0));
    EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_ALREADY_EXISTS));
    EXPECT_TRUE(::MoveFileExW((mountpoint_ / "docs" / "c.txt").c_str(),
                              (mountpoint_ / "docs" / "b.TXT").c_str(), MOVEFILE_REPLACE_EXISTING))
        << ::GetLastError();
    EXPECT_TRUE(indexed("docs/B.txt"));
    EXPECT_FALSE(indexed("docs/c.txt"));

    EXPECT_FALSE(::RemoveDirectoryW((mountpoint_ / "docs").c_str()));  // not empty
    EXPECT_TRUE(std::filesystem::remove(mountpoint_ / "docs" / "B.txt"));
    EXPECT_FALSE(indexed("docs/B.txt"));
    EXPECT_TRUE(std::filesystem::remove(mountpoint_ / "docs" / "old"));
    EXPECT_FALSE(std::filesystem::exists(mountpoint_ / "docs" / "old"));
}

TEST_F(WinFsp, DeleteOnCloseRemovesTheFile) {
    add("doomed.txt", random_bytes(10, 11));
    ASSERT_NO_FATAL_FAILURE(start(true));
    {
        Handle file{::CreateFileW((mountpoint_ / "doomed.txt").c_str(), DELETE,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_DELETE_ON_CLOSE, nullptr)};
        ASSERT_NE(file.h, INVALID_HANDLE_VALUE) << ::GetLastError();
        EXPECT_TRUE(indexed("doomed.txt"));
    }
    EXPECT_FALSE(indexed("doomed.txt"));
    EXPECT_FALSE(std::filesystem::exists(mountpoint_ / "doomed.txt"));
}

TEST_F(WinFsp, RefusesNamesWindowsDoesNotAllow) {
    ASSERT_NO_FATAL_FAILURE(start(true));
    // The \\?\ prefix passes the name through unchanged, as far as the filesystem.
    const std::wstring base =
        L"\\\\?\\" + std::filesystem::path(mountpoint_).make_preferred().wstring() + L"\\";
    for (const wchar_t* name : {L"a<b", L"trailing.", L"CON"}) {
        Handle file{::CreateFileW((base + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr)};
        EXPECT_EQ(file.h, INVALID_HANDLE_VALUE) << "a forbidden name was created";
    }
    EXPECT_TRUE(names_in(mountpoint_).empty());
}

TEST_F(WinFsp, OnlyTheCurrentUserHasAccess) {
    add("f.txt", random_bytes(10, 12));
    ASSERT_NO_FATAL_FAILURE(start());
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    ASSERT_EQ(::GetNamedSecurityInfoW((mountpoint_ / "f.txt").c_str(), SE_FILE_OBJECT,
                                      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                      &owner, nullptr, &dacl, nullptr, &sd),
              static_cast<DWORD>(ERROR_SUCCESS));
    HANDLE token = nullptr;
    ASSERT_TRUE(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> user(len);
    ASSERT_TRUE(::GetTokenInformation(token, TokenUser, user.data(), len, &len));
    ::CloseHandle(token);
    PSID me = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;

    EXPECT_TRUE(::EqualSid(owner, me));
    ASSERT_NE(dacl, nullptr);
    ACL_SIZE_INFORMATION info{};
    ASSERT_TRUE(::GetAclInformation(dacl, &info, sizeof(info), AclSizeInformation));
    ASSERT_EQ(info.AceCount, 1u);
    void* ace = nullptr;
    ASSERT_TRUE(::GetAce(dacl, 0, &ace));
    ASSERT_EQ(static_cast<ACE_HEADER*>(ace)->AceType, ACCESS_ALLOWED_ACE_TYPE);
    EXPECT_TRUE(
        ::EqualSid(reinterpret_cast<PSID>(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart), me));
    ::LocalFree(sd);
}

TEST_F(WinFsp, DirectoryMountpointComesAndGoes) {
    add("f.txt", random_bytes(10, 13));
    std::filesystem::create_directories(mountpoint_);  // an empty leftover is replaced
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_TRUE(std::filesystem::exists(mountpoint_ / "f.txt"));
    mount_->unmount();
    EXPECT_FALSE(std::filesystem::exists(mountpoint_));
}

TEST_F(WinFsp, MountsAtADriveLetter) {
    const DWORD used = ::GetLogicalDrives();
    wchar_t letter = 0;
    for (wchar_t c = L'Z'; c >= L'G'; --c) {
        if ((used & (1u << (c - L'A'))) == 0) {
            letter = c;
            break;
        }
    }
    if (letter == 0) GTEST_SKIP() << "no free drive letter";
    auto data = random_bytes(4321, 14);
    add("f.bin", data);
    ASSERT_NO_FATAL_FAILURE(start(false, std::wstring{letter, L':'}));
    const std::filesystem::path root = std::wstring{letter, L':', L'\\'};
    EXPECT_TRUE(read_whole(root / "f.bin") == data);
    wchar_t label[MAX_PATH + 1] = {};
    wchar_t fs_name[MAX_PATH + 1] = {};
    ASSERT_TRUE(::GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, nullptr, nullptr,
                                        nullptr, fs_name, MAX_PATH + 1))
        << ::GetLastError();
    EXPECT_STREQ(label, L"GhostDrive");
    EXPECT_STREQ(fs_name, L"GhostDrive");
}

}  // namespace
}  // namespace ghostdrive::vfs
