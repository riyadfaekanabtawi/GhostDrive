// Real mounts through WinFsp. The filesystem's executor runs on a background thread while the
// test thread uses ordinary Win32 calls on the mountpoint.

// clang-format off
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
// clang-format on

#include "ghostdrive/vfs/winfsp_mount.hpp"

#include <gtest/gtest.h>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
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
#include "ghostdrive/core/file.hpp"
#include "ghostdrive/core/hlc.hpp"
#include "ghostdrive/vfs/chunk_exchange.hpp"
#include "temp_dir.hpp"
#include "winfsp_names.hpp"

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

// Where a test's mountpoints go: a private directory in the user's temp folder. Mounting refuses
// parents that other users could change, and the build tree may lie below one (on CI runners,
// D:\ lets all users modify its folders). Only empty directories are created here; WinFsp
// removes each mountpoint on unmount.
class MountDir {
public:
    MountDir() {
        wchar_t temp[MAX_PATH + 1] = {};
        const DWORD n = ::GetTempPathW(MAX_PATH + 1, temp);
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::path(std::wstring(temp, n)) / "ghostdrive-tests" /
                (std::string(info->test_suite_name()) + "." + info->name());
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        EXPECT_TRUE(core::make_private_dir(path_));
    }
    MountDir(const MountDir&) = delete;
    MountDir& operator=(const MountDir&) = delete;
    ~MountDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

class WinFsp : public ::testing::Test {
protected:
    WinFsp()
        : secret_(core::generate_drive_secret().value()),
          store_(core::ChunkStore::open(dir_.path() / "store", core::ChunkHasher(secret_)).value()),
          index_(
              core::MetadataIndex::open(dir_.path() / "index.db", core::DriveId{}, key_).value()),
          cache_(128 * 1024 * 1024) {
        mountpoint_ = mounts_.path() / "mnt";  // WinFsp creates it
    }

    ~WinFsp() override {
        mount_.reset();  // unmount before the executor and the filesystem go away
        if (io_thread_.joinable()) {
            std::promise<void> closed;
            asio::post(io_, [&] {
                if (client_) client_->close();
                if (server_) server_->close();
                closed.set_value();
            });
            closed.get_future().wait();
        }
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

    // Starts the filesystem without mounting it. A fetch with no connected holder waits up to
    // 20 s for one, which the tests use to hold a read in flight.
    void prepare(bool writable = false, ConnectionLookup peers = nullptr) {
        fetcher_ = std::make_unique<Fetcher>(io_.get_executor(), disk_.get_executor(), store_,
                                             index_, cache_, std::move(peers),
                                             std::chrono::seconds(10), std::chrono::seconds(20));
        std::optional<WriteSupport> writes;
        if (writable) {
            writes.emplace(WriteSupport{store_, index_, clock_, disk_.get_executor(),
                                        dir_.path() / "staging"});
        }
        fs_ = std::make_unique<Filesystem>(io_.get_executor(), tree_, *fetcher_,
                                           FilesystemOptions{}, std::move(writes));
        io_thread_ = std::thread([this] { io_.run(); });
    }

    core::Result<std::unique_ptr<WinFspMount>> try_mount(const std::filesystem::path& at,
                                                         std::filesystem::path data_dir = {}) {
        WinFspMountOptions options;
        options.data_dir = data_dir.empty() ? dir_.path() : std::move(data_dir);
        return WinFspMount::mount(*fs_, at, options);
    }

    void start(bool writable = false, std::filesystem::path mountpoint = {},
               ConnectionLookup peers = nullptr) {
        if (!mountpoint.empty()) mountpoint_ = std::move(mountpoint);
        prepare(writable, std::move(peers));
        auto mounted = try_mount(mountpoint_);
        ASSERT_TRUE(mounted) << mounted.error().message;
        mount_ = std::move(mounted).value();
    }

    core::FileRecord add(core::ChunkStore& store, const std::string& path,
                         const core::Bytes& data) {
        auto source = dir_.path() / ("src-" + std::to_string(counter_++));
        write_whole(source, data);
        auto imported = store.import_file(source).value();
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
    core::FileRecord add(const std::string& path, const core::Bytes& data) {
        return add(store_, path, data);
    }

    // A file whose chunks only a second device holds, and a connection to that device that
    // publish() makes visible to the fetcher. Until then reads of the file wait in the fetcher.
    void add_remote(const std::string& path, const core::Bytes& data) {
        server_store_.emplace(
            core::ChunkStore::open(dir_.path() / "server-store", core::ChunkHasher(secret_))
                .value());
        auto record = add(*server_store_, path, data);
        ASSERT_TRUE(index_.add_device(server_key_.public_key(), "server"));
        for (const auto& id : record.chunks) {
            ASSERT_TRUE(index_.set_holds(server_key_.public_key(), id, true));
        }
    }
    ConnectionLookup peers() {
        return [this](const core::PublicKey&) { return conn_; };
    }
    void connect_peer() {
        transport::EndpointOptions server_options;
        server_options.accept_peer = [pin = client_key_.public_key()](const core::PublicKey& k) {
            return k == pin;
        };
        std::promise<core::Result<std::shared_ptr<transport::Connection>>> connected;
        asio::post(io_, [&] {
            server_ = transport::Endpoint::create(io_.get_executor(), server_key_, server_options)
                          .value();
            client_ = transport::Endpoint::create(io_.get_executor(), client_key_, {}).value();
            asio::co_spawn(
                io_,
                [&]() -> asio::awaitable<void> {
                    asio::co_spawn(
                        io_,
                        [this]() -> asio::awaitable<void> {
                            auto incoming = co_await server_->accept();
                            if (incoming) {
                                co_await serve_chunks(incoming.value(), *server_store_,
                                                      disk_.get_executor());
                            }
                        },
                        asio::detached);
                    connected.set_value(co_await client_->connect(server_->local_endpoint(),
                                                                  server_key_.public_key()));
                },
                asio::detached);
        });
        auto result = connected.get_future().get();
        ASSERT_TRUE(result) << result.error().message;
        pending_conn_ = result.value();
    }
    void publish() {
        std::promise<void> done;
        asio::post(io_, [&] {
            conn_ = pending_conn_;
            fetcher_->holders_changed();
            done.set_value();
        });
        done.get_future().wait();
    }

    test::TempDir dir_;
    MountDir mounts_;
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
    std::optional<core::ChunkStore> server_store_;
    core::DeviceKey server_key_ = core::DeviceKey::generate().value();
    core::DeviceKey client_key_ = core::DeviceKey::generate().value();
    std::shared_ptr<transport::Endpoint> client_;
    std::shared_ptr<transport::Endpoint> server_;
    std::shared_ptr<transport::Connection> pending_conn_;
    std::shared_ptr<transport::Connection> conn_;  // touched on the io thread only
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
    // Missing parents are created private to this user.
    ASSERT_NO_FATAL_FAILURE(start(false, mounts_.path() / "a" / "b" / "mnt"));
    EXPECT_TRUE(core::is_private_dir(mounts_.path() / "a"));
    EXPECT_TRUE(core::is_private_dir(mounts_.path() / "a" / "b"));
    EXPECT_TRUE(std::filesystem::exists(mountpoint_ / "f.txt"));
    mount_->unmount();
    EXPECT_FALSE(std::filesystem::exists(mountpoint_));
}

std::wstring my_sid() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD len = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    std::vector<BYTE> user(len);
    const bool got = len != 0 && ::GetTokenInformation(token, TokenUser, user.data(), len, &len);
    ::CloseHandle(token);
    LPWSTR text = nullptr;
    if (!got ||
        !::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &text)) {
        return {};
    }
    std::wstring out(text);
    ::LocalFree(text);
    return out;
}

// Gives `dir` a protected DACL: full access for this user plus `extra` ACEs in SDDL.
bool set_dacl(const std::filesystem::path& dir, const std::wstring& extra) {
    const std::wstring sddl = L"D:P(A;OICI;FA;;;" + my_sid() + L")" + extra;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd,
                                                                nullptr)) {
        return false;
    }
    const bool ok =
        ::SetFileSecurityW(dir.c_str(),
                           DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                           sd) != FALSE;
    ::LocalFree(sd);
    return ok;
}

// An existing parent that someone else could rename, replace or change makes the mount fail,
// and nothing is created below it. Only adding entries is allowed, as C:\ allows everyone.
TEST_F(WinFsp, RefusesParentsOthersCanChange) {
    ASSERT_NO_FATAL_FAILURE(prepare());
    const auto shared = mounts_.path() / "shared";
    std::filesystem::create_directories(shared / "mine");
    ASSERT_TRUE(core::make_private_dir(shared / "mine"));
    // Everyone (WD) may delete entries in it (FILE_DELETE_CHILD, 0x40) or rename it (DELETE).
    for (const wchar_t* ace : {L"(A;;0x40;;;WD)", L"(A;;0x10000;;;WD)", L"(A;;WD;;;WD)"}) {
        ASSERT_TRUE(set_dacl(shared, ace)) << ::GetLastError();
        // Directly above the mountpoint, above a missing parent, and above an existing private
        // parent.
        for (const auto& at : {shared / "mnt", shared / "new" / "mnt", shared / "mine" / "mnt"}) {
            auto mounted = try_mount(at);
            ASSERT_FALSE(mounted);
            EXPECT_EQ(mounted.error().code, core::Errc::permission) << mounted.error().message;
            EXPECT_NE(mounted.error().message.find("S-1-1-0"), std::string::npos)
                << mounted.error().message;
        }
        EXPECT_FALSE(std::filesystem::exists(shared / "new"));
    }
    // Deny entries, inherit-only entries and the right to add entries are not a problem.
    ASSERT_TRUE(set_dacl(shared,
                         L"(A;;0x1200a9;;;WD)(A;;0x4;;;WD)(A;OICIIO;FA;;;WD)"
                         L"(D;;FA;;;AN)"));
    auto mounted = try_mount(shared / "new" / "mnt");
    ASSERT_TRUE(mounted) << mounted.error().message;
    EXPECT_TRUE(core::is_private_dir(shared / "new"));
    mounted.value()->unmount();

    // A junction or directory link above the mountpoint is refused rather than followed.
    const auto target = mounts_.path() / "target";
    std::filesystem::create_directories(target);
    const auto link = mounts_.path() / "link";
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    if (!ec) {
        auto through_link = try_mount(link / "mnt");
        ASSERT_FALSE(through_link);
        EXPECT_EQ(through_link.error().code, core::Errc::permission);
        EXPECT_FALSE(std::filesystem::exists(target / "mnt"));
    }
}

TEST(WinFspNames, AFailedFoldMatchesNothing) {
    EXPECT_EQ(detail::fold_name(L"Report.txt", LOCALE_NAME_INVARIANT), std::wstring(L"REPORT.TXT"));
    EXPECT_EQ(detail::fold_name(L"", LOCALE_NAME_INVARIANT), std::wstring());
    // A malformed locale name makes LCMapStringEx fail; the result is no key at all, not the raw
    // name, which would not compare the way the kernel's uppercasing does.
    EXPECT_FALSE(detail::fold_name(L"report.txt", L"not a locale!").has_value());
}

TEST_F(WinFsp, LeavesWhatIsAlreadyAtTheMountpointAlone) {
    ASSERT_NO_FATAL_FAILURE(prepare());
    // An existing directory, even an empty one, is the user's.
    std::filesystem::create_directories(mountpoint_);
    auto mounted = try_mount(mountpoint_);
    ASSERT_FALSE(mounted);
    EXPECT_EQ(mounted.error().code, core::Errc::already_exists);
    EXPECT_TRUE(std::filesystem::is_directory(mountpoint_));

    // So is a junction or directory link, which is not followed or removed.
    const auto target = mounts_.path() / "target";
    std::filesystem::create_directories(target);
    const auto link = mounts_.path() / "link";
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    if (!ec) {
        auto over_link = try_mount(link);
        ASSERT_FALSE(over_link);
        EXPECT_TRUE(std::filesystem::is_symlink(link));
    }

    // A data directory inside the mount is refused.
    auto around_data = try_mount(mounts_.path() / "outer", mounts_.path() / "outer" / "data");
    ASSERT_FALSE(around_data);
    EXPECT_EQ(around_data.error().code, core::Errc::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(mounts_.path() / "outer"));
}

// WinFsp keeps one kernel file node per name compared without case, so of names that differ
// only in case, one is shown and every spelling opens it.
TEST_F(WinFsp, ShowsOneOfNamesThatDifferOnlyInCase) {
    auto upper = random_bytes(100, 15);
    auto lower = random_bytes(200, 16);
    add("Report.txt", upper);
    add("report.txt", lower);
    ASSERT_NO_FATAL_FAILURE(start());
    EXPECT_EQ(names_in(mountpoint_), (std::set<std::wstring>{L"Report.txt"}));
    EXPECT_TRUE(read_whole(mountpoint_ / "Report.txt") == upper);
    EXPECT_TRUE(read_whole(mountpoint_ / "report.txt") == upper);
    EXPECT_TRUE(read_whole(mountpoint_ / "REPORT.TXT") == upper);
}

// A read waiting on a peer is cancelled by its reader: WinFsp keeps the request (and its
// buffer) until it is answered, and the mount carries on.
TEST_F(WinFsp, ACancelledSlowReadLeavesTheMountHealthy) {
    auto remote = random_bytes(3 * core::kMinBlockSize + 7, 17);
    auto local = random_bytes(5000, 18);
    ASSERT_NO_FATAL_FAILURE(add_remote("remote.bin", remote));
    add("local.bin", local);
    ASSERT_NO_FATAL_FAILURE(start(false, {}, peers()));
    ASSERT_NO_FATAL_FAILURE(connect_peer());

    Handle file{::CreateFileW((mountpoint_ / "remote.bin").c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    ASSERT_NE(file.h, INVALID_HANDLE_VALUE) << ::GetLastError();
    core::Bytes buf(65536);
    OVERLAPPED at{};
    at.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_FALSE(::ReadFile(file.h, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &at));
    ASSERT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_IO_PENDING));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // the fetch is waiting
    ::CancelIoEx(file.h, &at);

    // Other files read correctly while that read is outstanding.
    EXPECT_TRUE(read_whole(mountpoint_ / "local.bin") == local);

    publish();
    DWORD n = 0;
    if (::GetOverlappedResult(file.h, &at, &n, TRUE)) {
        EXPECT_EQ(n, buf.size());
        EXPECT_TRUE(std::equal(buf.begin(), buf.end(), remote.begin()));
    } else {
        EXPECT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_OPERATION_ABORTED));
    }
    ::CloseHandle(at.hEvent);
    EXPECT_TRUE(read_whole(mountpoint_ / "remote.bin") == remote);
    EXPECT_TRUE(read_whole(mountpoint_ / "local.bin") == local);
}

// Unmounting cancels a read still waiting on a peer and unmaps its buffer; when the bytes
// arrive afterwards they must not be copied anywhere.
TEST_F(WinFsp, UnmountingWithASlowReadInFlightIsSafe) {
    auto remote = random_bytes(3 * core::kMinBlockSize + 7, 19);
    ASSERT_NO_FATAL_FAILURE(add_remote("remote.bin", remote));
    ASSERT_NO_FATAL_FAILURE(start(false, {}, peers()));
    ASSERT_NO_FATAL_FAILURE(connect_peer());

    Handle file{::CreateFileW((mountpoint_ / "remote.bin").c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    ASSERT_NE(file.h, INVALID_HANDLE_VALUE) << ::GetLastError();
    core::Bytes buf(65536);
    OVERLAPPED at{};
    at.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_FALSE(::ReadFile(file.h, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &at));
    ASSERT_EQ(::GetLastError(), static_cast<DWORD>(ERROR_IO_PENDING));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::thread unmounting([this] { mount_->unmount(); });  // waits for the fetch to end
    DWORD n = 0;
    EXPECT_FALSE(::GetOverlappedResult(file.h, &at, &n, TRUE));  // cancelled by the unmount
    publish();  // the bytes arrive after the request is gone
    unmounting.join();
    ::CloseHandle(at.hEvent);

    // The process is intact and a new mount reads the file.
    mount_.reset();
    auto again = try_mount(mounts_.path() / "mnt2");
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(read_whole(mounts_.path() / "mnt2" / "remote.bin") == remote);
}

TEST_F(WinFsp, WritesOnHandlesStillOpenAtUnmountAreCommitted) {
    ASSERT_NO_FATAL_FAILURE(start(true));
    Handle file{::CreateFileW((mountpoint_ / "open.txt").c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    ASSERT_NE(file.h, INVALID_HANDLE_VALUE) << ::GetLastError();
    const char text[] = "still open";
    DWORD n = 0;
    ASSERT_TRUE(::WriteFile(file.h, text, sizeof(text) - 1, &n, nullptr));
    mount_->unmount();
    auto record = indexed("open.txt");
    ASSERT_TRUE(record);
    EXPECT_EQ(record->size, sizeof(text) - 1);
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
