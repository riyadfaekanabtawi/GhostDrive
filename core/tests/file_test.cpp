#include "ghostdrive/core/file.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "permissions.hpp"
#include "temp_dir.hpp"

namespace ghostdrive::core {
namespace {

// Characters no single ANSI code page holds, as in a non-ASCII Windows profile path. On
// Windows, path::string() throws for these.
const std::filesystem::path kUnicodeName(u8"d\u00e9j\u00e0-\u65e5\u672c");
const std::string kUnicodeUtf8 = "d\xc3\xa9j\xc3\xa0-\xe6\x97\xa5\xe6\x9c\xac";

Bytes bytes_of(std::string_view s) {
    auto view = as_bytes(s);
    return Bytes(view.begin(), view.end());
}

TEST(File, PathsConvertToUtf8) {
    const auto path = std::filesystem::path("a") / kUnicodeName;
    const std::string expected = std::string("a") +
                                 static_cast<char>(std::filesystem::path::preferred_separator) +
                                 kUnicodeUtf8;
    EXPECT_EQ(path_utf8(path), expected);
    auto exact = exact_utf8(path);
    ASSERT_TRUE(exact) << exact.error().message;
    EXPECT_EQ(exact.value(), expected);
}

TEST(File, PrivateFilesUnderANonAsciiDirectory) {
    test::TempDir tmp;
    const auto dir = tmp.path() / kUnicodeName;
    ASSERT_TRUE(make_private_dir(dir));
    EXPECT_TRUE(is_private_dir(dir));
    EXPECT_TRUE(test::has_private_mode(dir, 0700));

    const auto secret = dir / kUnicodeName;
    ASSERT_TRUE(write_private_file(secret, bytes_of("secret")));
    auto read = read_private_file(secret);
    ASSERT_TRUE(read) << read.error().message;
    EXPECT_EQ(read.value(), bytes_of("secret"));

    // Errors name the path in UTF-8 instead of failing to convert it.
    auto missing = read_private_file(dir / "missing");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, Errc::not_found);
    EXPECT_NE(missing.error().message.find(kUnicodeUtf8), std::string::npos)
        << missing.error().message;

    auto scratch = File::open_scratch(dir / "scratch");
    ASSERT_TRUE(scratch) << scratch.error().message;
    EXPECT_TRUE(scratch.value().write_at(bytes_of("x"), 0));
}

TEST(File, ScratchReplacesAnExistingFileThatOthersCanRead) {
    test::TempDir tmp;
    const auto path = tmp.path() / "scratch";
    std::ofstream(path) << "left behind";
    ASSERT_TRUE(test::let_others_read(path, 0644));

    auto scratch = File::open_scratch(path);
    ASSERT_TRUE(scratch) << scratch.error().message;
    auto size = scratch.value().size();
    ASSERT_TRUE(size);
    EXPECT_EQ(size.value(), 0u);
    ASSERT_TRUE(scratch.value().close());
    EXPECT_TRUE(test::has_private_mode(path, 0600));
}

#ifdef _WIN32
// A junction or directory link to a private directory is not itself private, and is not made
// private in place of its target.
TEST(File, PrivateDirectoryChecksDoNotFollowLinks) {
    test::TempDir tmp;
    const auto target = tmp.path() / "target";
    const auto link = tmp.path() / "link";
    ASSERT_TRUE(make_private_dir(target));
    std::error_code ec;
    std::filesystem::create_directory_symlink(target, link, ec);
    if (ec) GTEST_SKIP() << "cannot create a directory link: " << ec.message();
    EXPECT_FALSE(is_private_dir(link));
    auto made = make_private_dir(link);
    ASSERT_FALSE(made);
    EXPECT_EQ(made.error().code, Errc::permission);
    EXPECT_TRUE(is_private_dir(target));
}
#endif

}  // namespace
}  // namespace ghostdrive::core
